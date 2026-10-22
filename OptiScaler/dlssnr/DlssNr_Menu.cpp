#include "pch.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNr_LookPresets.h"
#include "DlssNr_SuperiorScaling.h"
#include "DlssNr_SuperiorDiagnostics.h"
#include "DlssNrNative.h"


#include <Config.h>
#include <State.h>
#include <Util.h>
#include <menu/menu_common.h>
#include <menu/MenuPages.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeDriverDx11.h>
#include <native/NativeDriverVk.h>
#include <native/VkPresentBridge.h>
#include <resource_tracking/GenericDepth_Vk.h>
#include <framegen/IFGFeature.h>
#include <native/NativeLowLatency.h>
#include <misc/IdentifyGpu.h>
#include <upscalers/FeatureProvider_Vk.h>
#include <nvapi/fakenvapi.h>

#include <imgui/imgui.h>
#include <imgui/ImGuiNotify.hpp>
#include <shaders/dlssnr/DlssNr_GameScale.h>
#include <shaders/dlssnr/DlssNr_Spatial.h>
#include <shaders/dlssnr/DlssNr_TrimAnchors.h>
#include <shaders/dlssnr/DlssNr_AutoTrimDefault.h>
#include <shaders/dlssnr/DlssNr_FollowGame.h>
#include <shaders/dlssnr/DlssNr_ExposureCalibrate.h>
#include <shaders/dlssnr/DlssNr_ExposureAdapt.h>
#include <shaders/dlssnr/DlssNr_ExposureMeter.h>
#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>
#include "DlssNr_ColourEncodingStatus.h"
#include "DlssNr_GameDefaults.h"
#include "DlssNr_NativeMode.h"
#include "DlssNr_TierPlacement.h"
#include "DlssNr_LutStatus.h"
#include "DlssNrDetailReuseHost.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>

namespace DlssNr
{

// Highlight guard's own ceiling -- independent of the model pass limit, which is an
// unrelated setting that happens to share this file.
static constexpr float MaxHighlightGuard = 8.0f;

// Whether this game has ever offered a Game-exposure value. Shared by the White-point-source
// panel's own "No game exposure available" readout and the Optimized Defaults preset, so the
// two stay in agreement if this definition ever changes (e.g. gains a staleness check).
static bool HaveGameExposure()
{
    return DlssNr::IsRunningVk() ? DlssNr::ExposureOfferedVk()
                                  : DlssNr::GameExposureStatus().everOffered;
}

static void HelpMarker(const char* tip);

// Retry after a failure, on whichever backend failed: Direct3D 12 at once, native Vulkan on its next frame.
static void RetryNr()
{
    DlssNr::RetryAfterFailure();
    DlssNr::RequestRetryVk();
}

// A stretch of the menu that shows status text which comes and goes, or re-wraps as it changes (a warning that appears, a
// line that only shows while something is happening). Without a slot everything below it moves when that happens, and a click
// aimed at a button lands on the next control. The slot takes `lines` lines of height whatever is in it: text drawn between
// Begin and End that needs less is padded to that, more simply grows (rare, and then it is the text that decided).
static float StatusSlotBegin() { return ImGui::GetCursorPosY(); }

static void StatusSlotEnd(float begin, int lines)
{
    const float target = begin + (float) lines * ImGui::GetTextLineHeightWithSpacing();
    const float gap = target - ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;

    if (gap > 0.5f)
        ImGui::Dummy(ImVec2(0.0f, gap));
}

// `*.cube` files in `LUTs` inside the OptiScaler folder (MainDllPath: `OptiScaler` beside the game exe, or
// [Libraries] OptiDllPath -- where the bundled streamline/plugins folders live too), for the LUT combo below.
// Scanned once (first menu render) and on demand (the Rescan button) rather than every frame -- a directory
// listing is not worth paying for on every one of a menu's many redraws, and the folder only changes when the
// user drops a new file in.
static std::filesystem::path LutFolder()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value_or(Util::DllPath().parent_path().wstring())) /
           L"LUTs";
}

static std::vector<std::filesystem::path> ScanLutFolder()
{
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    const std::filesystem::path lutsDir = LutFolder();

    if (!std::filesystem::exists(lutsDir, ec) || ec)
        return found;

    for (const auto& entry : std::filesystem::directory_iterator(lutsDir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;

        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        if (ext == L".cube")
            found.push_back(entry.path());
    }

    std::sort(found.begin(), found.end());
    return found;
}

// Trim multiplies the white point, so a larger Trim darkens the picture NR is shown. The menu shows it in stops
// instead, the other way round (+ = brighter) and centred on each source's own default, which reads as 0 EV.
// The conversions are Tune for this scene's own (DlssNrExposureCalibrate), so the slider and a tuned value can never
// disagree; Tidy shows anything that rounds to zero as +0.0 (a tuned value lands exactly on 0 EV).
static float TrimToEv(float trim, float neutral)
{
    return DlssNrExposureCalibrate::Tidy(DlssNrExposureCalibrate::EvForTrim(trim, neutral));
}

static float EvToTrim(float ev, float neutral) { return DlssNrExposureCalibrate::TrimForEv(ev, neutral); }

// The one "Model input brightness" slider (and its Reset) for an exposure source's Trim. `anchorCount` is how
// many Trim anchors the ini holds for that source: they are ini-only now and take over from the slider, so
// with any present the slider is shown disabled and says why. `detectedDefault` is set for a source whose
// default is decided per game (Automatic, see DlssNr_AutoTrimDefault.h): the slider then shows it while the
// user has set nothing, and Reset goes back to it (the ini key back to auto) rather than to `neutral`.
static void RenderTrimEvSlider(CustomOptional<float>& trim, float neutral, size_t anchorCount, const char* idSuffix,
                               const char* tip, std::optional<float> detectedDefault = std::nullopt)
{
    const float minEv = TrimToEv(DlssNrTrim::kMaxTrim, neutral);
    const float maxEv = TrimToEv(DlssNrTrim::kMinTrim, neutral);
    const float shown = trim.has_value() ? trim.value() : detectedDefault.value_or(trim.value_or_default());
    float ev = std::clamp(TrimToEv(shown, neutral), minEv, maxEv);

    ImGui::BeginDisabled(anchorCount > 0);
    const std::string sliderLabel = std::string("Model input brightness##") + idSuffix;
    if (ImGui::SliderFloat(sliderLabel.c_str(), &ev, minEv, maxEv, "%+.1f EV"))
        trim = EvToTrim(ev, neutral);

    ImGui::SameLine();

    // Deliberately always present rather than greyed at 0 EV: the safe value is one click away.
    const std::string resetLabel = std::string("Reset##") + idSuffix;
    if (ImGui::SmallButton(resetLabel.c_str()))
    {
        if (detectedDefault.has_value())
            trim = std::nullopt;
        else
            trim = neutral;
    }
    ImGui::EndDisabled();

    HelpMarker(tip);

    if (anchorCount > 0)
        ImGui::TextDisabled("%u brightness point(s) are in use (below); the slider has no effect while they exist.",
                            (unsigned int) anchorCount);
}

// The brightness points of one exposure source (the ini's Trim anchors, "base white point:Trim;"): where Tune's results are
// saved. Each is the Model input brightness to use when the scene is as bright as it was at the Tune, blended in between
// and held beyond the first and last. A row per point with its own delete, and one button for them all.
static void RenderBrightnessPoints(CustomOptional<std::string>& table, float neutral, const char* idSuffix)
{
    std::string text = table.value_or_default();
    const auto points = DlssNrTrim::Parse(text);

    if (points.empty())
        return;

    ImGui::Indent();

    if (ImGui::TreeNode((std::string("Brightness points##") + idSuffix).c_str()))
    {
        size_t remove = points.size();

        for (size_t i = 0; i < points.size(); ++i)
        {
            ImGui::Text("Scene brightness %.4g: %+.1f EV", points[i].key, DlssNrExposureCalibrate::Tidy(TrimToEv(points[i].trim, neutral)));
            ImGui::SameLine();

            if (ImGui::SmallButton((std::string("Delete##") + idSuffix + std::to_string(i)).c_str()))
                remove = i;
        }

        if (ImGui::SmallButton((std::string("Clear all points##") + idSuffix).c_str()))
            table = std::string();
        else if (remove < points.size())
        {
            DlssNrTrim::RemovePoint(text, remove);
            table = text;
        }

        HelpMarker("Brightness settings Tune saved for scenes of different brightness. Automatic blends the nearest "
                   "two. Tune once in a bright and once in a dark scene; tuning again at the same brightness "
                   "replaces a point. Up to 8. Re-tune after changing Meter.");
        ImGui::TreePop();
    }

    ImGui::Unindent();
}

// "Tune for this scene" (shaders/dlssnr/DlssNr_ExposureCalibrate.h), under a brightness slider: sweeps it over the scene
// on screen and offers the step where the model's output had the most detail without flicker or clipping. Indented under
// the slider it sets, a SmallButton like the other actions here. D3D12 and Vulkan. `source` is the panel's white point source
// (3 Automatic, 1 Game exposure), `trim` / `neutral` its slider. Named apart from Follow-game's "Re-learn", which is a
// different calibration.
static void RenderTuneForThisScene(uint32_t source, CustomOptional<float>& trim, float neutral,
                                   CustomOptional<std::string>& table)
{
    const auto cal = DlssNr::ExposureCalibration();
    // A result belongs to the panel it was tuned in: its EVs are in that slider's units. A Measure detail run is shown
    // under Compare instead.
    const bool mine = cal.source == source && !cal.measure;
    const bool havePoints = !DlssNrTrim::Parse(table.value_or_default()).empty();
    // Where a result goes: with brightness points the slider is not in force, so it is saved as a point; without any it is
    // applied to the slider, as before, and "Save as point" starts a table.
    static std::string pointNote;
    const auto savePoint = [&](float ev)
    {
        std::string text = table.value_or_default();
        pointNote.clear();

        if (DlssNrTrim::AddPoint(text, cal.anchorKey, EvToTrim(ev, neutral)))
            table = text;
        else
            pointNote = "The table of brightness points is full (8): delete one first.";
    };
    ImGui::Indent();
    // Running and result are different heights, and a run ends under the cursor: three lines are reserved from the moment
    // one starts so the buttons below do not jump when it ends. Idle reserves nothing, so there is no blank stretch.
    const bool tuneBusy = mine && (cal.running || cal.starting || cal.finished);
    const float tuneSlot = StatusSlotBegin();

    if ((cal.running || cal.starting) && mine)
    {
        char text[96];

        if (cal.starting)
            snprintf(text, sizeof(text), "Starting...");
        else if (cal.stepIndex < cal.stepCount && cal.passes > 1)
            snprintf(text, sizeof(text), "Tuning %+.1f EV (%u of %u, pass %u of %u): hold the camera still", cal.stepEv,
                     cal.stepIndex + 1, cal.stepCount, cal.pass + 1, cal.passes);
        else if (cal.stepIndex < cal.stepCount)
            snprintf(text, sizeof(text), "Tuning %+.1f EV (%u of %u): hold the camera still", cal.stepEv,
                     cal.stepIndex + 1, cal.stepCount);
        else
            snprintf(text, sizeof(text), "Reading the results...");

        ImGui::ProgressBar(cal.progress, ImVec2(-FLT_MIN, 0.0f), text);

        if (ImGui::SmallButton("Cancel##tune"))
            DlssNr::CancelExposureCalibration();
    }
    else if (cal.finished && mine)
    {
        // The long results wrap, and their buttons go on the next line: on one unwrapped line OK ran off the panel's
        // right edge, and without it the result could not be dismissed, so Tune could not be run again.
        const auto warning = [](const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.25f, 1.0f));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        };
        bool ownLine = false;

        if (cal.unsure)
        {
            warning("The brightness steps changed NR's detail no more than the measurement varies on its own, so no "
                    "step was clearly better. Your current value is kept.");
            ownLine = true;
        }
        else if (cal.unrepeated)
        {
            char text[192];
            snprintf(text, sizeof(text), "The two passes disagreed (%+.1f EV, then %+.1f EV), so neither is reliable. "
                                         "Your current value is kept. Hold the camera still and try again.",
                     cal.firstPassEv, cal.lastPassEv);
            warning(text);
            ownLine = true;
        }
        else if (cal.changed)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "Best here: %+.1f EV (now %+.1f EV)",
                               cal.resultEv, cal.currentEv);
            ImGui::SameLine();

            if (havePoints)
            {
                if (ImGui::SmallButton("Save as point##tune"))
                {
                    savePoint(cal.resultEv);
                    if (pointNote.empty())
                        DlssNr::DismissExposureCalibration();
                }
            }
            else
            {
                if (ImGui::SmallButton("Apply##tune"))
                {
                    trim = EvToTrim(cal.resultEv, neutral);
                    DlssNr::DismissExposureCalibration();
                }

                ImGui::SameLine();

                if (ImGui::SmallButton("Save as point##tune"))
                {
                    savePoint(cal.resultEv);
                    if (pointNote.empty())
                        DlssNr::DismissExposureCalibration();
                }
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", "Keeps this result for scenes about this bright. Automatic blends between "
                                        "saved points.");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "Best here: %+.1f EV, as it is now.",
                               cal.currentEv);
        }

        if (!pointNote.empty())
            warning(pointNote.c_str());

        if (!ownLine)
            ImGui::SameLine();

        if (ImGui::SmallButton(cal.changed ? "Keep##tune" : "OK##tune"))
            DlssNr::DismissExposureCalibration();

        ImGui::SameLine();
        ImGui::BeginDisabled(!cal.available);

        if (ImGui::SmallButton("Tune again##tune"))
            DlssNr::StartExposureCalibration(source); // clears this result itself

        ImGui::EndDisabled();

        // The curves and the raw measure are for checking the tuning itself, not for choosing.
        if (!cal.ev.empty() && ImGui::TreeNode("Details##tune"))
        {
            const int n = (int) cal.ev.size();
            ImGui::PlotLines("Band-pass##tune", cal.scoreBand.data(), n, 0, nullptr, FLT_MAX, FLT_MAX,
                             ImVec2(0.0f, 50.0f));
            ImGui::PlotLines("Raw##tune", cal.scoreRaw.data(), n, 0, nullptr, FLT_MAX, FLT_MAX,
                             ImVec2(0.0f, 50.0f));
            ImGui::TextDisabled("Score from %+.1f EV (left) to %+.1f EV (right). Best: band-pass %+.1f, raw %+.1f.",
                                cal.ev.front(), cal.ev.back(), cal.bestBandEv, cal.bestRawEv);
            // What the model does to the game's colour and shadows there (output against the game's frame).
            ImGui::TextDisabled("At %+.1f EV: saturation %+.0f%%, warmth %+.3f, shadows %.0f%% %s, %.1f%% crushed.",
                                cal.resultStepEv, 100.0f * cal.resultSaturation, cal.resultWarmth,
                                std::fabs(100.0f * cal.resultShadowDarkening),
                                cal.resultShadowDarkening >= 0.0f ? "darker" : "lifted", 100.0f * cal.resultCrushed);

            // Not when the run was unsure or its passes disagreed: those keep the current value for either measure.
            // Nor when the passes picked different raw bests -- the number shown is the last pass's, and offering it
            // would be a single-pass answer from a run that promised two.
            ImGui::BeginDisabled(cal.unsure || cal.unrepeated || !cal.rawAgreed);

            if (ImGui::SmallButton(havePoints ? "Save raw as point instead##tune" : "Apply raw instead##tune"))
            {
                if (havePoints)
                    savePoint(cal.bestRawEv);
                else
                    trim = EvToTrim(cal.bestRawEv, neutral);

                if (pointNote.empty())
                    DlssNr::DismissExposureCalibration();
            }

            ImGui::EndDisabled();

            ImGui::TreePop();
        }
    }
    else
    {
        ImGui::BeginDisabled(!cal.available);

        if (ImGui::SmallButton("Tune for this scene"))
            DlssNr::StartExposureCalibration(source);

        ImGui::EndDisabled();
        HelpMarker("Finds the Model input brightness that gives the most detail in this scene. Hold the camera still "
                   "for a few seconds; the picture gets brighter and darker while it runs. Nothing changes until you "
                   "press Apply or Save as point.");

        if (!cal.available && !cal.unavailable.empty())
            ImGui::TextDisabled("Not available: %s", cal.unavailable.c_str());

        if (mine && !cal.startError.empty())
            ImGui::TextDisabled("Could not start: %s", cal.startError.c_str());
        else if (mine && !cal.aborted.empty())
        {
            // Wrapped: a stop now says what it measured, and an unwrapped line runs off the panel.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.25f, 1.0f));
            ImGui::TextWrapped("Stopped: %s", cal.aborted.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (tuneBusy)
        StatusSlotEnd(tuneSlot, 3);
    ImGui::Unindent();
}

// "Measure detail" (Story 1 of the input canvas epic): measures NR's output on the scene on screen at the current
// settings -- detail added over the game's frame, and flicker beyond the input's -- for about a second, and shows it
// beside the previous measurement, so any setting can be A/B'd by number. Shares Tune's run (DlssNr_ExposureCalibrate.h,
// MeasureSettings); nothing is dispatched until the button is pressed.
static void RenderMeasureDetail()
{
    const auto cal = DlssNr::ExposureCalibration();

    if (cal.measure && (cal.running || cal.starting))
    {
        ImGui::ProgressBar(cal.progress, ImVec2(-FLT_MIN, 0.0f),
                           cal.starting ? "Starting..." : "Measuring: hold the camera still");

        if (ImGui::SmallButton("Cancel##measure"))
            DlssNr::CancelExposureCalibration();

        return;
    }

    ImGui::BeginDisabled(!cal.measureAvailable);

    if (ImGui::Button("Measure detail"))
        DlssNr::StartMeasureDetail();

    ImGui::EndDisabled();
    HelpMarker("Measures NR against the game's picture for about a second: Detail (added fine detail), Flicker (1x = "
               "none added), Colour, Shadows.\n"
               "To compare: on a still scene press it, change one setting, press again. A few percent is noise.");

    if (!cal.measureAvailable && !cal.measureUnavailable.empty())
        ImGui::TextDisabled("Not available: %s", cal.measureUnavailable.c_str());

    if (cal.measure && !cal.startError.empty())
        ImGui::TextDisabled("Could not start: %s", cal.startError.c_str());
    else if (cal.measure && !cal.aborted.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.25f, 1.0f));
        ImGui::TextWrapped("Stopped: %s", cal.aborted.c_str());
        ImGui::PopStyleColor();
    }

    if (cal.measurements == 0)
        return;

    const auto& m = cal.latest;
    ImGui::Text("Measurement #%u", cal.measurements);
    ImGui::TextWrapped("%s", cal.detailWords.c_str());
    ImGui::TextWrapped("%s", cal.flickerWords.c_str());
    ImGui::TextWrapped("%s", cal.colourWords.c_str());
    ImGui::TextWrapped("%s", cal.shadowWords.c_str());
    ImGui::TextDisabled("detail added %.5f (out %.5f in %.5f), raw %.5f | flicker %.5f (change out %.5f in %.5f) | %u "
                        "frames", m.detail, m.detailOut, m.detailIn, m.raw, m.flicker, m.flickerOut, m.flickerIn,
                        m.frames);

    if (cal.hasPrevious && !cal.comparable)
    {
        ImGui::TextDisabled("vs #%u: not comparable (measured at another exposure: let it settle, or the white point "
                            "source changed)", cal.measurements - 1);
    }
    else if (cal.hasPrevious)
    {
        ImGui::TextWrapped("Against #%u: %s", cal.measurements - 1, cal.compareWords.c_str());
    }
}

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f", bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    if (std::strcmp(label, "Intensity") == 0)
        HelpMarker("Strength of this pass. Higher: stronger effect. Lower: closer to the game's picture. Default: 1. "
                   "Above 1 is experimental.");
    else if (std::strcmp(label, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast. Higher: crisper texture. Lower: smoother. Default: 1. Above 1 is "
                   "experimental.");
    else if (std::strcmp(label, "Local tone") == 0)
        HelpMarker("Broad lighting changes. Higher: stronger relighting. Lower: keeps the game's lighting. Default: "
                   "1 (0 on later passes). Above 1 is experimental.");
    else if (std::strcmp(label, "Skin structure") == 0)
        HelpMarker("Fine detail on skin. Higher: more skin detail. 0: smooth skin. -1: same as Local structure. "
                   "Above 1 is experimental.");
    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt,
                                  const char* const* names, int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

// This fork's starting point for the "Model passes" slider, one per pass count: the count, and NR on, on FP8 and with
// the model applied. How each pass looks is the look presets' (DlssNr_LookPresets.h), so picking a pass count never
// undoes a look and a look never undoes this; the multipass profiles these buttons used to write are the Natural
// look's.
static void ApplyPassPreset(Config* config, unsigned int passes)
{
    config->DlssNrEnabled = true;
    config->DlssNrPrecision = 0u; // NVIDIA (FP8)
    config->DlssNrApplyModel = true;
    config->DlssNrUnlockPasses = false;
    config->DlssNrPasses = passes;

    // Upscale Method, Upscale Mode and Final Image Composition are deliberately not set here: the Pre-SR/Post-SR
    // tiers and the looks set them. Reuse bottleneck is not set here either: it is a speed/quality choice per kernel
    // set, not part of a look, so a preset leaves both checkboxes as the user had them.
    // Exposure back to its default (DlssNr_ExposureDefault.h: Game exposure when the game hands one over, on the scale
    // measured for the game, else Automatic at its default brightness).
    config->DlssNrWhitePointSource = DlssNrExposureDefault::kSourceAuto;
    config->DlssNrAutoExposureTrim = std::nullopt;
    config->DlssNrMaxRatio = 2.0f;               // Highlight guard
}

// Whether a pass preset is in effect, for highlighting its button: its pass count. Derived from the config (not
// remembered), like ResolutionTierActive.
static bool PassPresetActive(Config* config, unsigned int passes)
{
    return config->DlssNrPasses.value_or_default() == passes;
}

// A later pass's override (Pass 2 / Pass 3 keys): absent when none of its keys is set; a key not set takes pass 1's
// value, Local tone 0 (as PassProfiles.h runs it).
template <typename StyleOpt, typename FloatOpt, typename BoolOpt>
static std::optional<DlssNrLook::Profile>
ReadOverride(const StyleOpt& style, const FloatOpt& intensity, const FloatOpt& structure, const FloatOpt& tone,
             const FloatOpt& skin, const BoolOpt& mask, const DlssNrLook::Profile& base)
{
    if (!style.has_value() && !intensity.has_value() && !structure.has_value() && !tone.has_value() &&
        !skin.has_value() && !mask.has_value())
        return std::nullopt;

    return DlssNrLook::Profile { style.has_value() ? style.value() : base.style,
                                 intensity.has_value() ? intensity.value() : base.intensity,
                                 structure.has_value() ? structure.value() : base.structure,
                                 tone.has_value() ? tone.value() : 0.0f,
                                 skin.has_value() ? skin.value() : base.skin,
                                 mask.has_value() ? mask.value() : base.autoMask };
}

template <typename StyleOpt, typename FloatOpt, typename BoolOpt>
static void WriteOverride(StyleOpt& style, FloatOpt& intensity, FloatOpt& structure, FloatOpt& tone, FloatOpt& skin,
                          BoolOpt& mask, const std::optional<DlssNrLook::Profile>& profile)
{
    if (!profile.has_value())
    {
        style = std::optional<uint32_t> {};
        intensity = std::optional<float> {};
        structure = std::optional<float> {};
        tone = std::optional<float> {};
        skin = std::optional<float> {};
        mask = std::optional<bool> {};
        return;
    }

    style = profile->style;
    intensity = profile->intensity;
    structure = profile->structure;
    tone = profile->tone;
    skin = profile->skin;
    mask = profile->autoMask;
}

// What the config has of the settings a look owns
static DlssNrLook::Settings ReadLook(Config* config)
{
    DlssNrLook::Settings s;
    s.pass1 = { config->DlssNrStyle.value_or_default(),          config->DlssNrIntensity.value_or_default(),
                config->DlssNrLocalStructure.value_or_default(), config->DlssNrLocalTone.value_or_default(),
                config->DlssNrSkinStructure.value_or_default(),  config->DlssNrAutoMask.value_or_default() };
    s.pass2 = ReadOverride(config->DlssNrPass2Style, config->DlssNrPass2Intensity, config->DlssNrPass2LocalStructure,
                           config->DlssNrPass2LocalTone, config->DlssNrPass2SkinStructure, config->DlssNrPass2AutoMask,
                           s.pass1);
    s.pass3 = ReadOverride(config->DlssNrPass3Style, config->DlssNrPass3Intensity, config->DlssNrPass3LocalStructure,
                           config->DlssNrPass3LocalTone, config->DlssNrPass3SkinStructure, config->DlssNrPass3AutoMask,
                           s.pass1);

    for (const auto& extra : config->DlssNrExtraPasses)
    {
        if (extra.style.has_value() || extra.intensity.has_value() || extra.structure.has_value() ||
            extra.tone.has_value() || extra.skin.has_value() || extra.autoMask.has_value())
            s.laterOverrides = true;
    }

    s.curve = config->DlssNrReversibleMode.value_or_default();
    s.detail = config->DlssNrTransferStrength.value_or_default();
    s.colour = config->DlssNrColourStrength.value_or_default();
    s.skinSplit = config->DlssNrSkinProtection.value_or_default();
    s.skinTone = config->DlssNrSkinToneEnabled.value_or_default();
    s.skinDetail = config->DlssNrSkinDetail.value_or_default();
    s.skinColour = config->DlssNrSkinColour.value_or_default();
    s.envDetail = config->DlssNrEnvironmentDetail.value_or_default();
    s.envColour = config->DlssNrEnvironmentColour.value_or_default();
    s.inputs = config->DlssNrInputStyles();
    return s;
}

// A look: every setting it owns, the ones it does not mention back to their defaults
static void WriteLook(Config* config, const DlssNrLook::Settings& s)
{
    config->DlssNrStyle = s.pass1.style;
    config->DlssNrIntensity = s.pass1.intensity;
    config->DlssNrLocalStructure = s.pass1.structure;
    config->DlssNrLocalTone = s.pass1.tone;
    config->DlssNrSkinStructure = s.pass1.skin;
    config->DlssNrAutoMask = s.pass1.autoMask;
    WriteOverride(config->DlssNrPass2Style, config->DlssNrPass2Intensity, config->DlssNrPass2LocalStructure,
                  config->DlssNrPass2LocalTone, config->DlssNrPass2SkinStructure, config->DlssNrPass2AutoMask, s.pass2);
    WriteOverride(config->DlssNrPass3Style, config->DlssNrPass3Intensity, config->DlssNrPass3LocalStructure,
                  config->DlssNrPass3LocalTone, config->DlssNrPass3SkinStructure, config->DlssNrPass3AutoMask, s.pass3);

    for (auto& extra : config->DlssNrExtraPasses)
        WriteOverride(extra.style, extra.intensity, extra.structure, extra.tone, extra.skin, extra.autoMask,
                      std::nullopt);

    config->DlssNrReversibleMode = s.curve;
    config->DlssNrTransferStrength = s.detail;
    config->DlssNrColourStrength = s.colour;
    config->DlssNrSkinProtection = s.skinSplit;
    config->DlssNrSkinToneEnabled = s.skinTone;
    config->DlssNrSkinDetail = s.skinDetail;
    config->DlssNrSkinColour = s.skinColour;
    config->DlssNrEnvironmentDetail = s.envDetail;
    config->DlssNrEnvironmentColour = s.envColour;
    config->SetDlssNrInputStyles(s.inputs);
}

// "##lookbase": Natural and Cinematic are also look buttons on the same page, and ImGui takes the label as the ID
static constexpr const char* LookBaseNames[] = { "Standard##lookbase", "Natural##lookbase", "Cinematic##lookbase" };
static_assert(IM_ARRAYSIZE(LookBaseNames) == DlssNrLook::kBaseCount);

// A button drawn green while its preset is the one in effect (the overlay's existing success green). `width` 0: as wide
// as its label.
static bool PresetButton(const char* label, bool active, float width = 0.0f)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.65f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.45f, 0.20f, 1.0f));
    }

    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));

    if (active)
        ImGui::PopStyleColor(3);

    return pressed;
}

static constexpr const char* PassPresetNames[] = { "1 Pass", "2 Pass", "3 Pass" };

// Quality tiers (High/Medium/Low/Potato) for running NR at a reduced model resolution, offered as
// two preset rows that differ only in where NR runs: Pre-SR (Before Super Resolution) and Post-SR
// (After Super Resolution). Each touches only Upscale Mode, Upscale Method and Model resolution, plus Auto model
// resolution -- turned off because Auto overrides the Model resolution slider when NR runs
// post-SR, which would make the tier's resolution a silent no-op. Each also sets "NR Pass at:",
// which includes turning off Generate-before-SR/apply-after-SR (DLSS): that mode generates pre-SR
// and disables the placement choice, so leaving it on would contradict the row's name. All four run composed: a
// Replace composition goes back to composed (DlssNrLook::TierCurve), and a composed curve a look chose stays, so a
// look and a tier never undo each other.
struct ResolutionTier
{
    const char* name;
    uint32_t upscaleMethod;      // 0 Bilinear, 1 SGSR1
    float workingScale;          // model resolution as a fraction (1.0 = 100%)
};

static constexpr ResolutionTier ResolutionTiers[] = {
    { "High", 1u, 1.00f },
    { "Medium", 1u, 0.80f },
    { "Low", 0u, 0.65f },
    { "Potato", 0u, 0.50f },
};

// One width for every preset button on the status page (looks, pass presets, both tier rows): the widest label's, so
// the rows line up as columns instead of each button hugging its own label. From the current font, so it follows the
// menu's scale.
static float PresetButtonWidth()
{
    float widest = 0.0f;

    for (const auto& look : DlssNrLook::kLooks)
        widest = std::max(widest, ImGui::CalcTextSize(look.name).x);

    for (const char* name : PassPresetNames)
        widest = std::max(widest, ImGui::CalcTextSize(name).x);

    for (const auto& tier : ResolutionTiers)
        widest = std::max(widest, ImGui::CalcTextSize(tier.name).x);

    return widest + 2.0f * ImGui::GetStyle().FramePadding.x;
}

static void ApplyResolutionTier(Config* config, int& pendingScale, const ResolutionTier& tier,
                                bool beforeSuperResolution)
{
    // With Finished Picture on (NR Only mode) the placement stays: the tier is about the model's size, and clearing it
    // would switch NR Only off (DlssNr_TierPlacement.h).
    const auto placement = DlssNrTierPlacement::AfterTier(
        { config->DlssNrFinishedPicture.value_or_default(), config->DlssNrRunBeforeSr.value_or_default(),
          config->DlssNrDeferredDlss.value_or_default() },
        beforeSuperResolution);

    // Same rule as the "NR Pass at:" combo: leaving Finished Picture clears a session failure.
    if (config->DlssNrFinishedPicture.value_or_default() && !placement.finishedPicture)
        RetryNr();
    config->DlssNrFinishedPicture = placement.finishedPicture;
    config->DlssNrRunBeforeSr = placement.runBeforeSr;
    config->DlssNrDeferredDlss = placement.deferred;

    // Upscale Mode: NVIDIA residual. A Superior Scaling choice is kept: it is an enlargement of its own, not a tier
    // setting, and the tier still sets the Model resolution it works at.
    if (!DlssNr::SuperiorScaling::Enabled(*config))
        config->DlssNrTransfer = 2u;
    config->DlssNrReducedUpscaleMethod = tier.upscaleMethod;
    config->DlssNrModelResolutionAuto = false;
    config->DlssNrWorkingScale = tier.workingScale;
    pendingScale = -1; // clear any in-flight drag
    config->DlssNrReversibleMode = DlssNrLook::TierCurve(config->DlssNrReversibleMode.value_or_default());
}

// Whether a tier's settings are what is currently in effect, for highlighting its button. Derived
// from the config rather than remembered, so it can never claim a preset the settings have since
// drifted from, and it survives a restart. The curve is left out: it is the look's (a look picked after a tier keeps
// the tier green). Tiers have distinct resolutions, so one tier at most matches. Its two rows differ in placement, which
// tells them apart, except with Finished Picture on: placement does not apply there and both rows of that tier match
// (DlssNr_TierPlacement.h).
static bool ResolutionTierActive(Config* config, const ResolutionTier& tier, bool beforeSuperResolution)
{
    return DlssNrTierPlacement::RowMatchesPlacement(
               { config->DlssNrFinishedPicture.value_or_default(), config->DlssNrRunBeforeSr.value_or_default(),
                 config->DlssNrDeferredDlss.value_or_default() },
               beforeSuperResolution) &&
           (config->DlssNrTransfer.value_or_default() == 2u || DlssNr::SuperiorScaling::Enabled(*config)) &&
           config->DlssNrReducedUpscaleMethod.value_or_default() == tier.upscaleMethod &&
           !config->DlssNrModelResolutionAuto.value_or_default() &&
           std::fabs(config->DlssNrWorkingScale.value_or_default() - tier.workingScale) < 0.005f;
}

// Which API's Optical F5Low drivers and depth finder this game uses: a game presents with one of them. A dxvk game
// presents its D3D frames through Vulkan; by default its D3D drivers are still the ones that run (NativeDriverDx11.h),
// unless the experimental DlssNrNativeDxvkVulkan key asks for dxvk's own Vulkan calls instead.
enum class NativeApi
{
    Dx12,
    Dx11,
    Vulkan
};

static NativeApi CurrentNativeApi()
{
    const auto& state = State::Instance();

    if (VkPresentBridge::DxvkThroughVulkan())
        return NativeApi::Vulkan;

    const auto present = state.swapchainApi == API::Vulkan  ? DlssNrNativeMode::PresentApi::Vulkan
                         : state.swapchainApi == API::DX12 ? DlssNrNativeMode::PresentApi::Dx12
                         : state.swapchainApi == API::DX11 ? DlssNrNativeMode::PresentApi::Dx11
                                                           : DlssNrNativeMode::PresentApi::NotSelected;
    const auto interop = state.swapchainInteropApi == SwapchainInteropApi::VkwDx12
                             ? DlssNrNativeMode::Interop::VkwDx12
                         : state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12
                             ? DlssNrNativeMode::Interop::Dx11wDx12
                             : DlssNrNativeMode::Interop::None;

    switch (DlssNrNativeMode::GameApiFor(present, interop, state.currentD3D11Device != nullptr))
    {
    case DlssNrNativeMode::GameApi::Vulkan:
        return NativeApi::Vulkan;
    case DlssNrNativeMode::GameApi::Dx11:
        return NativeApi::Dx11;
    default:
        return NativeApi::Dx12;
    }
}

static bool NativeGameCallsUpscaler(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        return GenericDepthDx11::GameCallsUpscaler();
    case NativeApi::Vulkan:
        return GenericDepthVk::GameCallsUpscaler();
    default:
        return GenericDepthDx12::GameCallsUpscaler();
    }
}

static DlssNrNativeMode::Finder NativeFinder(NativeApi api, bool depthWanted)
{
    using DlssNrNativeMode::FinderFor;

    switch (api)
    {
    case NativeApi::Dx11:
        return FinderFor(depthWanted, GenericDepthDx11::Installed(), GenericDepthDx11::InstallFailed());
    case NativeApi::Vulkan:
        return FinderFor(depthWanted, GenericDepthVk::Installed(), GenericDepthVk::InstallFailed());
    default:
        return FinderFor(depthWanted, GenericDepthDx12::Installed(), GenericDepthDx12::InstallFailed());
    }
}

static void NativeDepthStatus(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        GenericDepthDx11::DrawStatus();
        break;
    case NativeApi::Vulkan:
        GenericDepthVk::DrawStatus();
        break;
    default:
        GenericDepthDx12::DrawStatus();
        break;
    }
}

static void NativeMotionStatus(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        NativeMotionDx11::DrawStatus();
        break;
    case NativeApi::Vulkan:
        NativeMotionVk::DrawStatus();
        break;
    default:
        NativeMotionDx12::DrawStatus();
        break;
    }
}

static bool NativeNrOnlyRunning(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        return NativeMotionDx11::NrOnlyRunning();
    case NativeApi::Vulkan:
        return NativeMotionVk::NrOnlyRunning();
    default:
        return NativeMotionDx12::NrOnlyRunning();
    }
}

// "FG only (game's upscaler)": frame generation is fed from a Vulkan-on-D3D12 backend's evaluate
// (upscalers/IFeature_VkwDx12.cpp), so the game's Vulkan upscaler call has to run through one. (Such a feature reports
// API::DX12: its upscaler type is what says so.)
static bool VulkanFeatureIsOn12()
{
    const auto feature = State::Instance().currentFeature;
    return feature != nullptr && FeatureProvider_Vk::IsOn12(feature->GetUpscalerType());
}

// The backend frame generation only runs the game's upscaler on, and the one to go back to when the mode is left.
static std::optional<Upscaler> g_backendBeforeFrameGenerationOnly;

static void SwitchVulkanBackend(Config* config, Upscaler backend)
{
    auto& state = State::Instance();
    config->VulkanUpscaler = backend;
    state.newBackend = backend;

    // A live feature is rebuilt on the game's next evaluate (inputs/NVNGX_DLSS_Vk.cpp); a later one is made with it.
    for (auto& changeBackend : state.changeBackend)
        changeBackend.second = true;
}

static void EnterFrameGenerationOnly(Config* config)
{
    const Upscaler current = config->VulkanUpscaler.value_or_default();

    // The same DLSS on D3D12 where the game's DLSS could run, FSR otherwise (FeatureProvider_Vk::On12For). Already a
    // D3D12 one: rebuilt all the same, so it is made on the bridge's device (IFeature_VkwDx12::CreateDx12Device).
    const Upscaler target = FeatureProvider_Vk::On12For(current);

    if (target != current)
        g_backendBeforeFrameGenerationOnly = current;

    SwitchVulkanBackend(config, target);
}

static void LeaveFrameGenerationOnly(Config* config)
{
    if (!g_backendBeforeFrameGenerationOnly.has_value())
        return;

    SwitchVulkanBackend(config, g_backendBeforeFrameGenerationOnly.value());
    g_backendBeforeFrameGenerationOnly.reset();
}

static void RenderNativeMode(Config* config, bool nrEnabled, bool& finishedPicture)
{
    using namespace DlssNrNativeMode;

    const NativeApi api = CurrentNativeApi();
    const bool gameUpscaler = NativeGameCallsUpscaler(api);
    const Shown shown =
        FromKeys({ config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
                   config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });

    struct ModeChoice
    {
        Mode mode;
        Shown shown;
        const char* label;
    };

    static constexpr ModeChoice kModes[] = {
        { Mode::Off, Shown::Off, "Off##nativemode" },
        { Mode::NrOnly, Shown::NrOnly, "NR only##nativemode" },
        { Mode::NrAndFrameGeneration, Shown::NrAndFrameGeneration, "NR + frame generation##nativemode" },
        { Mode::FrameGenerationOnly, Shown::FrameGenerationOnly, "FG only (game's upscaler)##nativemode" },
    };

    // FG only feeds frame generation from the game's Vulkan upscaler call through a Vulkan-on-D3D12 backend; a dxvk
    // game's upscaler calls are D3D11 ones, so it is not offered there.
    const bool vulkan = api == NativeApi::Vulkan && !VkPresentBridge::DxvkGame();
    const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted("Mode:");

    for (const auto& choice : kModes)
    {
        if (!Offered(choice.mode, vulkan))
            continue;

        const float width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                            ImGui::CalcTextSize(choice.label, nullptr, true).x;
        ImGui::SameLine();

        if (ImGui::GetCursorScreenPos().x + width > rowRight)
            ImGui::NewLine();

        const bool selectable = Selectable(choice.mode, gameUpscaler, vulkan);
        ImGui::BeginDisabled(!selectable);
        const bool clicked = ImGui::RadioButton(choice.label, shown == choice.shown);
        ImGui::EndDisabled();

        if (clicked && selectable)
        {
            const Change change = ForClick(choice.mode, shown, finishedPicture);

            if (change.keys.has_value())
            {
                config->DlssNrNativeDepthFinder = change.keys->depthFinder;
                config->DlssNrNativeMotion = change.keys->motion;
                config->DlssNrNativeInput = change.keys->input;
                config->DlssNrNativeUpscaler = change.keys->upscaler;
                config->DlssNrNativeFrameGenerationOnly = change.keys->frameGenerationOnly;

                if (change.keys->frameGenerationOnly)
                    EnterFrameGenerationOnly(config);
                else if (shown == Shown::FrameGenerationOnly)
                    LeaveFrameGenerationOnly(config);
            }

            if (change.retryAfterFailure)
                RetryNr();

            if (change.finishedPicture.has_value())
            {
                finishedPicture = change.finishedPicture.value();
                config->DlssNrFinishedPicture = finishedPicture;
            }
        }
    }

    HelpMarker("For games that don't call an upscaler: Optical F5Low works out the motion itself so NR can run.\n"
               "NR only: NR on the finished picture. Cheapest; no frame generation.\n"
               "NR + frame generation: adds OptiScaler's upscaler, which gives frame generation its input; needed for "
               "frame generation. It does not process the picture unless you tick Run the upscaler on the picture.\n"
               "FG only (Vulkan): frame generation for Vulkan games that call DLSS, FSR or XeSS themselves.\n"
               "Needs Enable Neural Rendering. On Vulkan, set up frame generation before the game starts.");

    if (shown == Shown::Off)
    {
        if (gameUpscaler)
            ImGui::TextWrapped(vulkan ? "The game is calling an upscaler of its own, so NR runs on that call. For "
                                        "frame generation, choose FG only (game's upscaler)."
                                      : "The game is calling an upscaler of its own, so NR runs on that call; this "
                                        "is not needed.");

        return;
    }

    // The backend's own pass over the picture (NativeUpscalerPass), with NR on or off. Off (the default) it only gives frame
    // generation its input.
    if (shown == Shown::NrAndFrameGeneration)
    {
        bool pass = config->DlssNrNativeUpscalerPass.value_or_default();

        if (ImGui::Checkbox("Run the upscaler on the picture##nativeupscalerpass", &pass))
            config->DlssNrNativeUpscalerPass = pass;

        HelpMarker("Runs the chosen upscaler (DLSS, FSR or XeSS) over the game's picture at the same size. It adds no "
                   "detail, but can steady shimmer in games with weak anti-aliasing; it can also soften the picture "
                   "or leave trails where the estimated motion is wrong. Costs about 1.5-3 ms. Off: the upscaler only "
                   "gives frame generation its input. Works with NR on or off.");
    }

    const bool depthWanted = config->DlssNrNativeDepthFinder.value_or_default();
    const Finder finder = NativeFinder(api, depthWanted);
    const bool depthRestart = DepthRestartWarning(shown, finder);
    // A dxvk game reporting NativeApi::Vulkan (NativeDxvkVulkan) gets its frame generation from the same present bridge as
    // a native Vulkan game, on dxvk's own Vulkan swapchain.
    const bool vulkanNoBridge = api == NativeApi::Vulkan && !VkPresentBridge::IsUp();
    const Warning warning =
        WarningFor(shown, nrEnabled, finishedPicture, gameUpscaler, vulkanNoBridge, VulkanFeatureIsOn12());
    const char* warningText = nullptr;

    switch (warning)
    {
    case Warning::GameUpscaler:
        warningText = "The game is calling an upscaler of its own, so this stands aside and NR runs on that call. "
                      "Choose Off, or turn the game's upscaler off to use this.";
        break;
    case Warning::VulkanNeedsRestart:
        warningText = "Waiting for the game to make a new swapchain for frame generation (it was told to). If nothing "
                      "changes, switch the game's window mode or resolution once; if it still does not start, the Frame "
                      "Generation input (OptiFG, Upscaler) and output were not set when the game started: set them, "
                      "save the settings and restart the game.";
        break;
    case Warning::NeedsGameUpscaler:
        warningText = "The game is not calling an upscaler now: turn DLSS, FSR or XeSS on in the game's settings. (In "
                      "its menus a game often makes no upscaler call; frame generation starts in play.)";
        break;
    case Warning::NeedsOn12Backend:
        warningText = "Switching the game's upscaler to its D3D12 copy (DLSS or FSR on D3D12), which feeds frame "
                      "generation; it is rebuilt on the game's next upscaler call.";
        break;
    case Warning::NrDisabled:
        warningText = shown == Shown::NrOnly ? "Enable Neural Rendering (above) is off, so NR does not run."
                                             : "Enable Neural Rendering (above) is off: frame generation can still "
                                               "use this, but NR does not run.";
        break;
    case Warning::NeedsFinishedPicture:
        warningText = "NR only needs NR Pass at: Finished Picture (under NR Options). Click NR only again to set it.";
        break;
    case Warning::None:
        break;
    }

    // Enough for a wrapped warning of each kind; the usual two status lines leave the rest blank.
    const float slot = StatusSlotBegin();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.25f, 1.0f));

    if (warningText != nullptr)
        ImGui::TextWrapped("%s", warningText);

    if (depthRestart)
        ImGui::TextWrapped("Save the settings and restart the game to use its depth (NR runs on motion only until "
                           "then).");

    ImGui::PopStyleColor();

    if (shown == Shown::FrameGenerationOnly)
    {
        if (warning == Warning::None)
        {
            const auto feature = State::Instance().currentFeature;
            ImGui::TextDisabled("Frame generation from the game's upscaler (%s on D3D12).",
                                feature != nullptr ? feature->Name().c_str() : "upscaler");
        }

        StatusSlotEnd(slot, 4);
        return;
    }

    if (!depthRestart)
        NativeDepthStatus(api);

    if (shown == Shown::MotionOnly)
        ImGui::TextDisabled("Estimating motion only; nothing uses it.");
    else if (shown == Shown::NrAndFrameGeneration || warning == Warning::None)
        NativeMotionStatus(api);

    StatusSlotEnd(slot, 4);
}

// Model-resolution drag in flight, shared by the tier presets (which clear it) and the Model resolution slider.
static int pendingScale = -1;

// What every page needs. Read after the Enable checkbox so a toggle shows up the same frame.
struct NrCommon
{
    bool enabled;
    // Read early (checkbox itself is drawn down in NR Options) so the running-status block
    // can already report finished-picture-specific text.
    bool finishedPicture;
    // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
    // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
    // that is demonstrably running.
    bool vulkan;
};

static NrCommon ReadNrCommon(Config* config)
{
    return { config->DlssNrEnabled.value_or_default(), config->DlssNrFinishedPicture.value_or_default(),
             DlssNr::IsRunningVk() };
}

static void RenderEnableToggle(Config* config)
{
    bool enabled = config->DlssNrEnabled.value_or_default();
    if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
        config->DlssNrEnabled = enabled;

    HelpMarker("Turns DLSS Neural Rendering on. Needs nvngx_dlssnr.dll and the included nvngx.dll_dlssnr.dll.");
}

// The full running-status block, for the Status & Presets page.
static void RenderRunningStatus(Config* config, const NrCommon& nr)
{
    const bool enabled = nr.enabled;
    const bool finishedPicture = nr.finishedPicture;
    const bool vulkan = nr.vulkan;

    // Turning the pass off does not release the model, so the feature handle stays alive and
    // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
    // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
    // moment it describes the frame before last.
    if (!enabled)
    {
        ImGui::TextDisabled("NR off.");
    }
    else if (!DlssNr::IsRunning() && !vulkan)
    {
        const auto feature = State::Instance().currentFeature;
        const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

        if (reason[0] != 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
            ImGui::SameLine();

            if (nativeVk && !DlssNr::RetryableVk())
                ImGui::TextUnformatted("A retry cannot fix this; restart the game.");
            else if (ImGui::SmallButton("Retry"))
                RetryNr();
        }
        else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
        {
            ImGui::TextWrapped("NR needs the D3D12 bridge on D3D11. Choose an upscaler marked w/Dx12 and restart.");
        }
        else if (nativeVk && config->DlssNrDeferredDlss.value_or_default())
        {
            ImGui::TextWrapped("Disable Generate before SR, apply after SR (DLSS) to use native Vulkan NR.");
        }
        else if (enabled && finishedPicture)
        {
            // On the finished picture (Optical F5Low's NR only among them) there is no upscaler to wait for: what NR
            // says about the picture is the reason it has not started (only the log had it).
            const std::string status = DlssNr::FinishedPictureStatus();
            ImGui::TextWrapped("%s", status.empty() ? "Waiting for a finished picture." : status.c_str());
        }
        else if (enabled)
            ImGui::TextUnformatted("Waiting for the upscaler to run.");
    }
    else
    {
        // The elapsed time belongs here rather than only in the upscaler's breakdown: that tooltip needs
        // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
        // nothing in it to hang this off.
        // Either backend's timer. They measure the same thing by different means, and only one
        // of them is running.
        const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

        // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
        // a frozen frame) -- it only outputs the clean frame.
        // Enable Neural Rendering off stops the work.
        const char* runSuffix =
            !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

        // With Reuse detail between frames, full and reused frames alternate, so one reading is either the heavy or
        // the light one; the average over the recent frames is the real per-frame cost.
        const auto detailReuse = !config->DlssNrDetailReuse.value_or_default() ? DlssNr::DetailReuseInfo {}
                                 : vulkan                                     ? DlssNr::DetailReuseStatusVk()
                                                                              : DlssNr::DetailReuseStatus();
        {
            ScopedMonoFont monoFont {};

            if (detailReuse.active && detailReuse.averageMs > 0.0)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   "Running - %.2f ms elapsed per frame on average (%.2f to %.2f)%s",
                                   detailReuse.averageMs, detailReuse.lightMs, detailReuse.heavyMs, runSuffix);
            else if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms elapsed%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.%s", runSuffix);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", "NR's GPU time per frame, including waits for other GPU work. Check FPS for the "
                                    "real cost.");
        if (finishedPicture)
            ImGui::TextDisabled("Includes time shared with other GPU work.");

        if (!vulkan && DlssNr::BackendName()[0] != 0)
            ImGui::TextDisabled("Model backend: %s", DlssNr::BackendName());
    }
}

// One line of the same status, for the other pages.
static void RenderStatusLine(const NrCommon& nr)
{
    const auto ms = nr.vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();
    ScopedMonoFont monoFont {};

    if (!nr.enabled)
        ImGui::TextDisabled("NR off.");
    else if (!DlssNr::IsRunning() && !nr.vulkan)
        ImGui::TextDisabled("NR is not running. See Status & Presets.");
    else if (ms.has_value())
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms elapsed",
                           nr.vulkan ? " natively on Vulkan" : "", ms.value());
    else
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.");
}

// The mode in effect, as the Optical F5Low page and the summary line name it.
static const char* NativeModeName(DlssNrNativeMode::Shown shown)
{
    switch (shown)
    {
    case DlssNrNativeMode::Shown::NrOnly:
        return "NR only";
    case DlssNrNativeMode::Shown::NrAndFrameGeneration:
        return "NR + frame generation";
    case DlssNrNativeMode::Shown::MotionOnly:
        return "motion only";
    case DlssNrNativeMode::Shown::FrameGenerationOnly:
        return "FG only (game's upscaler)";
    default:
        return "Off";
    }
}

// Compress screen edges (shaders/dlssnr/DlssNr_Spatial.h): what the backend that is running last published, and the
// words for it. The runtime only publishes a status code; every word is here. Grey is "not running / not available",
// green is "on", amber is "not applied" or "turned itself off" -- never red, because NR itself is still running.
static DlssNr::Spatial::Published EdgeCompressionNow(const NrCommon& nr)
{
    return nr.vulkan ? DlssNr::EdgeCompressionStatusVk() : DlssNr::EdgeCompressionStatus();
}

// The model resolution NR applies, as a fraction: what the layout was built for when a frame has run (Auto included),
// else the slider.
static float EdgeCompressionScale(const Config* config, const NrCommon& nr)
{
    const auto published = EdgeCompressionNow(nr);

    if (published.nativeW != 0 && std::isfinite(published.scale) && published.scale > 0.0f)
        return published.scale;

    return config->DlssNrWorkingScale.value_or_default();
}

struct EdgeCompressionLine
{
    enum class Tone { Grey, Green, Amber } tone = Tone::Grey;
    std::string text;
};

static EdgeCompressionLine EdgeCompressionText(const Config* config, const NrCommon& nr)
{
    using DlssNr::Spatial::Status;
    EdgeCompressionLine line;

    if (!nr.enabled)
    {
        line.text = "NR is off.";
        return line;
    }

    const auto published = EdgeCompressionNow(nr);
    char buffer[256];

    switch (published.status)
    {
    case Status::Off:
        line.text = "Waiting for NR to run a frame.";
        break;
    case Status::Proxy:
        line.text = "Not available with the driver-proxy backend.";
        break;
    case Status::Active:
    {
        const double share = 100.0 * published.modelW * published.modelH /
                             std::max(1.0, (double) published.ordinaryW * (double) published.ordinaryH);
        std::snprintf(buffer, sizeof(buffer), "NR works on %ux%u instead of %ux%u (%.0f%% of the pixels).",
                      published.modelW, published.modelH, published.ordinaryW, published.ordinaryH, share);
        line.tone = EdgeCompressionLine::Tone::Green;
        line.text = buffer;
        break;
    }
    case Status::BadSettings:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Not applied: a layout value is out of range. Pick a layout, or press Reset on the advanced values.";
        break;
    case Status::TooSmall:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Not applied: NR would work on under a quarter of the picture. Raise Model resolution or the NR "
                    "picture size.";
        break;
    case Status::NothingToCompress:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Not applied: the NR picture is the whole frame in both directions, so there is nothing to compress.";
        break;
    case Status::ThinEdge:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Not applied: the edges would be left with less than a pixel. Make the sharp middle smaller or the "
                    "NR picture larger.";
        break;
    case Status::TurnedOffResources:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Turned itself off: its shader or textures could not be created. NR carries on without it.";
        break;
    case Status::TurnedOffDispatch:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Turned itself off: one of its steps failed on this GPU. NR carries on without it; changing the "
                    "layout tries again.";
        break;
    case Status::TurnedOffModel:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "Turned itself off: NR would not take the packed picture. NR carries on without it; changing the "
                    "layout tries again.";
        break;
    }

    // Asked for and nothing published yet is "waiting"; anything published while it is off in the settings is stale.
    if (!config->DlssNrSpatialCompression.value_or_default())
    {
        line.tone = EdgeCompressionLine::Tone::Grey;
        line.text = "Off.";
    }

    return line;
}

static void EdgeCompressionDrawText(const EdgeCompressionLine& line)
{
    using Tone = EdgeCompressionLine::Tone;
    const ImVec4 colour = line.tone == Tone::Green   ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                          : line.tone == Tone::Amber ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                                     : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
}

// "Reuse detail" on the NR Options page: off, every other frame or every 3rd frame (DetailReuse, DetailReuseInterval),
// and the spread of the model's frames. The Optical F5Low page shows the state and links here. The edit cache
// (EditCache) is not offered here: it is set in the ini only, and shown here when it is. Returns whether reuse is on
// (its fine-tuning follows on the page).
static bool RenderDetailReuseToggle(Config* config)
{
    bool detailReuse = config->DlssNrDetailReuse.value_or_default();
    const unsigned int interval = std::clamp(config->DlssNrDetailReuseInterval.value_or_default(), 2u, 3u);
    int choice = !detailReuse ? 0 : interval == 3 ? 2 : 1;
    const char* choices[] = { "Off", "Every other frame", "Every 3rd frame" };
    if (ImGui::Combo("Reuse detail (experimental)", &choice, choices, IM_ARRAYSIZE(choices)))
    {
        detailReuse = choice != 0;
        config->DlssNrDetailReuse = detailReuse;
        if (choice != 0)
            config->DlssNrDetailReuseInterval = choice == 2 ? 3u : 2u;
    }
    HelpMarker("Runs the model on fewer frames and moves its last detail onto the frames between, for a lower NR GPU "
               "cost: half the frames run the model at every other frame, a third at every 3rd frame (detail trails a "
               "little more on motion). Detail can pop where things move or appear; best with 1 pass. Not with NR "
               "before SR. Fine-tune under Debug.");

    if (detailReuse)
    {
        // 0 = Auto: none at every other frame (as it shipped), over 3 at every 3rd frame (DlssNrDetailReuse::SpreadFor)
        // Never longer than the interval (DlssNrDetailReuse::SpreadFor): no "over 3" at every other frame
        const unsigned int every = choice == 2 ? 3u : 2u;
        int spread = (int) std::min(config->DlssNrDetailReuseSpread.value_or_default(), every);
        char autoLabel[48];
        const unsigned int autoSpread = DlssNrDetailReuse::SpreadFor(every, 0);
        std::snprintf(autoLabel, sizeof(autoLabel), autoSpread > 1 ? "Auto (over %u frames)" : "Auto (off)",
                      autoSpread);
        const char* spreads[] = { autoLabel, "Off", "Over 2 frames", "Over 3 frames" };
        if (ImGui::Combo("Smooth the model's frames", &spread, spreads, (int) every + 1))
            config->DlssNrDetailReuseSpread = (uint32_t) spread;
        HelpMarker("Each time the model runs, its new detail differs a little from the moved old one, and the light "
                   "can pulse at that rate (more at every 3rd frame). Smoothed, the model's frame and the frames after "
                   "it blend from what was shown toward the new detail. Only what is shown is blended, so trails do "
                   "not build up.");
    }

    // The edit cache: ini only (EditCache=true), not offered here
    if (config->DlssNrEditCache.value_or_default())
    {
        const DlssNr::EditCacheInfo status = DlssNr::EditCacheStatus();
        if (status.running)
            ImGui::TextDisabled("Edit cache (set in the ini): on, the model ran on %.0f%% of frames",
                                status.modelShare * 100.0f);
        else if (status.standsAside != nullptr)
            ImGui::TextDisabled("Edit cache (set in the ini): every frame runs the model (%s)", status.standsAside);
        else
            ImGui::TextDisabled("Edit cache (set in the ini): not running");
    }
    return detailReuse;
}

// Scene cuts the game does not flag (shaders/dlssnr/DlssNr_SceneCut.inl), on the NR Options page. The detector runs from
// SceneCut=1, where it only counts; the checkbox is 2, where it also acts. 0 (nothing runs) is set in the ini.
static void RenderSceneCutToggle(Config* config)
{
    if (DlssNr::IsRunningVk())
    {
        ImGui::TextDisabled("Scene cuts the game does not flag: D3D12 games only");
        return;
    }

    const uint32_t setting = config->DlssNrSceneCut.value_or_default();
    bool act = setting >= 2;

    ImGui::BeginDisabled(setting == 0);
    if (ImGui::Checkbox("Reset NR on scene cuts the game does not flag (experimental)", &act))
        config->DlssNrSceneCut = act ? 2u : 1u;
    ImGui::EndDisabled();

    HelpMarker("Restarts NR on camera cuts the game doesn't report, so the old scene doesn't fade through. Off: cuts "
               "are only counted below, to show whether this game needs it. D3D12 only.");

    const DlssNr::SceneCutInfo status = DlssNr::SceneCutStatus();

    if (setting == 0)
        ImGui::TextDisabled("Scene cuts: off (SceneCut=0)");
    else if (status.failed)
        ImGui::TextDisabled("Scene cuts: the detector could not start (see the log)");
    else if (status.found == 0)
        ImGui::TextDisabled(status.running ? "Scene cuts: none found yet" : "Scene cuts: not running on this input");
    else if (act)
        ImGui::TextDisabled("Scene cuts: %llu found, %llu flagged by the game, %llu reset by NR", status.found,
                            status.flagged, status.resets);
    else
        ImGui::TextDisabled("Scene cuts: %llu found, %llu flagged by the game, %llu not", status.found, status.flagged,
                            status.silent);
}

// Optical F5Low's own Reflex calls (native/NativeLowLatency.h): only the state here; the Input and Output choices, Boost
// and the details are on the Framerate page. The status is the one that page shows (native::lowlatency::ReasonText).
static void RenderLowLatencySection()
{
    const auto status = native::lowlatency::GetStatus();

    ImGui::SeparatorText("Low latency");
    ImGui::TextWrapped("Low latency: %s, %s", native::lowlatency::StatusLabel(status.decision),
                       native::lowlatency::ReasonText(status));
    HelpMarker("Keeps the game's render queue short. Which method and who makes the calls is set on the Framerate page.");

    if (ImGui::SmallButton("Set on the Framerate page##f5lowlatency"))
        MenuPages::RequestPage(MenuPages::Page::Framerate);
}

// Optical F5Low: NR for a game that makes no upscaler call of its own (the native modes). The mode selector sits at the
// top, then the Advanced settings (depth, flow tuning).
static void RenderF5LowPage(Config* config, const NrCommon& nr)
{
    ImGui::SeparatorText("Optical F5Low (experimental)");
    ImGui::TextWrapped("Runs NR in a game that makes no upscaler call of its own.");
    ImGui::TextWrapped(
        "Optical F5Low works out how the picture moves by itself, the way a game's motion vectors would tell "
        "it: it compares each frame with the last, uses the game's depth when it can find it, keeps a "
        "still HUD still and notices scene cuts. NR, and in the second mode OptiScaler's upscaler and "
        "frame generation, run on that motion. D3D11, D3D12 and Vulkan games.");

    bool finishedPicture = nr.finishedPicture;
    RenderNativeMode(config, nr.enabled, finishedPicture);

    RenderLowLatencySection();

    // Detail reuse on Optical F5Low's motion: shown here because this is where its saving matters most (NR on the whole
    // finished picture). Only its state: the checkbox and its fine-tuning live on the NR Options page.
    ImGui::SeparatorText("Lighter NR");
    if (!config->DlssNrDetailReuse.value_or_default())
        ImGui::TextUnformatted("Reuse detail between frames: Off");
    else
    {
        const auto status = DlssNr::DetailReuseStatus();
        if (!status.why.empty())
            ImGui::TextWrapped("Reuse detail between frames: On, %s", status.why.c_str());
        else if (status.active)
            ImGui::Text("Reuse detail between frames: On (full %llu | reused %llu%s)", status.full, status.reused,
                        status.holding ? " | paused while moving fast" : "");
        else
            ImGui::TextUnformatted("Reuse detail between frames: On");
    }
    HelpMarker("Runs the model every other frame (or every 3rd) to cut NR's cost by about half (or two thirds); with "
               "Optical F5Low it skips detail where the motion can't be trusted. Set on the NR Options page.");
    if (ImGui::SmallButton("Set in NR Options##f5lowdetailreuse"))
        MenuPages::RequestPage(MenuPages::Page::NrOptions);

    // Compress screen edges is another way to lighten NR; it works on the finished picture too. Only its state here.
    {
        const bool compress = config->DlssNrSpatialCompression.value_or_default();
        const EdgeCompressionLine line = EdgeCompressionText(config, nr);
        const std::string text = std::string("Compress screen edges: ") + (compress ? line.text : std::string("Off"));
        EdgeCompressionDrawText({ compress ? line.tone : EdgeCompressionLine::Tone::Grey, text });
        HelpMarker("NR works on fewer pixels: the middle stays sharp, the edges are squeezed. Saves GPU time; edges "
                   "slightly softer. Set on the NR Input page.");
        if (ImGui::SmallButton("Set in NR Input##f5lowcompress"))
            MenuPages::RequestPage(MenuPages::Page::NrInput);
    }

    // The page is Optical F5Low's own, so every control shows and the section starts open.
    if (ImGui::TreeNodeEx("Advanced##nativeinputadvanced", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool pictures = config->DlssNrNativeDebugView.value_or_default();

        if (ImGui::Checkbox("Show the motion and trust pictures##nativedebugview", &pictures))
            config->DlssNrNativeDebugView = pictures;

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Shows the motion estimate and trust mask on this page (small GPU cost, D3D12). "
                                    "The depth picture also needs \"Show the picked depth here\" and a restart.");

        switch (CurrentNativeApi())
        {
        case NativeApi::Dx11:
            GenericDepthDx11::DrawAdvancedUi();
            NativeMotionDx11::DrawAdvancedUi();
            break;
        case NativeApi::Vulkan:
            GenericDepthVk::DrawAdvancedUi();
            NativeMotionVk::DrawAdvancedUi();
            break;
        default:
            GenericDepthDx12::DrawAdvancedUi();
            NativeMotionDx12::DrawAdvancedUi();
            break;
        }

        ImGui::TreePop();
    }
}

// Status & Presets: a line pointing to Optical F5Low, and the pass-count and quality-tier presets.
static void RenderStatusPage(Config* config, float menuResScale, const NrCommon& nr)
{
    const auto shown = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });

    ImGui::Text("Optical F5Low (NR for a game with no upscaler call): %s", NativeModeName(shown));
    ImGui::SameLine();

    if (ImGui::SmallButton("Open Optical F5Low##statuslink"))
        MenuPages::RequestPage(MenuPages::Page::NrF5Low);

    const float presetWidth = PresetButtonWidth();

    ImGui::SeparatorText("Look Presets");
    {
        const auto now = ReadLook(config);
        // The input styles are skipped above 100% model resolution and on the deferred path (Config.h DlssNrInputFlatten ...)
        const bool inputStyleOff = config->DlssNrWorkingScale.value_or_default() > 1.0f + 0.005f ||
                                   config->DlssNrDeferredDlss.value_or_default();

        // The base the second row is built on is derived from the settings when one of those looks is in effect, else
        // it is this session's last pick (Standard at start) and changes nothing until a look is clicked
        static DlssNrLook::Base pickedBase = DlssNrLook::Base::Standard;
        const auto inEffect = DlssNrLook::SecondRowInEffect(now);

        if (inEffect)
            pickedBase = inEffect->base;

        for (size_t i = 0; i < DlssNrLook::kLookCount; ++i)
        {
            const auto& look = DlssNrLook::kLooks[i];

            // Two rows: the everyday looks, then the ones that aim at part of the picture or restyle it
            if (i != 0 && i != 4)
                ImGui::SameLine();

            const bool active = look.isModifier ? inEffect && inEffect->look == look.id
                                                : DlssNrLook::Matches(now, look.settings);

            if (PresetButton(look.name, active, presetWidth))
                WriteLook(config, look.isModifier ? DlssNrLook::ComposeLook(look.id, pickedBase) : look.settings);

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%s", look.tip,
                                  look.experimental && inputStyleOff
                                      ? "\nNo effect right now: the input style is skipped above 100% model "
                                        "resolution and on the deferred path."
                                      : "");
        }

        // Directly under the row it changes. Picking a base while one of these looks is on moves that look onto it.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Built on:");

        for (size_t b = 0; b < DlssNrLook::kBaseCount; ++b)
        {
            ImGui::SameLine();

            if (ImGui::RadioButton(LookBaseNames[b], pickedBase == (DlssNrLook::Base) b))
            {
                pickedBase = (DlssNrLook::Base) b;

                if (inEffect)
                    WriteLook(config, DlssNrLook::ComposeLook(inEffect->look, pickedBase));
            }
        }

        ImGui::SameLine();
        HelpMarker("Which look Faces, Painterly, Soft canvas and Structure start from: its style, strength and "
                   "curve. The look then adds its own skin, detail or input changes. Picking a base while one of "
                   "these looks is on applies it right away; with none on, it only sets what the next click uses.");

        // An input style that is skipped does nothing: say so where the look is, not only in its tooltip
        if (inEffect && inputStyleOff && DlssNrLook::Get(inEffect->look).experimental)
        {
            const char* name = DlssNrLook::Get(inEffect->look).name;

            if (config->DlssNrDeferredDlss.value_or_default())
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f), "%s: no effect with \"Generate before SR, apply after SR\".",
                                   name);
            else
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                   "%s: no effect at %.0f%% model resolution (works at 100%% or below).", name,
                                   config->DlssNrWorkingScale.value_or_default() * 100.0f);
        }
    }
    HelpMarker("How NR's edit looks: each pass's style and strengths, the curve, Detail and Colour strength, skin and "
               "scenery, and the input style. Leaves the pass count, the quality tier and exposure alone. Green = in "
               "effect; changing one of its settings clears it.");

    ImGui::SeparatorText("Multipass Presets");
    for (uint32_t passes = 1; passes <= IM_ARRAYSIZE(PassPresetNames); ++passes)
    {
        if (passes > 1)
            ImGui::SameLine();
        if (PresetButton(PassPresetNames[passes - 1], PassPresetActive(config, passes), presetWidth))
            ApplyPassPreset(config, passes);
    }
    HelpMarker("Sets the pass count, with NR on FP8 and the default exposure. How each pass looks is the Look "
               "presets'. Green = in effect; changing the pass count clears it.");

    // Directly under the pass presets, since those buttons set this slider's value. This page
    // has no page-wide item width, so this block pushes its own to keep the slider's width.
    ImGui::PushItemWidth(220.0f * menuResScale);

    // The checkbox that sets this lives under "Apply the model" on the NR Options page; read from config.
    bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();
    const unsigned int passLimit = unlockPasses ? MaxPassCount : DefaultMaxPassCount;

    {
        int passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u,
                                      passLimit);
        const ImVec4 colour = passes <= 1   ? ImVec4(0.35f, 0.88f, 0.38f, 1.0f)
                              : passes == 2 ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                            : ImVec4(0.92f, 0.30f, 0.25f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, colour);

        if (ImGui::SliderInt("Model passes", &passes, 1, (int) passLimit,
                             passes == 1 ? "%d (normal)" : "%dx model cost"))
            config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, (int) passLimit);

        ImGui::PopStyleColor(2);

        // Reset's own label stays plain text -- placed after PopStyleColor so the
        // passes-based warning colour above doesn't tint it too.
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##modelpasses"))
            config->DlssNrPasses = 1u;

        HelpMarker("How often the model runs per frame. Higher: stronger effect, more GPU time. Default: 1.");
    }

    {
        // Disabled rather than hidden at Passes == 1: the control exists, it just has nothing to
        // do yet (there is no boundary between passes to damp), which is a clearer statement
        // than making it vanish and reappear as Passes changes.
        const bool noBoundary = config->DlssNrPasses.value_or_default() <= 1;
        ImGui::BeginDisabled(noBoundary);
        float feedback = config->DlssNrPassFeedback.value_or_default();
        if (ImGui::SliderFloat("Pass feedback", &feedback, 0.0f, 1.0f,
                               feedback >= 1.0f ? "%.2f (full, current behaviour)" : "%.2f"))
            config->DlssNrPassFeedback = std::clamp(feedback, 0.0f, 1.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##passfeedback"))
            config->DlssNrPassFeedback = 1.0f;
        ImGui::EndDisabled();

        HelpMarker("How much of each extra pass's change reaches the next. Higher (1.0): full effect. Lower: "
                   "gentler, steadier extra passes. Default: 1.0. No effect with 1 pass.");
    }

    ImGui::PopItemWidth();

    // Both rows share the same button labels, so each gets its own ImGui ID scope.
    const auto tierRow = [&](const char* id, bool beforeSuperResolution) {
        ImGui::PushID(id);
        for (int i = 0; i < IM_ARRAYSIZE(ResolutionTiers); ++i)
        {
            if (i > 0)
                ImGui::SameLine();
            if (PresetButton(ResolutionTiers[i].name,
                             ResolutionTierActive(config, ResolutionTiers[i], beforeSuperResolution), presetWidth))
                ApplyResolutionTier(config, pendingScale, ResolutionTiers[i], beforeSuperResolution);
        }
        ImGui::PopID();
    };

    ImGui::SeparatorText("Pre-SR Presets");
    tierRow("pre", true);
    HelpMarker("Quality tiers for NR before SR, from High (full resolution, best) to Potato (50%, cheapest). Sets "
               "Upscale Mode and Method and Model resolution, and runs composed. The look is the Look presets'. "
               "Superior Scaling, if selected, is kept. Green = in effect; changing those clears it.");

    ImGui::SeparatorText("Post-SR Presets");
    tierRow("post", false);
    HelpMarker("Quality tiers for NR after SR, from High (full resolution, best) to Potato (50%, cheapest). Sets "
               "Upscale Mode and Method and Model resolution, and runs composed. The look is the Look presets'. "
               "Superior Scaling, if selected, is kept. Green = in effect; changing those clears it.");
}

// NR Exposure, drawn at the end of the NR Input page.
static void RenderExposureSection(Config* config, float menuResScale)
{
    ImGui::SeparatorText("NR Exposure");
    ImGui::TextDisabled("HDR input settings. Adjust the brightness range presented to NR.");

    // Whether these controls act on the frame NR last ran on (shaders/dlssnr/DlssNr_ExposureState.h). They pick the
    // white point that divides a linear HDR frame; a tone-mapped frame reaches the model as it is, and a display HDR
    // frame (HDR10, scRGB, forced PQ) is put at its paper white instead. Shown greyed rather than hidden, so the values
    // stay visible and come back as they were on a linear HDR frame. Highlight guard, below, acts in every case.
    const auto encodingStatus = DlssNr::ReadColourEncodingStatus();
    const auto exposureState = encodingStatus.seen ? encodingStatus.exposureState : DlssNrExposureState::State::LinearHdr;
    const bool exposureActs = exposureState == DlssNrExposureState::State::LinearHdr;

    if (exposureState == DlssNrExposureState::State::ToneMapped)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f), "Not used here: the game's picture is already tone mapped.");
        HelpMarker("NR gets this picture as it is, with no exposure applied, so the settings below change nothing. "
                   "Colour encoding (NR Output) says how the picture was read.");
    }
    else if (exposureState == DlssNrExposureState::State::DisplayPinned)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f), "Not used here: HDR10/scRGB picture. NR's white is the paper "
                                                            "white below.");
        HelpMarker("An HDR10 or scRGB picture is already in screen brightness, so NR takes its white from the game's "
                   "HDR paper white, not from an exposure. The greyed settings change nothing.");

        // The one brightness control that acts here (DlssNr_ExposureState.h PinnedWhitePoint).
        float nits = DlssNrExposureState::PaperWhiteNits(config->DlssNrDisplayPaperWhiteNits.value_or_default());

        if (ImGui::SliderFloat("Game's HDR paper white", &nits, DlssNrExposureState::kMinPaperWhiteNits,
                               DlssNrExposureState::kMaxPaperWhiteNits, "%.0f nits", ImGuiSliderFlags_Logarithmic))
            config->DlssNrDisplayPaperWhiteNits = DlssNrExposureState::PaperWhiteNits(nits);

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##displaypaperwhite"))
            config->DlssNrDisplayPaperWhiteNits = DlssNrExposureState::kReferenceWhiteNits;

        HelpMarker("Set it to the game's own HDR paper white (or UI brightness) setting. Higher: darker input to NR. "
                   "Default 203 nits, the HDR reference white.");
    }

    ImGui::BeginDisabled(!exposureActs);

    {
    // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
    // a frame the game already tone mapped wants roughly 1, the high end because there is no
    // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
    // given game needs to go is a property of that game's exposure, not of anything we can bound.
    // One tester was still improving at 100. A linear slider over that span would spend nine
    // tenths of its travel on values nobody needs and never reach the ones they do.
    // One dropdown, because there is one answer.
    //
    // This was two checkboxes that could both be on, and every attempt to stop that was a patch
    // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
    // once both were set the only way out was a button the notice never mentioned. Clearing
    // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
    // state being REACHED; a single choice cannot reach it, because there is only one value to
    // be in.
    //
    // Each option also says whether it can actually do anything in THIS game, in colour, so the
    // choice is made on what is available rather than on what sounds best.
    {
        const bool vk = DlssNr::IsRunningVk();
        const auto ex = vk ? DlssNr::GameExposureStatusVk() : DlssNr::GameExposureStatus();
        const bool haveExposure = HaveGameExposure();

        const float anchorNow = DlssNr::ExposureScan::BestValue();
        const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

        static const char* sourceNames[] = { "Manual paper white", "Game exposure",
                                             "Scanned exposure (experimental)",
                                             "Automatic exposure from HDR frame" };

        // Auto first, naming what it is now; the other entries are the values 0-3
        const uint32_t configuredSource = config->DlssNrWhitePointSource.value_or_default();
        // While the exposure scan's match moves the picture (story 5), Auto names that one. Asked of the pass, not the
        // scan: the scan can hold a match the picture is not using (SDR, a finished picture, a white point override).
        const bool autoFound = configuredSource == DlssNrExposureDefault::kSourceAuto &&
                               DlssNr::WhitePointSourceEffective(*config) == DlssNrExposureDefault::kSourceAutomatic &&
                               (vk ? DlssNr::FollowGameExposureStatusVk() : DlssNr::FollowGameExposureStatus())
                                   .foundInForce;
        const std::string autoSource = std::string("Auto (") +
                                       (autoFound ? "Game's exposure, found in the game"
                                                  : sourceNames[DlssNr::WhitePointSourceEffective(*config)]) +
                                       ")";
        const char* sourceItems[] = { autoSource.c_str(), sourceNames[0], sourceNames[1], sourceNames[2],
                                      sourceNames[3] };
        int sourceItem = configuredSource == DlssNrExposureDefault::kSourceAuto
                             ? 0
                             : (int) std::min(configuredSource, DlssNrExposureDefault::kSourceAutomatic) + 1;

        if (ImGui::Combo("White point source", &sourceItem, sourceItems, IM_ARRAYSIZE(sourceItems)))
        {
            config->DlssNrWhitePointSource =
                sourceItem == 0 ? DlssNrExposureDefault::kSourceAuto : (uint32_t) (sourceItem - 1);

            // Nothing else to set. The scan asks the source whether it is wanted, so choosing
            // it here is the whole of switching it on -- there is no second flag to keep in
            // step, and so no way for the two to disagree.
        }

        const int source = (int) DlssNr::WhitePointSourceEffective(*config);

        HelpMarker("Where NR's brightness reference comes from.\n"
                   "Auto: the game's exposure when the game gives one. Else, in D3D12 games, OptiScaler looks for\n"
                   "the game's own exposure among its GPU buffers while you play; once found, the game's value moves\n"
                   "the brightness at Automatic exposure's level. Until then, or if none is found, Automatic exposure.\n"
                   "Manual: Paper white.\n"
                   "Game exposure: the game's own value.\n"
                   "Scanned exposure: read from game buffers; needs calibration.\n"
                   "Automatic exposure: OptiScaler measures the frame itself.");

        // Auto's exposure search, while Auto is chosen: searching, found, not needed, ...
        if (configuredSource == DlssNrExposureDefault::kSourceAuto)
        {
            const std::string search = DlssNr::ExposureScan::AutoStatus();

            if (!search.empty())
                ImGui::TextDisabled("Exposure search: %s", search.c_str());
        }

        // Availability, in colour, for the option currently chosen. Two lines at most, whichever source.
        if (ImGui::TreeNode("Exposure readout##autoexposure"))
        {
            const float exposureSlot = StatusSlotBegin();
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("Waiting for a frame...");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "No game exposure available. Using manual paper white.");
                else if (ex.exposure > 1e-6f)
                {
                    // Brightness points are keyed by the game's exposure in both scales; the scale only changes what
                    // the Trim multiplies. Both whites are shown so the scale can be chosen by eye.
                    const float anchorKey = DlssNrGameScale::AnchorKey(ex.preExposure, ex.exposure);
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        anchorKey, config->DlssNrWhitePointTrim.value_or_default(), trimAnchors, false);
                    const bool asIs = DlssNr::GameExposureScaleEffective(*config) == DlssNrGameScale::kAsIs;
                    const float whiteAsIs =
                        DlssNrGameScale::WhiteBase(DlssNrGameScale::kAsIs, ex.preExposure, ex.exposure) * trim;
                    const float whiteWithExposure =
                        DlssNrGameScale::WhiteBase(DlssNrGameScale::kWithExposure, ex.preExposure, ex.exposure) * trim;
                    ImGui::TextColored(
                        ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                        asIs ? "Game value %.2f  ->  NR level %.2f (Native look), %.2f with Game's setting%s"
                             : "Game value %.2f  ->  NR level %.2f (Game's setting), %.2f with Native look%s",
                        ex.exposure, asIs ? whiteAsIs : whiteWithExposure, asIs ? whiteWithExposure : whiteAsIs,
                        ex.offeredNow ? "" : "  (held: absent this frame)");
                }
                else
                    ImGui::TextDisabled("Reading exposure...");
            }
            else if (source == 3)
            {
                const auto autoEx = vk ? DlssNr::AutoExposureStatusVk() : DlssNr::AutoExposureStatus();

                if (autoEx.exposure > 1e-8f)
                {
                    const float baseWhitePoint = autoEx.preExposure / autoEx.exposure;
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        baseWhitePoint, DlssNr::AutoTrimEffective(*config), trimAnchors, false);
                    // Middle-grey metering, mode 13 in dlssnr.hlsl: exposure = 0.18 / (0.82 * average scene brightness).
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scene brightness %.3f  ->  model white at %.2f",
                                       0.18f / (0.82f * autoEx.exposure), baseWhitePoint * trim);
                    HelpMarker("Measured on the frame before NR. Model white: the brightness treated as full white.");
                    ImGui::TextDisabled("Automatic exposure %.4f", autoEx.exposure);
                }
                else
                    ImGui::TextDisabled("Calculating automatic exposure...");
            }
            else if (source == 2 && vk)
            {
                // The scan finds Vulkan candidates too, but the Vulkan path resolves source 2 to Paper white.
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                   "Scanned exposure is DirectX 12 only. Using Paper white.");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "No exposure candidates found.");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "%u candidates; move between bright and dark areas to test them.",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "Exposure candidate found. Adjust Paper white, then select Anchor here.");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "Game exposure is available.");
            }
            StatusSlotEnd(exposureSlot, 2);
            ImGui::TreePop();
        }
    }






    // A measured suggestion for paper white used to sit here and has been withdrawn.
    //
    // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
    // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
    // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
    // sits wherever most tiles are. The guard meant to catch that compared each tile against the
    // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
    // relative-threshold mistake the white point meter was removed for, made a second time.
    //
    // A wrong number offered confidently is worse than no number, so nothing is offered. What
    // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
    // the exposure texture where a game supplies one, and otherwise the ratio between the
    // scene-referred buffer and the finished frame, which is that exposure by definition.

    // Two controls, not one control with two meanings.
    //
    // These are different quantities. The manual path wants an absolute divisor on an open-ended
    // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
    // number the game already supplied, where 1 is correct and anything far from it says the read
    // is wrong rather than that somebody prefers it.
    //
    // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
    // a ruinous value unreachable but left two worse problems: moving the slider in one mode
    // silently destroyed the number found in the other, and there was no way back to "just take
    // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
    // Switching modes is now non-destructive in both directions.
    // The trim belongs to both automatic sources, since both end in "the game's number times a
    // little". Only the manual source gets the absolute slider.
    // One slider per source, each remembering its own number.
    //
    // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
    // things, and a value found against one means nothing against the other. Sharing them meant
    // changing source silently carried a number across, so a picture that had been tuned came
    // back wrong for a reason nothing on screen explained.
    //
    // The scan before it is anchored is the exception, and it has to be: anchoring captures an
    // absolute white point, so there must be an absolute slider to set. Showing a trim there
    // asked people to "set paper white below" next to a control that was not paper white.
    const int wpSource = (int) DlssNr::WhitePointSourceEffective(*config);

    // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
    // local and not persisted; the anchor block below sets it when a row is clicked. Declared
    // here because both the slider (this block) and the table (below) read it in the same frame.
    static int selectedAnchor = -1;
    auto anchors = DlssNr::ExposureScan::Anchors();
    if (selectedAnchor >= (int) anchors.size())
        selectedAnchor = -1;

    if (wpSource == 2)
    {
        const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

        // The single scan -> white point readout, above the sliders it explains.
        if (!anchors.empty())
        {
            const float liveScan = DlssNr::ExposureScan::BestValue();

            if (liveScan > 0.0f)
            {
                const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                    liveScan, config->DlssNrScanInverted.value_or_default(),
                    config->DlssNrScanTrim.value_or_default());

                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                   (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
            }
        }

        // Paper white shows only when there is a point to set: before the first anchor, or when a
        // row is selected to edit. Once points exist and none is selected, the white point is fixed
        // by the anchors and only the trim adjusts the live picture -- so the trim takes the
        // slider's place, the same shape as the game-exposure source.
        const bool showPaperWhite = anchors.empty() || editingRow;

        if (showPaperWhite)
        {
            float pw = editingRow ? anchors[selectedAnchor].white
                                  : config->DlssNrWhitePointScale.value_or_default();

            char lbl[48];
            if (editingRow)
                snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
            else
                snprintf(lbl, sizeof(lbl), "Paper white");

            if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (editingRow)
                {
                    DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                    config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                }
                else
                    config->DlssNrWhitePointScale = pw;
            }

            HelpMarker("Adjusts the selected calibration point, or the next one. Anchor here saves the current "
                       "lighting.");
        }

        // The trim multiplies the interpolated result, and in the steady state it is the control
        // that stands in for paper white: adjust it until the picture looks right in the current
        // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
        if (!anchors.empty())
        {
            float trim = config->DlssNrScanTrim.value_or_default();

            if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

            ImGui::SameLine();

            if (ImGui::SmallButton("Reset##scantrim"))
                config->DlssNrScanTrim = 1.0f;

            HelpMarker("Multiplies the calibrated white point. Higher: darker NR input. Lower: brighter. Anchor here "
                       "saves it and resets this to 1.");
        }
    }
    else if (wpSource == 1)
    {
        // Up to 50x under the hood: a game's reported exposure scale can sit well below what the picture wants
        // (Marvel's Spider-Man Remastered with XeSS swapped to DLSS is one), so 4x was too tight. Shown as
        // stops around 1x, which is why the slider runs further towards darker than towards brighter.
        // Index = DlssNrGameScale value: 0 with the game's exposure, 1 as is.
        static const char* scaleNames[] = { "Game's setting", "Native look" };
        // Auto first, naming what it is for this game; the other entries are the values 0-1
        const uint32_t configuredScale = config->DlssNrGameExposureScale.value_or_default();
        const std::string autoScale =
            std::string("Auto (") + scaleNames[DlssNr::GameExposureScaleEffective(*config)] + ")";
        const char* scaleItems[] = { autoScale.c_str(), scaleNames[0], scaleNames[1] };
        int scale = configuredScale == DlssNrExposureDefault::kScaleAuto
                        ? 0
                        : (int) std::min(configuredScale, DlssNrExposureDefault::kScaleAsIs) + 1;

        if (ImGui::Combo("Starting point##gameexposure", &scale, scaleItems, IM_ARRAYSIZE(scaleItems)))
        {
            const uint32_t before = DlssNr::GameExposureScaleEffective(*config);
            config->DlssNrGameExposureScale = scale == 0 ? DlssNrExposureDefault::kScaleAuto : (uint32_t) (scale - 1);

            // A Trim set on one scale means something else on the other, so the slider starts again at 0 EV. Pinning the
            // scale Auto already gave (Auto (Native look) -> Native look) keeps it.
            if (DlssNr::GameExposureScaleEffective(*config) != before)
                config->DlssNrWhitePointTrim = DlssNrExposureCalibrate::kGameExposureNeutralTrim;
        }

        HelpMarker("Auto: Native look for games measured to need it (NBA 2K27), else Game's setting.\n"
                   "Native look: like games with built-in NR (e.g. NBA 2K27).\n"
                   "Game's setting: the game's brightness value. Use if Native look is far too dark (e.g. RDR2).\n"
                   "Changing it resets the brightness slider to 0.");

        RenderTrimEvSlider(config->DlssNrWhitePointTrim, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                           DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default()).size(),
                           "gameexposure",
                           "How bright a picture NR works on. 0 is the starting point chosen above."
                           "\nHigher is brighter, lower is darker. Too bright loses detail in highlights;"
                           "\ntoo dark loses detail in shadows.");
        RenderTuneForThisScene(1, config->DlssNrWhitePointTrim, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                               config->DlssNrGameExposureTrimAnchors);
        RenderBrightnessPoints(config->DlssNrGameExposureTrimAnchors, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                               "gameexposure");
    }
    else if (wpSource == 3)
    {
        // The scale stays centred on a 5x Trim (0 EV), the old default from the PR this came from. The default is
        // +1.5 EV for every game; see DlssNr_AutoTrimDefault.h for the measurements. It is independent of the Game exposure Trim.
        RenderTrimEvSlider(config->DlssNrAutoExposureTrim, DlssNrExposureCalibrate::kNeutralTrim,
                           DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default()).size(),
                           "autoexposure",
                           "Brightness of the picture handed to NR. + is brighter, - is darker."
                           "\nUntil you move it, it is +1.5 EV in every game."
                           "\nReset goes back to that default."
                           "\nToo bright clips highlights or tints shadows; too dark hides shadow detail."
                           "\nOptiScaler meters the linear HDR frame itself before NR runs."
                           "\nAutomatic exposure is available on D3D12 and Vulkan.",
                           DlssNrAutoTrim::kDefaultTrim);

        RenderTuneForThisScene(3, config->DlssNrAutoExposureTrim, DlssNrExposureCalibrate::kNeutralTrim,
                               config->DlssNrAutoExposureTrimAnchors);
        RenderBrightnessPoints(config->DlssNrAutoExposureTrimAnchors, DlssNrExposureCalibrate::kNeutralTrim,
                               "autoexposure");

        // Following the game's own exposure (DlssNr_FollowGame.h): on by default for a known unexposed game
        // (DlssNr_GameDefaults.h). Vulkan follows from the host value, a few frames behind the game.
        {
            const bool followVk = DlssNr::IsRunningVk();
            bool follow = DlssNr::FollowGameOn(*config);

            if (ImGui::Checkbox("Follow the game's exposure", &follow))
                config->DlssNrAutoExposureFollowGame = follow;

            HelpMarker("Automatic follows the game's own exposure, so brightness moves with the game (cutscenes, "
                       "fades). For games that hand over the frame before exposure; on by default for RDR2. Leave "
                       "off for most games: their exposure would apply twice.");

            // Status and Re-learn only while following: off, the checkbox already says so, and a second
            // "calibrate" button next to Tune for this scene read as the same thing.
            if (follow)
            {
                const auto followStatus =
                    followVk ? DlssNr::FollowGameExposureStatusVk() : DlssNr::FollowGameExposureStatus();
                const auto& calibration = DlssNrFollowGame::Instance();
                ImGui::Indent();
                if (ImGui::TreeNode("Follow status##autoexposure"))
                {
                    const float followSlot = StatusSlotBegin();

                    if (!followStatus.gameExposureSeen)
                        ImGui::TextDisabled("Not available yet: no exposure from the game");
                    else if (!calibration.Locked())
                        ImGui::TextDisabled("Learning the calibration... (%u/%u)", calibration.Readings(),
                                            DlssNrFollowGame::kWindow);
                    else
                        ImGui::TextDisabled("Calibration %+.2f EV against the game's exposure%s", calibration.OffsetEv(),
                                            followStatus.following ? "; following" : "; not following");

                    // The calibration eases toward Automatic when the two stay apart (DlssNr_FollowGame.h Track), so a
                    // large disagreement is usually on its way out; say which, wrapped (it ran off the panel before).
                    // The warning shows above the limit and stays until the disagreement is well under it: it changes every
                    // frame, and hovering at the limit made the warning (and everything below it) flicker.
                    static bool disagreementShown = false;
                    const float disagreement = std::fabs(followStatus.disagreementEv);
                    const float limit = DlssNrExposureCalibrate::kFollowDisagreementLimitEv;
                    disagreementShown = disagreement > limit || (disagreementShown && disagreement > 0.75f * limit);

                    if (followStatus.following && calibration.Locked() && calibration.Easing())
                    {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextDisabled("Easing the calibration toward Automatic's own exposure (%+.1f EV apart).",
                                            followStatus.disagreementEv);
                        ImGui::PopTextWrapPos();
                    }
                    else if (disagreementShown)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.25f, 1.0f));
                        // Easing is off in a game whose own exposure moves (DlssNr_FollowGame.h): then only Re-learn.
                        if (calibration.GameMoves())
                            ImGui::TextWrapped("%+.1f EV off Automatic's own exposure. Re-learn in an ordinary scene if "
                                               "the picture looks too bright or too dark.",
                                               followStatus.disagreementEv);
                        else
                            ImGui::TextWrapped("%+.1f EV off Automatic's own exposure. It eases back by itself if this "
                                               "lasts; Re-learn to start over in an ordinary scene.",
                                               followStatus.disagreementEv);
                        ImGui::PopStyleColor();
                    }

                    StatusSlotEnd(followSlot, 4);
                    ImGui::TreePop();
                }

                // Learns the calibration again from scratch.
                if (ImGui::SmallButton("Re-learn##autoexposure"))
                {
                    DlssNrFollowGame::Instance().Reset();
                    LOG_INFO("DLSS-NR automatic exposure: re-learning the calibration against the game's exposure");
                }

                HelpMarker("Re-learns how Automatic relates to the game's exposure (about 2 s). Do it in a normal "
                           "daylight scene, e.g. if it learned during a cutscene.");
                ImGui::Unindent();
            }
        }

        static const char* const meterNames[] = { "Average", "Percentile (log)" };
        int meter = std::min<int>((int) config->DlssNrAutoExposureMeter.value_or_default(), 1);

        if (ImGui::Combo("Meter", &meter, meterNames, IM_ARRAYSIZE(meterNames)))
            config->DlssNrAutoExposureMeter = (uint32_t) meter;

        HelpMarker("How scene brightness is measured.\n"
                   "Average: the whole frame, bright areas counted less.\n"
                   "Percentile: the middle of the brightness range, as Unreal and Unity do; lamps and sky don't pull "
                   "it up.\n"
                   "Re-check Model input brightness after switching.");

        if (meter == (int) DlssNrExposureMeter::kPercentile)
        {
            float low = config->DlssNrAutoExposureMeterLowPercent.value_or_default();
            float high = config->DlssNrAutoExposureMeterHighPercent.value_or_default();

            if (ImGui::SliderFloat("Ignore darkest", &low, 0.0f, 50.0f, "%.0f%%"))
                config->DlssNrAutoExposureMeterLowPercent = std::clamp(low, 0.0f, 50.0f);

            HelpMarker("Darkest share of the picture left out of the reading. Higher: ignores more shadow. Default: "
                       "10%.");

            if (ImGui::SliderFloat("Ignore brightest", &high, 50.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureMeterHighPercent = std::clamp(high, 50.0f, 100.0f);

            HelpMarker("Readings above this brightness are left out. Lower: ignores more highlights. Default: 90%.");
        }
        else
        {
            float protection = config->DlssNrAutoExposureShadowProtection.value_or_default();
            if (ImGui::SliderFloat("Ignore bright highlights", &protection, 0.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureShadowProtection = std::clamp(protection, 0.0f, 100.0f);

            HelpMarker("Stops sky, lamps and reflections darkening the rest. Higher: highlights count less. 0%: "
                       "everything counts equally.");
        }

        // DlssNr_ExposureAdapt.h: a pass of its own on D3D12 and Vulkan.
        float adaptBrighter = DlssNrExposureAdapt::Seconds(
            config->DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(), DlssNrExposureAdapt::kDefaultBrighterSeconds);
        float adaptDarker = DlssNrExposureAdapt::Seconds(config->DlssNrAutoExposureAdaptDarkerSeconds.value_or_default());

        if (ImGui::SliderFloat("Eye adaptation, to brighter", &adaptBrighter, 0.0f, DlssNrExposureAdapt::kMaxSeconds,
                               adaptBrighter > 0.0f ? "%.2f s" : "off"))
            config->DlssNrAutoExposureAdaptBrighterSeconds =
                DlssNrExposureAdapt::Seconds(adaptBrighter, DlssNrExposureAdapt::kDefaultBrighterSeconds);

        HelpMarker("How fast brightness follows the scene getting brighter. Higher: slower, steadier. Lower: faster. "
                   "Off: instant. Not used while following the game's exposure.");

        if (ImGui::SliderFloat("Eye adaptation, to darker", &adaptDarker, 0.0f, DlssNrExposureAdapt::kMaxSeconds,
                               adaptDarker > 0.0f ? "%.2f s" : "off"))
            config->DlssNrAutoExposureAdaptDarkerSeconds = DlssNrExposureAdapt::Seconds(adaptDarker);

        HelpMarker("The same for the scene getting darker. Slower by default, so a brief flash doesn't darken the "
                   "picture for long.");
    }
    else
    {
        // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
        // because a frame the game already tone mapped wants roughly 1, the high end because
        // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
        // how far up a given game needs to go is a property of that game's exposure rather than
        // of anything that can be bounded here. One tester was still improving at 100.
        float wpScale = config->DlssNrWhitePointScale.value_or_default();

        if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic))
            config->DlssNrWhitePointScale = wpScale;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##paperwhite"))
            config->DlssNrWhitePointScale = 1.0f;

    HelpMarker("Brightness reference for NR's input (linear HDR pictures). Higher: darker input. Lower: brighter "
               "input. Change it if NR loses detail or shifts colours.");
    }

    ImGui::EndDisabled();

    // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
    // belongs with the exposure controls it works alongside.
    float maxRatio = config->DlssNrMaxRatio.value_or_default();
    if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, MaxHighlightGuard, "%.1fx"))
        config->DlssNrMaxRatio = maxRatio;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##guard"))
        config->DlssNrMaxRatio = 2.0f;

    HelpMarker("Limits how much NR can brighten a pixel. Lower: fewer highlight changes. Higher: more. In Replace "
               "mode it limits darkening too.");

    // Directly under the white point, because that is the number it moves and the number the
    // anchor captures. It used to sit under Inspect, a whole section away from the slider it
    // reads, which left "Anchor here" looking like a control for something else entirely.
    {
        // No checkbox here any more.
        //
        // The dropdown above says whether the scan is the white point's source, and that is
        // the only reason anybody using this would want it running. A second control could
        // only agree with the dropdown or contradict it, and both were on offer: it began as
        // a redundant question and became a way to switch off the thing the chosen source
        // depended on.
        //
        // The ini key survives as a developer override for the one case a user has no reason
        // to want -- running the scan in a game that supplies a REAL exposure, so the log can
        // compare the two. That is validation, and validation does not need a widget.
        //
        // Worth keeping written down, since the panel no longer says it: the scan matches
        // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
        // real exposure, so the right answer sat visible beside it -- the best candidate was
        // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
        // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
        // eye adaptation.

            // Only where it means something. The lamp reads the scan, so offering it beside a
            // white point that comes from the game's own exposure is offering a control that
            // cannot light up.
            bool meter = config->DlssNrScanMeter.value_or_default();

            if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                ImGui::Checkbox("Show exposure meter", &meter))
                config->DlssNrScanMeter = meter;

            HelpMarker("Shows the scanned exposure and a colour indicator. Display only.");

        // Shown when the scan is actually running, whichever way it got switched on.
        if (DlssNr::ExposureScan::Scanning())
        {
            // Anchoring: one press, then it never needs touching again.
            //
            // The absolute white point cannot come out of a buffer whose units are unknown.
            // Every value AFTER the first can: only the ratio against the anchor is used, so
            // whatever the number means, it cancels. That is why this is a button and not a
            // measurement -- the one thing a person can supply that no amount of cleverness
            // can is "this looks right to me".
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

            // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
            // replace. One row is the old single-anchor ratio law; add a second in different
            // light and the white point is interpolated between the points, so it holds across
            // the whole range instead of only near one anchor. Greyed unless the scan is the
            // chosen source and it currently has a value to capture.
            ImGui::BeginDisabled(live <= 0.0f || !isSource);

            if (ImGui::Button("Anchor here"))
            {
                // What to capture. Before the first point, the paper white above (an absolute value
                // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                // picture is showing right now -- the interpolated value times the Trim the user just
                // dialed in -- so a second point in different light captures the trimmed look, not a
                // frozen paper white (which would make two equal whites and a flat, non-tracking
                // curve). The trim is reset afterwards: the new point, which the picture now passes
                // through exactly, must not be multiplied by it a second time.
                const float captureWhite =
                    anchors.empty()
                        ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                        : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                              live, config->DlssNrScanInverted.value_or_default(),
                                              config->DlssNrScanTrim.value_or_default()));

                if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                {
                    config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    config->DlssNrScanTrim = 1.0f;
                    selectedAnchor = -1;
                }
            }

            ImGui::EndDisabled();

            HelpMarker("Saves the current exposure and white point as a calibration point (up to 8). Set Paper white "
                       "for the first, Trim for the rest.");

            if (!isSource)
                ImGui::TextDisabled("Scanned exposure is not the selected white point source.");

            if (!anchors.empty())
            {
                // The row nearest the live scan value (in log space) is the one driving the
                // picture right now; mark it so the user can see which calibration is in effect.
                int active = 0;
                float bestDist = 1e30f;
                const float liveLog = std::log(std::max(live, 1e-6f));

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    const float d =
                        std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        active = (int) i;
                    }
                }

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    ImGui::PushID((int) i);

                    // Delete first, so its click is never swallowed by the row-wide Selectable.
                    if (ImGui::SmallButton("x"))
                    {
                        DlssNr::ExposureScan::AnchorRemove((int) i);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        if (selectedAnchor == (int) i)
                            selectedAnchor = -1;
                        else if (selectedAnchor > (int) i)
                            --selectedAnchor;
                        ImGui::PopID();
                        continue;
                    }

                    ImGui::SameLine();

                    const bool sel = (int) i == selectedAnchor;
                    char row[96];
                    snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                             ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                             anchors[i].white, sel ? "   [editing]" : "");

                    // Click selects the row (slider edits it); click again deselects (slider
                    // returns to the live unanchored point).
                    if (ImGui::Selectable(row, sel))
                        selectedAnchor = sel ? -1 : (int) i;

                    ImGui::PopID();
                }

                ImGui::TextDisabled("Select a row to edit it; select it again"
                                    " to deselect. > marks the active point.");
            }

            // The direction flag only means anything with a single point; with two or more the
            // direction the white point moves is already fixed by the data.
            if (anchors.size() == 1)
            {
                bool inverted = config->DlssNrScanInverted.value_or_default();
                if (ImGui::Checkbox("Invert exposure tracking", &inverted))
                    config->DlssNrScanInverted = inverted;

                HelpMarker("Reverses how scanned exposure moves the white point. Only with one calibration point.");
            }

            // The scan -> white point readout is shown above the sliders now, not here.

            // Everything below is read-out rather than control: what the scan is looking at and
            // how to tell whether it found the right thing. Folded away because the two decisions
            // that matter -- anchor, and which way the number runs -- are above it.
            if (ImGui::TreeNode("Advanced"))
            {

                const auto found = DlssNr::ExposureScan::Report();
                const char* why = DlssNr::ExposureScan::Status();

                if (found.empty())
                {
                    ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                  ? why
                                                  : "No exposure candidates found.");
                }
                else
                {
                    for (size_t i = 0; i < found.size(); ++i)
                    {
                        const auto& c = found[i];

                        if (c.reads == 0)
                        {
                            ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                            continue;
                        }

                        // Moving is the whole signal, so it is the thing that is coloured.
                        ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                   : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                           "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1,
                                           c.shape.c_str(), c.latest, c.lowest, c.highest,
                                           c.moves ? "MOVES" : "flat so far");
                    }

                    ImGui::TextDisabled("Move between bright and dark areas to check exposure tracking.");
                    ImGui::TextDisabled("A value that only increases may be a counter.");
                }

                ImGui::TreePop();
            }
        }
    }


    }
}

// NR Options.
static void RenderOptionsPage(Config* config, float menuResScale, const NrCommon& nr)
{
    const bool enabled = nr.enabled;
    bool finishedPicture = nr.finishedPicture;
    // Edited by the "Lift model pass limit" checkbox below.
    bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();

    ImGui::SeparatorText("NR Options");

    bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                    (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    const bool deferredActive = !finishedPicture && config->DlssNrDeferredDlss.value_or_default() && !rayReconstruction;

    // A single exclusive choice, not two independent checkboxes: "before SR" used to be one
    // checkbox whose own label AND meaning silently changed depending on finishedPicture's
    // state, and "neither checked" was an unlabeled third placement (after SR, not on the
    // finished picture) a user had to infer rather than see. All three are named options here.
    //
    // A Combo, not inline RadioButtons: this panel's width isn't user-resizable, and three
    // radios with these labels ran off the visible edge with no way to reach the third one.
    // Every other 3+-option control in this file (Model precision right below, Upscale Mode,
    // Upscale Method, Final Image Composition) is already a Combo for the same reason.
    static const char* placementNames[] = { "After Super Resolution", "Before Super Resolution",
                                             "Finished Picture" };
    int placement = finishedPicture ? 2 : (beforeSr ? 1 : 0);

    if (deferredActive)
        ImGui::BeginDisabled();
    if (ImGui::Combo("NR Pass at:", &placement, placementNames, IM_ARRAYSIZE(placementNames)))
    {
        if (placement == 0)
        {
            if (finishedPicture)
                RetryNr();
            finishedPicture = false;
            beforeSr = false;
            config->DlssNrFinishedPicture = false;
            config->DlssNrRunBeforeSr = false;
        }
        else if (placement == 1)
        {
            if (finishedPicture)
                RetryNr();
            finishedPicture = false;
            beforeSr = true;
            config->DlssNrFinishedPicture = false;
            config->DlssNrRunBeforeSr = true;
        }
        else
        {
            if (!finishedPicture)
                RetryNr();
            finishedPicture = true;
            config->DlssNrFinishedPicture = true;
        }
    }
    if (deferredActive)
        ImGui::EndDisabled();

    HelpMarker("Where NR runs.\n"
               "After Super Resolution (default): on the upscaled picture.\n"
               "Before Super Resolution: on the smaller picture; cheaper.\n"
               "Finished Picture: after the game's effects; may help green noise, but also changes HUD and menus "
               "(DirectX 12).\n"
               "With Ray Reconstruction, NR always runs after.");

    if (finishedPicture && enabled)
    {
        const auto feature = State::Instance().currentFeature;
        if (feature && (feature->Api() != API::DX12 || feature->IsWithDx12()))
            ImGui::TextWrapped("This option needs a native DirectX 12 game.");
        else
            ImGui::TextWrapped("%s", DlssNr::FinishedPictureStatus().c_str());
    }

    // Nested under Finished Picture: a second, independent axis (generate the changes at the
    // smaller pre-SR size vs. at the finished picture's own size), not a fourth top-level
    // placement -- progressive disclosure, same as every other mode-gated control in this file.
    if (finishedPicture)
    {
        if (ImGui::Checkbox("Run the model before Super Resolution", &beforeSr))
        {
            config->DlssNrRunBeforeSr = beforeSr;
            config->DlssNrDeferredDlss = false;
        }
        HelpMarker("Runs the model on the smaller input, upscales its changes with DLSS and applies them to the "
                   "final picture. Experimental; colours may differ. Needs DLSS SR, not RR.");
    }

    bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
    int precisionChoice = config->DlssNrPrecision.value_or_default() == 4 ? 1 : 0;
    const char* precisions[] = { "NVIDIA (FP8)", "Experimental (FP8+NVFP4 hybrid)" };
    if (ImGui::Combo("Model precision", &precisionChoice, precisions, IM_ARRAYSIZE(precisions)))
        config->DlssNrPrecision = precisionChoice == 1 ? 4u : 0u;
    HelpMarker("NVIDIA: the original FP8 model. Default.\n"
               "FP8+NVFP4: this fork's mixed-precision model for RTX 50; output may differ slightly. D3D12 only.");
    // One setting per kernel set: the fp8 kernels (NVIDIA's DLL and fp8-based builds) and the plain FP16 kernels (used by some modified DLSS-NR DLLs).
    // Only the one for the kernels actually running is used.
    const char* kernelSet = DlssNrNative::VitKernelSet();
    bool vitReuse = config->DlssNrVitEvery.value_or_default() > 1;
    if (ImGui::Checkbox("Reuse bottleneck: FP8 kernels", &vitReuse))
        config->DlssNrVitEvery = vitReuse ? 2u : 1u;
    HelpMarker("Reuses the model's slowest-changing stage every other frame. Saves about a tenth of NR's GPU time; "
               "fast motion can look slightly softer. On by default.");
    bool vitReusePlain = config->DlssNrVitEveryPlain.value_or_default() > 1;
    if (ImGui::Checkbox("Reuse bottleneck: plain FP16 kernels", &vitReusePlain))
        config->DlssNrVitEveryPlain = vitReusePlain ? 2u : 1u;
    HelpMarker("The same as above, used when the model runs the plain FP16 kernels (used by some modified DLSS-NR DLLs).\nOn by default.");
    ImGui::Text("Kernel set in use: %s", kernelSet);
    bool detailReuse = config->DlssNrDetailReuse.value_or_default();
    const auto detailReuseStatus = !detailReuse            ? DlssNr::DetailReuseInfo {}
                                   : DlssNr::IsRunningVk() ? DlssNr::DetailReuseStatusVk()
                                                           : DlssNr::DetailReuseStatus();
    const bool detailReuseRunning = detailReuse && detailReuseStatus.active;
    if (detailReuseRunning)
        ImGui::TextDisabled("Bottleneck reuse: off while Reuse detail between frames runs");
    else if (DlssNrNative::VitPlainKernels() ? vitReusePlain : vitReuse)
        ImGui::TextUnformatted(("Bottleneck reuse: " + DlssNrNative::VitStatus()).c_str());
    detailReuse = RenderDetailReuseToggle(config);
    if (detailReuse)
    {
        // Debugging and A/B testing only; the defaults are the tuned values.
        if (ImGui::TreeNode("Debug##detailReuse"))
        {
            bool detailReuseDebug = config->DlssNrDetailReuseDebug.value_or_default();
            if (ImGui::Checkbox("Show dropped detail", &detailReuseDebug))
                config->DlssNrDetailReuseDebug = detailReuseDebug;
            HelpMarker("On reused frames, paints magenta where the moved detail was dropped and cyan where Fill "
                       "replaced it.\nFor testing.");
            float fill = config->DlssNrDetailReuseFill.value_or_default();
            if (ImGui::SliderFloat("Fill dropped detail", &fill, 0.0f, 1.0f, "%.2f"))
                config->DlssNrDetailReuseFill = std::clamp(fill, 0.0f, 1.0f);
            HelpMarker("Fills areas where moved detail was dropped with NR detail from nearby pixels of the same "
                       "surface. Higher: fills more. 0: those areas show the game's picture. Default: 1.");
            float steady = config->DlssNrDetailReuseSteady.value_or_default();
            if (ImGui::SliderFloat("Steady full frames", &steady, 0.0f, 1.0f, "%.2f"))
                config->DlssNrDetailReuseSteady = std::clamp(steady, 0.0f, 1.0f);
            HelpMarker("Pulls full frames toward the reused detail so the two differ less. Higher: less pumping, a "
                       "little lag on motion. Default: 0 (off).");
            ImGui::SeparatorText("How far a moved sample is trusted");
            float depthTolerance = DlssNr::DetailReuseDepthToleranceEffective(*config);
            if (ImGui::SliderFloat("Depth tolerance", &depthTolerance, 0.0f, 1.0f, "%.3f"))
                config->DlssNrDetailReuseDepthTolerance = std::clamp(depthTolerance, 0.0f, 1.0f);
            HelpMarker("How different the depth may be before moved detail is dropped. Higher: keeps detail on water "
                       "and glass, may smear. Lower: drops more. Default: 0.051 (RDR2: 0.267).");
            float clipGamma = config->DlssNrDetailReuseClipGamma.value_or_default();
            if (ImGui::SliderFloat("Colour box", &clipGamma, 0.0f, 10.0f, "%.2f sigma"))
                config->DlssNrDetailReuseClipGamma = std::clamp(clipGamma, 0.0f, 10.0f);
            HelpMarker("How different the colour may be before moved detail is dropped. Higher: keeps detail on "
                       "busy, noisy content. Lower: drops more. Default: 1.25.");
            float clipFalloff = config->DlssNrDetailReuseClipFalloff.value_or_default();
            if (ImGui::SliderFloat("Colour falloff", &clipFalloff, 0.01f, 10.0f, "%.2f sigma"))
                config->DlssNrDetailReuseClipFalloff = std::clamp(clipFalloff, 0.01f, 10.0f);
            HelpMarker("How gradually trust fades past that limit. Higher: softer. Lower: all-or-nothing (speckles). "
                       "Never 0. Default: 1.00.");
            float sigmaFloor = config->DlssNrDetailReuseSigmaFloor.value_or_default();
            if (ImGui::SliderFloat("Flat area floor", &sigmaFloor, 0.0f, 1.0f, "%.3f"))
                config->DlssNrDetailReuseSigmaFloor = std::clamp(sigmaFloor, 0.0f, 1.0f);
            HelpMarker("Smallest colour range used on flat areas, so their noise doesn't drop their detail. Higher: "
                       "keeps more on flat areas. Default: 0.010.");
            bool withFg = config->DlssNrDetailReuseWithFg.value_or_default();
            if (ImGui::Checkbox("Keep on with frame generation", &withFg))
                config->DlssNrDetailReuseWithFg = withFg;
            HelpMarker("Keeps detail reuse running with frame generation. Off: reuse pauses while frame generation "
                       "runs. On by default.");
            float minFps = config->DlssNrDetailReuseMinFps.value_or_default();
            if (ImGui::SliderFloat("Minimum frame rate", &minFps, 0.0f, 120.0f, "%.0f fps"))
                config->DlssNrDetailReuseMinFps = std::clamp(minFps, 0.0f, 240.0f);
            HelpMarker("Reuse runs only above this rendered frame rate; below it every frame runs the model. Higher: "
                       "stops reusing sooner. 0: no minimum. Default: 25.");
            float maxDropped = config->DlssNrDetailReuseMaxDropped.value_or_default();
            if (ImGui::SliderFloat("Pause while moving fast", &maxDropped, 0.0f, 50.0f, "%.0f%% dropped"))
                config->DlssNrDetailReuseMaxDropped = std::clamp(maxDropped, 0.0f, 50.0f);
            HelpMarker("Runs the model every frame while more than this share of the picture is new (fast turns), "
                       "where reuse flickers at the edges. Lower: pauses sooner. 0: never. Default: 10%.");
            ImGui::TreePop();
        }
        const auto& status = detailReuseStatus;
        // Counters for tuning and bug reports, folded away by default so the menu is not left with a blank
        // stretch while they are hidden. The slot inside keeps the lines below still while it is open.
        if (ImGui::TreeNode("Statistics##detailReuse"))
        {
            // One line while it is not running, up to four while it is, and the last three come and go with the picture's
            // motion: always four lines, so nothing below moves with the camera.
            const float reuseSlot = StatusSlotBegin();
            if (!status.why.empty())
                ImGui::TextDisabled("Reuse detail: %s (rendered %.0f fps)", status.why.c_str(), status.baseFps);
            else
            {
                ImGui::Text("Full %llu | Reused %llu | Fallback %llu | %.0f fps", status.full,
                            status.reused, status.fallback, status.baseFps);
                if (status.held > 0 || status.holding)
                {
                    if (status.holding)
                        ImGui::TextUnformatted("Reuse: paused while moving fast");
                    if (status.dropped >= 0.0f)
                        ImGui::TextDisabled("%.0f%% of the last measured frame had no detail to move; paused on %llu "
                                            "frames so far",
                                            100.0f * status.dropped, status.held);
                    else
                        ImGui::TextDisabled("paused on %llu frames so far", status.held);
                }
                if (status.heavyMs > 0.0)
                {
                    ImGui::Text("NR GPU: %.2f ms avg (%.2f to %.2f)", status.averageMs,
                                status.lightMs, status.heavyMs);
                    HelpMarker("Full and reused frames take different time. If motion judders, cap the frame rate "
                               "just below the average.");
                }
            }
            StatusSlotEnd(reuseSlot, 4);
            ImGui::TreePop();
        }
    }
    RenderSceneCutToggle(config);
    if (precisionChoice > 0 && DlssNr::IsRunningVk())
    {
        // The hybrid rewrites the model's kernels through NvAPI's D3D12 entry points; on Vulkan the model runs
        // unchanged.
        ImGui::TextUnformatted("Hybrid: D3D12 only (not applied on Vulkan)");
    }
    else if (precisionChoice > 0)
    {
        ImGui::TextUnformatted(enabled && DlssNrNative::IsActive() ? "Hybrid: active" : "Hybrid: inactive");
        ImGui::TextWrapped("Loading may pause the game and look like a freeze. Please wait.");
    }
    // Keep failure details in the log without displaying changing kernel counters in the menu.
    auto hybridStatus = DlssNrNative::Status();
    hybridStatus = hybridStatus.substr(0, hybridStatus.find(" |"));
    static std::string lastHybridWarning;
    if (hybridStatus.rfind("Restart required:", 0) == 0 || hybridStatus.find("fallback") != std::string::npos)
    {
        if (hybridStatus != lastHybridWarning)
            LOG_WARN("Hybrid: {}", hybridStatus);
        lastHybridWarning = hybridStatus;
    }
    else
        lastHybridWarning.clear();
    if (!finishedPicture)
    {
        if (ImGui::Checkbox("Generate before SR, apply after SR (DLSS)", &deferredDlss))
            config->DlssNrDeferredDlss = deferredDlss;
        HelpMarker("Computes NR on the small input, upscales its changes with DLSS, applies them after SR. "
                   "Experimental: may flicker, costs more. Needs DLSS on D3D12; not RR. Turns off Hold frame, "
                   "Compare and Debug view.");
        if (deferredDlss && rayReconstruction)
            ImGui::TextWrapped("Generate before / apply after is unavailable with RR. Apply before SR "
                               "controls NR placement.");
        else if (deferredDlss)
            ImGui::TextWrapped("Residual DLSS: %s", DlssNr::DeferredDlssStatus().c_str());
    }
    else if (beforeSr)
        ImGui::TextWrapped("Pre-SR changes: %s", DlssNr::DeferredDlssStatus().c_str());

    // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
    // unless told. Dimmed, because it is a note rather than a setting.
    ImGui::TextDisabled("Set the NR toggle shortcut under Keybinds.");

    bool applyModel = config->DlssNrApplyModel.value_or_default();
    if (ImGui::Checkbox("Apply the model", &applyModel))
        config->DlssNrApplyModel = applyModel;

    HelpMarker("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural Rendering to stop its GPU cost.");

    if (ImGui::Checkbox("Lift model pass limit (up to 30; expensive)", &unlockPasses))
        config->DlssNrUnlockPasses = unlockPasses;
    HelpMarker("Allow up to 30 passes instead of 3. More passes use more GPU time and VRAM; high values may crash the game.");

    ImGui::Spacing();
}

// A 3D LUT (.cube) graded onto the NR input image before the model sees it (dlssnr-lut-apply epic,
// Story 4). Scanned from the LUTs folder inside the OptiScaler folder; LutFile also accepts any path typed
// into the ini directly, so the combo's preview shows the current selection's filename even when it
// is not one of the scanned entries.
static void RenderLutSection(Config* config)
{
    static std::vector<std::filesystem::path> lutFiles = ScanLutFolder();

    const std::string current = config->DlssNrLutFile.value_or_default();
    const std::string preview = current.empty() ? "(none)" : std::filesystem::path(current).filename().string();

    if (ImGui::BeginCombo("LUT", preview.c_str()))
    {
        const bool noneSelected = current.empty();
        if (ImGui::Selectable("(none)", noneSelected))
            config->DlssNrLutFile = std::string();
        if (noneSelected)
            ImGui::SetItemDefaultFocus();

        for (const auto& path : lutFiles)
        {
            const std::string pathStr = path.string();
            const bool selected = pathStr == current;
            if (ImGui::Selectable(path.filename().string().c_str(), selected))
                config->DlssNrLutFile = pathStr;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan##lut"))
        lutFiles = ScanLutFolder();

    HelpMarker("Applies a colour grade (.cube LUT) to the picture NR works from. Put files in OptiScaler\\LUTs and "
               "press Rescan. Strong grades also affect exposure, skin detection and reuse.");

    if (!current.empty())
    {
        float strength = config->DlssNrLutStrength.value_or_default();
        if (ImGui::SliderFloat("LUT strength", &strength, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLutStrength = strength;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##lutstrength"))
            config->DlssNrLutStrength = 1.0f;

        HelpMarker("How much of the LUT's grade reaches the image. 0 = no effect, 1 = the full grade.");
    }

    const auto lutStatus = DlssNr::ReadLutStatus();
    if (ImGui::TreeNode("Status##lut"))
    {
        const float lutSlot = StatusSlotBegin();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (!lutStatus.seen)
            ImGui::TextWrapped("Waiting for a frame.");
        else if (!lutStatus.wanted)
            ImGui::TextWrapped("No LUT loaded.");
        else if (lutStatus.loaded)
            ImGui::TextWrapped("Loaded: %s (%d^3)",
                               std::filesystem::path(lutStatus.loadedPath).filename().string().c_str(), lutStatus.size);
        else
            ImGui::TextWrapped("No LUT loaded.");
        ImGui::PopStyleColor();

        if (lutStatus.failed)
        {
            // Wrapped: a parse error can be longer than the menu is wide. A failed attempt does not
            // necessarily mean nothing is loaded -- a working LUT stays active if a later, different
            // path fails (DlssNr_Lut.h's documented contract) -- so this is shown alongside, not
            // instead of, the line above.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s: %s", std::filesystem::path(lutStatus.attemptedPath).filename().string().c_str(),
                               lutStatus.error.c_str());
            ImGui::PopStyleColor();
        }
        StatusSlotEnd(lutSlot, 5);
        ImGui::TreePop();
    }
}

// Compress screen edges, in the NR Input "Size" group after the Downscaler. The checkbox, the layout, what is happening,
// the on-screen layout, and the advanced values. The runtime rebuilds on any change (a layout change resets NR's
// history once); every edit goes through Spatial::Constrain, the same limits the runtime checks against.
static void StoreSpatialSettings(Config* config, const DlssNr::Spatial::Settings& s)
{
    config->DlssNrSpatialCenterX = s.centerX;
    config->DlssNrSpatialCenterY = s.centerY;
    config->DlssNrSpatialWorkX = s.workX;
    config->DlssNrSpatialWorkY = s.workY;
    config->DlssNrSpatialOffsetX = s.offsetX;
    config->DlssNrSpatialOffsetY = s.offsetY;
    config->DlssNrSpatialShiftX = s.shiftX;
    config->DlssNrSpatialShiftY = s.shiftY;
}

// Draws exactly the boxes the runtime published, as fractions of the frame: cyan for the sharp middle, orange for the
// part of the frame NR works on. Behind the menu, never in what the model is shown. Not saved; only while the menu is
// open (this runs from the menu).
static void DrawEdgeCompressionBoxes(const DlssNr::Spatial::Published& published, float menuResScale)
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const float thickness = std::max(1.0f, 2.0f * menuResScale);

    const auto box = [&](const DlssNr::Spatial::Rect& r, ImU32 colour)
    {
        list->AddRect(ImVec2(r.left * size.x, r.top * size.y), ImVec2(r.right * size.x, r.bottom * size.y), colour, 0.0f,
                      0, thickness);
    };

    box(published.work, IM_COL32(255, 150, 0, 230));
    box(published.center, IM_COL32(0, 230, 255, 230));
}

static void RenderEdgeCompression(Config* config, float menuResScale, const NrCommon& nr)
{
    namespace Sp = DlssNr::Spatial;

    bool on = config->DlssNrSpatialCompression.value_or_default();

    if (ImGui::Checkbox("Compress screen edges (experimental)", &on))
        config->DlssNrSpatialCompression = on;

    HelpMarker("NR works on fewer pixels: the middle stays sharp, the edges are squeezed and stretched back. Saves "
               "GPU time; edges a bit softer and may shimmer. Not with Reuse detail.");

    if (!on)
        return;

    Sp::Settings settings = Sp::ReadSettings(*config);
    const float scale = EdgeCompressionScale(config, nr);
    const int matched = Sp::MatchedLayout(settings);

    char preview[48];
    if (matched >= 0)
        std::snprintf(preview, sizeof(preview), "%s (%.0f / %.0f)", Sp::kNamedLayouts[matched].name,
                      Sp::kNamedLayouts[matched].center, Sp::kNamedLayouts[matched].work);
    else
        std::snprintf(preview, sizeof(preview), "Custom");

    if (ImGui::BeginCombo("Layout", preview))
    {
        for (int i = 0; i != 3; ++i)
        {
            char item[48];
            std::snprintf(item, sizeof(item), "%s (%.0f / %.0f)", Sp::kNamedLayouts[i].name, Sp::kNamedLayouts[i].center,
                          Sp::kNamedLayouts[i].work);

            if (ImGui::Selectable(item, matched == i))
            {
                // Picking a layout is also the reset: it puts every advanced value back to that layout's own.
                Sp::ApplyNamedLayout(settings, i);
                Sp::Constrain(settings, scale);
                StoreSpatialSettings(config, settings);
            }
        }

        // What the advanced values add up to when they are none of the above; shown, not picked.
        ImGui::BeginDisabled();
        ImGui::Selectable("Custom", matched < 0);
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }

    HelpMarker("How much stays sharp and how much NR works on. Light: about 90% of the pixels, Balanced: about 81%, "
               "Strong: about 64%. Picking one resets the advanced values; Custom means they were changed.");

    // What is happening. A slot of two lines, so the controls below do not move when this changes length.
    const float slot = StatusSlotBegin();
    EdgeCompressionDrawText(EdgeCompressionText(config, nr));

    // Only while compression is applied: when it is not (too small, a thin edge, turned itself off) Reuse detail runs.
    if (config->DlssNrDetailReuse.value_or_default() && nr.enabled &&
        EdgeCompressionNow(nr).status == Sp::Status::Active)
        ImGui::TextDisabled("Reuse detail between frames does not run while this is applied.");

    StatusSlotEnd(slot, 2);

    // The layout on screen: not saved, and only while the menu is open.
    static bool showLayout = false;
    ImGui::Checkbox("Show the layout on screen##edgecompression", &showLayout);
    HelpMarker("Draws two outlines over the game while this menu is open: cyan around the sharp middle, orange around the "
               "part of the picture NR works on. Outside the orange line the picture is not used by NR's model at all. "
               "It is not saved and never appears in what NR is shown.");

    if (showLayout)
    {
        const auto published = EdgeCompressionNow(nr);

        if (published.status == Sp::Status::Active)
            DrawEdgeCompressionBoxes(published, menuResScale);
        else
            ImGui::TextDisabled("Nothing to show: compression is not running.");
    }

    // The eight values behind the layout, each committed on release (a change rebuilds NR's model) with its own Reset.
    if (ImGui::TreeNode("Advanced layout##edgecompression"))
    {
        const float minimumWork = Sp::MinimumWorkPercent(scale);
        bool changed = false;

        // Limits from the values as they stand; Constrain below keeps every pair consistent after an edit.
        const auto xShift = Sp::WorkShiftLimits(settings, false);
        const auto yShift = Sp::WorkShiftLimits(settings, true);
        const float xOffset = Sp::MaxCenterOffset(settings.centerX);
        const float yOffset = Sp::MaxCenterOffset(settings.centerY);
        const auto range = [](float low, float high) { return std::pair<float, float> { low, std::max(low, high) }; };

        {
            const auto r = range(1.0f, settings.workX - 0.5f);
            changed |= DeferredSlider("Sharp middle width", &config->DlssNrSpatialCenterX, r.first, r.second, 80.0f,
                                      "%.1f%%");
            HelpMarker("Width of the full-sharpness middle. Higher: more stays sharp, saves less. Lower: saves more.");
        }
        {
            const auto r = range(1.0f, settings.workY - 0.5f);
            changed |= DeferredSlider("Sharp middle height", &config->DlssNrSpatialCenterY, r.first, r.second, 80.0f,
                                      "%.1f%%");
            HelpMarker("Height of the full-sharpness middle. Higher: more stays sharp, saves less. Lower: saves "
                       "more.");
        }
        {
            const auto r = range(std::max(minimumWork, settings.centerX + 0.5f), 100.0f);
            changed |= DeferredSlider("NR picture width", &config->DlssNrSpatialWorkX, r.first, r.second, 90.0f,
                                      "%.1f%%");
            HelpMarker("How much of the width NR works on. Lower: saves more, edges squeezed harder. Higher: "
                       "gentler.");
        }
        {
            const auto r = range(std::max(minimumWork, settings.centerY + 0.5f), 100.0f);
            changed |= DeferredSlider("NR picture height", &config->DlssNrSpatialWorkY, r.first, r.second, 90.0f,
                                      "%.1f%%");
            HelpMarker("How much of the height NR works on. Lower: saves more, edges squeezed harder. Higher: "
                       "gentler.");
        }
        {
            const auto r = range(-xOffset, xOffset);
            changed |= DeferredSlider("Move the middle left/right", &config->DlssNrSpatialOffsetX, r.first, r.second,
                                      0.0f, "%+.1f%%");
            HelpMarker("Moves the sharp middle sideways, as a share of the frame. Negative is left, positive is right. "
                       "Useful when the part of the picture you look at is not in the centre.");
        }
        {
            const auto r = range(-yOffset, yOffset);
            changed |= DeferredSlider("Move the middle up/down", &config->DlssNrSpatialOffsetY, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("Moves the sharp middle up or down, as a share of the frame. Negative is up, positive is down.");
        }
        {
            const auto r = range(xShift.first, xShift.second);
            changed |= DeferredSlider("Edge pixels left/right", &config->DlssNrSpatialShiftX, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("Shifts NR's edge pixels between the sides. Positive: right edge sharper, left softer. "
                       "Negative: the reverse. At the very ends compression turns off.");
        }
        {
            const auto r = range(yShift.first, yShift.second);
            changed |= DeferredSlider("Edge pixels top/bottom", &config->DlssNrSpatialShiftY, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("The same for the top and bottom edges. Positive gives the bottom edge more of NR's pixels and "
                       "the top edge fewer, negative the other way round.");
        }

        if (changed)
        {
            Sp::Settings edited = Sp::ReadSettings(*config);
            Sp::Constrain(edited, scale);
            StoreSpatialSettings(config, edited);
        }

        ImGui::TreePop();
    }
}

// NR Input.
static void RenderInputPage(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    // Same definitions as at the top of NR Options.
    const bool finishedPicture = nr.finishedPicture;
    const bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                          (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;

    ImGui::SeparatorText("NR Input Options");
    ImGui::Text("Size");

    // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
    // is 25%: below that the model is working on so little of the picture that its answer no
    // longer survives being enlarged onto it.
    // Applied when the handle is let go, not while it is moving.
    //
    // Every distinct value here is a different working size, and a different working size tears
    // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
    // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
    // reads live; only the commit waits.

    bool resolutionAuto = config->DlssNrModelResolutionAuto.value_or_default();
    const bool autoActive = resolutionAuto && (!beforeSr || rayReconstruction);

    int scalePercent = autoActive ? DlssNr::CurrentModelResolutionPercent()
                      : pendingScale >= 0
                           ? pendingScale
                           : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

    ImGui::BeginDisabled(autoActive);
    if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
        pendingScale = scalePercent;

    // Captured right here, before the Reset button below becomes the new "last item" --
    // IsItemDeactivatedAfterEdit() only ever reports on whatever was most recently
    // submitted, so checking it after the button would report the button's state, not
    // the slider's release, and the commit below would never fire.
    const bool sliderReleased = ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##modelresolution"))
    {
        config->DlssNrWorkingScale = 1.0f;
        pendingScale = -1;
    }
    ImGui::EndDisabled();

    if (!autoActive && sliderReleased && pendingScale >= 0)
    {
        config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
        pendingScale = -1;
        // The saved layout is not rewritten for a Model resolution: Strong at 30% would become a permanent Custom. When the
        // NR picture would be too small the status line says so and compression is not applied (Constrain is for edits to
        // the layout's own sliders).
    }

    if (DlssNr::SuperiorScaling::Enabled(*config))
        HelpMarker("Size NR works at, relative to its input. Lower: faster, less fine detail. Above 100%: costs more. "
                   "The game's resolution is unchanged.\n\n"
                   "With Superior Scaling, some percentages may look unusual or differ noticeably from NR at full "
                   "resolution. If so, try a few percentage points higher or lower.");
    else
        HelpMarker("Size NR works at, relative to its input. Lower: faster, less fine detail. Above 100%: costs more. "
                   "The game's resolution is unchanged.");

    {
        unsigned int modelWidth = 0;
        unsigned int modelHeight = 0;

        // Native Vulkan keeps its own; the network pools 2x2 in the same model on both.
        if (nr.vulkan)
            DlssNr::CurrentModelSizeVk(modelWidth, modelHeight);
        else
            DlssNr::CurrentModelSize(modelWidth, modelHeight);

        if (modelWidth != 0 && modelHeight != 0)
            ImGui::TextDisabled("Model input %ux%u; its main network runs at %ux%u.", modelWidth, modelHeight,
                                (modelWidth + 1) / 2, (modelHeight + 1) / 2);
    }

    if (ImGui::Checkbox("Auto (post-SR only)", &resolutionAuto))
        config->DlssNrModelResolutionAuto = resolutionAuto;
    HelpMarker("Sets Model resolution to the upscaler's own render-to-output ratio when NR runs after SR. Ignored "
               "when NR runs before SR.");

    if (autoActive)
        ImGui::TextDisabled("NR scale: %.2fx, derived from the upscaler's render:output ratio.",
                            scalePercent / 100.0f);
    else if (scalePercent > 100)
        ImGui::TextDisabled("NR scale: %.2fx. Higher resolution increases GPU cost.",
                            scalePercent / 100.0f);

    if (scalePercent > 100)
    {
        static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", "Lanczos2",
                                         "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
        int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
        if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
            ds = (int) Scaler::Lanczos3;

        if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
            config->DlssNrScalingDownscaler = (Scaler) ds;

        HelpMarker("Filter used to reduce NR output when Model resolution exceeds 100%.\nSharper filters may introduce ringing around edges.");
    }

    // Experimental input styles (Config.h DlssNrInputFlatten ..., DlssNr_InputStyles.h): restyle the picture the model is
    // shown, as one card. One way to flatten first (watercolour and smooth canvas do the same job, so a choice of one),
    // then the structure hint as a checkbox; each style's strength shows only while it is on. Its own section, open at start:
    // with the modern style a top-level section is a card.
    if (auto card = ScopedCollapsingHeader("Input style (experimental)", ImGuiTreeNodeFlags_DefaultOpen);
        card.IsHeaderOpen())
    {
        DlssNrInput::Styles styles = config->DlssNrInputStyles();
        bool changed = false;

        ImGui::TextDisabled("Restyles the picture NR is shown; the styles combine.");
        HelpMarker("A/B test: restyle the picture NR is shown before it runs. The game's own picture is not changed. "
                   "The styles combine and run in this order: flatten, then structure hint.\nWith a composed "
                   "curve only what NR paints differently shows; with a replace curve you see NR's painting as it is "
                   "(Restore Sharpness is off while a style is on, or it would put the game's texture back).\nEach "
                   "style is one more pass before the model runs. No effect above 100% Model resolution.");

        // A strength slider for a style that is on, with its Reset (1.0) beside it
        const auto strength = [&](const char* label, const char* resetId, float& value, uint32_t style)
        {
            const float most = DlssNrInput::MostStrength(style);
            ImGui::Indent();
            float shown = std::min(value, most);

            if (ImGui::SliderFloat(label, &shown, 0.0f, most, "%.2f"))
            {
                value = shown;
                changed = true;
            }

            ImGui::SameLine();

            if (ImGui::SmallButton(resetId))
            {
                value = 1.0f;
                changed = true;
            }

            ImGui::Unindent();
        };

        ImGui::TextUnformatted("Flatten first:");
        ImGui::SameLine();
        int flatten = (int) std::min(styles.flatten, DlssNrInput::kFlattenSmoothCanvas);
        changed |= ImGui::RadioButton("Off##inputflatten", &flatten, (int) DlssNrInput::kFlattenOff);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Watercolour##inputflatten", &flatten, (int) DlssNrInput::kFlattenWatercolour);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Smooth canvas##inputflatten", &flatten, (int) DlssNrInput::kFlattenSmoothCanvas);
        styles.flatten = (uint32_t) flatten;
        HelpMarker("Watercolour flattens texture into washes and keeps edges (can look blocky).\nSmooth canvas shows NR "
                   "skin like an untextured old-game surface: pores and fine shading flattened, contours kept, so NR "
                   "paints its own skin detail.\nStrength blends with the picture as it was (0 to 1).");

        if (styles.flatten != DlssNrInput::kFlattenOff)
            strength("Strength##inputflattenstrength", "Reset##inputflattenstrength", styles.flattenStrength,
                     DlssNrInput::FlattenStyle(styles.flatten));

        changed |= ImGui::Checkbox("Structure hint##inputstructure", &styles.structure);
        HelpMarker("Raises local contrast (brightness only) so NR has more structure to follow; high strength blurs "
                   "with a composed curve. Strength 0 to 3.");

        if (styles.structure)
            strength("Strength##inputstructurestrength", "Reset##inputstructurestrength", styles.structureStrength,
                     DlssNrInput::kStyleStructureHint);

        if (DlssNrInput::Any(styles))
        {
            changed |= ImGui::Checkbox("Faces only (skin colours)##inputstyle", &styles.facesOnly);
            HelpMarker("Apply the input styles only where the colour looks like skin (the same colour test as Skin "
                       "protection; warm materials can match too, coloured light can hide skin).");
        }

        if (changed)
            config->SetDlssNrInputStyles(styles);
    }

    RenderEdgeCompression(config, menuResScale, nr);

    RenderLutSection(config);

    ImGui::PopItemWidth();

    // After the pop: the exposure controls keep the default width they always had.
    RenderExposureSection(config, menuResScale);
}

// The model resolution NR applies, as a percentage: the slider, or with Auto (post-SR only) the
// ratio the runtime derived from render:output. The same rule the Input page uses for its slider.
static float AppliedModelScale(Config* config, const NrCommon& nr)
{
    const bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                          (nr.finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    const bool autoActive = config->DlssNrModelResolutionAuto.value_or_default() && (!beforeSr || rayReconstruction);

    return autoActive ? DlssNr::CurrentModelResolutionPercent() / 100.0f : config->DlssNrWorkingScale.value_or_default();
}

// NR Output, including Effect strength.
static void RenderOutputPage(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    ImGui::SeparatorText("NR Output Options");

    // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
    // supersampling composites its down-legged answer at native -- the residual collapses to the
    // model's own picture and the modes are identical, so the control says so by going grey. Superior
    // Scaling is the exception while it is selected: with Compress screen edges it also works at 100%, and it must
    // always be possible to switch it off.
    {
        // The applied scale, not the slider: with Auto after SR the slider can say 100% while NR runs smaller.
        const bool reduced = AppliedModelScale(config, nr) < 0.999f;
        const bool superior = DlssNr::SuperiorScaling::Enabled(*config);

        // Superior needs D3D12 (or a D3D12 bridge) and NVIDIA's DLSS SR through NGX for its temporal part.
        const bool wantsResidualDlss = DlssNr::SuperiorScaling::Diagnostics::PrivateDlss(
            config->DlssNrSuperiorResidualDlss.value_or_default(), DlssNr::SuperiorScaling::Diagnostics::Mask(*config));
        const bool superiorBlocked = nr.vulkan || (wantsResidualDlss && DlssNr::SuperiorScaling::SrUnavailable());

        if (!reduced && !superior)
            ImGui::BeginDisabled();

        static const char* enlargeNames[] = { "Classic", "Matched residual", "NVIDIA residual", "Superior Scaling" };
        const int enlarge =
            superior ? 3 : (int) std::min(config->DlssNrTransfer.value_or_default(), 2u);

        if (ImGui::BeginCombo("Upscale Mode", enlargeNames[enlarge]))
        {
            for (int i = 0; i < IM_ARRAYSIZE(enlargeNames); ++i)
            {
                const bool blocked = i == 3 && superiorBlocked && !superior;

                if (ImGui::Selectable(enlargeNames[i], i == enlarge,
                                      blocked ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
                    config->DlssNrTransfer = i == 3 ? DlssNr::SuperiorScaling::Transfer : (uint32_t) i;

                if (i == enlarge)
                    ImGui::SetItemDefaultFocus();
            }

            ImGui::EndCombo();
        }

        if (!reduced && !superior)
            ImGui::EndDisabled();

        HelpMarker("Below 100% Model resolution, how NR's result is enlarged.\n"
                   "Classic: the whole output.\n"
                   "Matched residual: only NR's changes; less blur and colour shift.\n"
                   "NVIDIA residual: only the changes, NVIDIA's way.\n"
                   "Superior Scaling: its own Area downsample, frequency shaping and a private DLSS that steadies NR "
                   "over time. Once selected it also works at 100% with Compress screen edges. Needs DirectX 12 and "
                   "NVIDIA DLSS; where it cannot run, Matched residual is used.");

        if (superior)
        {
            const bool skipCarrier = config->DlssNrSuperiorSkipCarrier.value_or_default();
            bool residualDlss = wantsResidualDlss;
            ImGui::BeginDisabled(skipCarrier);
            if (ImGui::Checkbox("DLSS residual stabilization (experimental)", &residualDlss))
                config->DlssNrSuperiorResidualDlss = residualDlss;
            ImGui::EndDisabled();
            HelpMarker("Disable the private residual DLSS for comparison. NR, motion-vector conversion, frequency "
                       "shaping and guided reconstruction remain active. At 100% with Compress screen edges, "
                       "guided reconstruction runs only near/in compressed regions; the center keeps NR without "
                       "the guided filter. Disabling DLSS may increase flicker or instability in motion.");
            ImGui::BeginDisabled(!residualDlss);
            static const char* presets[] = { "A - higher performance", "K - slower, more stable NR in motion" };
            int preset =
                DlssNr::SuperiorScaling::Preset(config->DlssNrSuperiorScalingPreset.value_or_default()) == 11 ? 1 : 0;

            if (ImGui::Combo("DLSS to temporally stabilize NR", &preset, presets, IM_ARRAYSIZE(presets)))
                config->DlssNrSuperiorScalingPreset = preset == 1 ? 11u : 1u;

            HelpMarker("Superior Scaling: a private DLSS in DLAA mode, applied to NR's change at its working "
                       "resolution. Independent of the NR model preset and the game's own DLSS preset.");
            ImGui::EndDisabled();

            if (ImGui::TreeNode("Superior stage bypasses (diagnostic)"))
            {
                ImGui::TextWrapped("Change one stage at a time. Each change resets NR and residual history. "
                                   "All boxes clear restores the default experimental path.");
                const auto option = [&](const char* label, auto& setting, const char* help)
                {
                    bool value = setting.value_or_default();
                    if (ImGui::Checkbox(label, &value))
                        setting = value;
                    HelpMarker(help);
                };
                option("Skip MV unit conversion", config->DlssNrSuperiorSkipMvUnits,
                       "Use the original motion units before Compress endpoint mapping. Without Compress, keep the "
                       "game scale for NR's matched vectors. Packed vectors are shared by NR and residual DLSS: "
                       "test with residual DLSS off first. Depth and the endpoint transform stay active.");
                option("Skip inter-pass correction", config->DlssNrSuperiorSkipInterPass,
                       "Run every NR pass without Superior's corrective inter-passes. Final shaping still runs.");
                option("Skip carrier encode/decode", config->DlssNrSuperiorSkipCarrier,
                       "Use signed NR minus proxy directly, without the 0.5 offset or [-1,2] carrier clipping. "
                       "FP16 residual storage stays. This temporarily disables residual DLSS; unchecking restores "
                       "the saved DLSS choice.");
                option("Skip guided reconstruction", config->DlssNrSuperiorSkipGuided,
                       "Use bilinear residual reconstruction instead of the 3x3 guided filter. Compress still "
                       "unwarps the result. Frequency shaping stays active.");
                option("Skip final frequency shaping", config->DlssNrSuperiorSkipFinalShaping,
                       "Set final high/medium and low gains to 1 and skip the final low field. "
                       "Inter-pass shaping is a separate switch.");
                option("Skip Compress correction for low", config->DlssNrSuperiorSkipCompressionLow,
                       "Low uses Model resolution before Compress, just like high/medium. "
                       "This changes both the inter-pass schedule and final gains.");
                option("Skip cached warp lookup", config->DlssNrSuperiorSkipWarpCache,
                       "Calculate the same Compress mapping directly instead of using its cached axis lookup.");
                option("Use full-frame guided at 100%", config->DlssNrSuperiorFullGuided,
                       "Disable this branch's uncompressed-center bypass. At 100% with Compress, guided runs "
                       "across the whole frame again. No effect while guided reconstruction is skipped.");
                option("Point-sampled motion (legacy)", config->DlssNrSuperiorPointMotion,
                       "Restore the previous nearest-texel motion sampling before Compress. Leave off to interpolate "
                       "smooth motion fields while retaining point sampling across motion discontinuities. "
                       "Keep Skip MV unit conversion off for this comparison. Only affects Superior with Compress.");
                if (ImGui::Button("Reset stage bypasses"))
                {
                    config->DlssNrSuperiorSkipMvUnits = false;
                    config->DlssNrSuperiorSkipInterPass = false;
                    config->DlssNrSuperiorSkipCarrier = false;
                    config->DlssNrSuperiorSkipGuided = false;
                    config->DlssNrSuperiorSkipFinalShaping = false;
                    config->DlssNrSuperiorSkipCompressionLow = false;
                    config->DlssNrSuperiorSkipWarpCache = false;
                    config->DlssNrSuperiorFullGuided = false;
                    config->DlssNrSuperiorPointMotion = false;
                }
                ImGui::TreePop();
            }

            const auto unavailable = DlssNr::SuperiorScaling::Unavailable();

            if (nr.vulkan)
                ImGui::TextWrapped("Superior Scaling needs DirectX 12 or a DirectX 12 bridge; Matched residual is "
                                   "used here.");
            else if (!unavailable.empty())
                ImGui::TextWrapped("Superior Scaling is not running (%s); Matched residual is used. Select it again "
                                   "to retry.",
                                   unavailable.c_str());
            else if (const auto status = DlssNr::SuperiorScaling::Status(); !status.empty())
                ImGui::TextWrapped("%s", status.c_str());
        }

        if (!reduced || superior)
            ImGui::BeginDisabled();

        static const char* upscaleMethodNames[] = { "Bilinear (fast)", "SGSR1" };
        int upscaleMethod = (int) std::min(config->DlssNrReducedUpscaleMethod.value_or_default(), 1u);

        if (ImGui::Combo("Upscale Method", &upscaleMethod, upscaleMethodNames, IM_ARRAYSIZE(upscaleMethodNames)))
            config->DlssNrReducedUpscaleMethod = (uint32_t) upscaleMethod;

        if (!reduced || superior)
            ImGui::EndDisabled();

        HelpMarker("Below 100% Model resolution, the filter that enlarges NR's result. Bilinear: cheapest, softest. "
                   "SGSR1: edge-aware, sharper. No effect with Superior Scaling, which enlarges its own way.");
    }

    // How the game's colour is decoded (DlssNr_ColourEncoding.h). Auto trusts the game; a forced choice is applied
    // even when the format looks unusual for it, and the line under the combo says so -- except Linear HDR on a format
    // that cannot hold it, which is refused (read as Tone-mapped sRGB), also said under the combo.
    {
        static const char* encodingNames[] = { "Auto", "Linear HDR", "Tone-mapped sRGB", "Tone-mapped gamma 2.2",
                                               "PQ (HDR10)" };
        static_assert(IM_ARRAYSIZE(encodingNames) == DlssNrColourEncoding::kSettingCount);
        int encoding = (int) config->DlssNrColourEncoding.value_or_default();
        if (encoding < 0 || encoding >= (int) DlssNrColourEncoding::kSettingCount)
            encoding = 0;
        if (ImGui::Combo("Colour Encoding", &encoding, encodingNames, IM_ARRAYSIZE(encodingNames)))
        {
            config->DlssNrColourEncoding = (uint32_t) encoding;
            LOG_INFO("DLSS-NR colour encoding set to {}", encodingNames[encoding]);
        }
        HelpMarker("How NR reads the game's colour. Auto: trust the game (default). Pick another only if the picture "
                   "looks washed out, too dark or banded. Linear HDR needs a float buffer; on an 8/10-bit one it is "
                   "ignored.");

        const auto status = DlssNr::ReadColourEncodingStatus();
        // The line and the warning are wrapped and the warning comes and goes with the detected format: a fixed slot.
        if (ImGui::TreeNode("Detected format##colourEncoding"))
        {
            const float encodingSlot = StatusSlotBegin();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", status.line.c_str());
            ImGui::PopStyleColor();
            if (!status.warning.empty())
            {
                // Wrapped: the sentence is longer than the menu is wide.
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
                ImGui::TextWrapped("%s", status.warning.c_str());
                ImGui::PopStyleColor();
            }
            StatusSlotEnd(encodingSlot, 5);
            ImGui::TreePop();
        }
    }

    // Experimental. 0 off (soft knee), 1 Reversible curve + our composition, 2 Reversible curve +
    // pure-inverse replace, 3 Balanced+composed, 4 Balanced+replace (identity midtones + unclipped
    // highlights), 5 HLG+composed, 6 PQ+composed (BT.2100 / ST 2084, white per BT.2408), 7 Linear+composed (what
    // game integrations hand the model). Always shown.
    static const char* reversibleNames[] = { "Off (soft knee)",          "Reversible curve + composed",
                                             "Reversible curve + replace", "Balanced curve + composed",
                                             "Balanced curve + replace",   "HLG curve + composed",
                                             "PQ curve + composed",        "Linear + composed" };
    static_assert(IM_ARRAYSIZE(reversibleNames) == DlssNrProxyCurve::kCount, "one name per proxy curve");
    const uint32_t reversibleValue = config->DlssNrReversibleMode.value_or_default();
    const int reversible = DlssNrProxyCurve::Valid(reversibleValue) ? (int) reversibleValue : 0;
    if (ImGui::BeginCombo("Final Image Composition (experimental)", reversibleNames[reversible]))
    {
        for (int i = 0; i < (int) DlssNrProxyCurve::kPickable; ++i)
        {
            const bool selected = i == reversible;
            if (ImGui::Selectable(reversibleNames[i], selected))
                config->DlssNrReversibleMode = (uint32_t) i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    HelpMarker("How NR's brightness range is mapped, and how its result is blended back.\n"
               "Soft knee / Balanced: compress highlights, keep midtones.\n"
               "Reversible curve: an exactly reversible mapping.\n"
               "HLG / PQ: more room for highlights; midtones look different to the model.\n"
               "Linear: NVIDIA's own look; use with Game exposure and Native look.\n"
               "Composed: blended with the strength controls. Replace: NR's answer directly; lower Highlight guard "
               "if it flickers.\n"
               "Re-run Tune after changing.");

    if (DlssNrProxyCurve::IsReplace((uint32_t) reversible))
    {
        float replaceDetail = config->DlssNrReplaceDetailStrength.value_or_default();
        if (ImGui::SliderFloat("Restore Sharpness", &replaceDetail, 0.0f, 2.0f, "%.2f"))
            config->DlssNrReplaceDetailStrength = replaceDetail;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##replacedetail"))
            config->DlssNrReplaceDetailStrength = 0.5f;

        HelpMarker("Sharpens fine edges using the original frame. Higher: sharper. 0: off. Only below 100% Model "
                   "resolution; most useful with Replace.");
    }

    ImGui::SeparatorText("Effect strength");

    float transfer = config->DlssNrTransferStrength.value_or_default();
    if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
        config->DlssNrTransferStrength = transfer;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##detail"))
        config->DlssNrTransferStrength = 1.0f;

    HelpMarker("Higher: more NR detail (above 1 exaggerated). Lower: less. 0: none. Default: 1.");

    float colour = config->DlssNrColourStrength.value_or_default();
    if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
        config->DlssNrColourStrength = colour;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##colour"))
        config->DlssNrColourStrength = 1.0f;

    HelpMarker("Higher: stronger NR colour (above 1 more saturated). 0: keeps the game's colours. Default: 1.");

    ImGui::PopItemWidth();
}

// Model passes and Colour.
static void RenderPassesPage(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    const bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();
    const unsigned int passLimit = unlockPasses ? MaxPassCount : DefaultMaxPassCount;

    ImGui::SeparatorText("Model passes");
    ImGui::TextWrapped("Settings apply when you release a slider.");
    static const char* styles[] = { "Standard", "Natural", "Cinematic" };
    static const char* inheritedStyles[] = { "Auto (inherit pass 1)", "Standard", "Natural", "Cinematic" };
    // [DlssNr] Preset / Pass2Preset / Pass3Preset, the model's DLSSNR.Hint.Render.Preset: 1 and 2 named as NVIDIA names
    // its presets (A = 1, B = 2). On 310.9 DLLs 1 selects the main model and 2 the small one, and any other value (0,
    // Default, included) falls back to 2; 310.8 DLLs have only the main model. A saved 3 behaves as 2 and shows as B.
    static const char* presets[] = { "Default", "Preset A (main model)", "Preset B (small model)" };
    static const char* inheritedPresets[] = { "Auto (inherit pass 1)", "Default", "Preset A (main model)",
                                              "Preset B (small model)" };
    static const char* presetHelp =
        "Which model NR runs on this pass. Preset A: the main model (more detail). Preset B: the small model (about half "
        "the cost, softer; only in 310.9 DLLs). Default: the DLL decides, which is the small model on 310.9 DLLs. 310.8 "
        "DLLs have only the main model, so the preset changes nothing there. Passes 4 and up use Pass 1's preset.";

    if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
        if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = (uint32_t) style;
        HelpMarker("Look profile: Standard, Natural or Cinematic. Intensity sets its strength.");
        int preset = (int) std::min(config->DlssNrPreset.value_or_default(), 2u);
        if (ImGui::Combo("Preset", &preset, presets, IM_ARRAYSIZE(presets)))
            config->DlssNrPreset = (uint32_t) preset;
        HelpMarker(presetHelp);
        DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
        DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
        bool mask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &mask))
            config->DlssNrAutoMask = mask;
        HelpMarker("Uses the model's own skin detection for Skin structure. Accuracy varies. Separate from the "
                   "colour mask below.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Pass 2", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
        InheritedProfileCombo("Style", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        InheritedProfileCombo("Preset", &config->DlssNrPass2Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
        HelpMarker(presetHelp);
        DeferredSlider("Intensity", &config->DlssNrPass2Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value() : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &mask))
            config->DlssNrPass2AutoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##mask"))
            config->DlssNrPass2AutoMask = std::optional<bool> {};
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Pass 3", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0. Reset restores these defaults.");
        InheritedProfileCombo("Style", &config->DlssNrPass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        InheritedProfileCombo("Preset", &config->DlssNrPass3Preset, inheritedPresets, IM_ARRAYSIZE(inheritedPresets));
        HelpMarker(presetHelp);
        DeferredSlider("Intensity", &config->DlssNrPass3Intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &config->DlssNrPass3LocalStructure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &config->DlssNrPass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &config->DlssNrPass3SkinStructure, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value() : config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &mask))
            config->DlssNrPass3AutoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##mask"))
            config->DlssNrPass3AutoMask = std::optional<bool> {};
        ImGui::TreePop();
    }

    const unsigned int visiblePasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, passLimit);
    for (unsigned int pass = 3; pass < visiblePasses; ++pass)
    {
        auto& settings = config->DlssNrExtraPasses[pass - 3];
        if (!ImGui::TreeNode(std::format("Pass {}", pass + 1).c_str()))
            continue;
        ImGui::TextWrapped("Defaults: inherit Pass 1; Local tone = 0.");
        InheritedProfileCombo("Style", &settings.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        DeferredSlider("Intensity", &settings.intensity, 0.0f, 2.0f, config->DlssNrIntensity.value_or_default(), "%.2f", true);
        DeferredSlider("Local structure", &settings.structure, 0.0f, 2.0f, config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
        DeferredSlider("Local tone", &settings.tone, 0.0f, 2.0f, 0.0f, "%.2f", true);
        DeferredSlider("Skin structure", &settings.skin, -1.0f, 2.0f, config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
        bool mask = settings.autoMask.value_or(config->DlssNrAutoMask.value_or_default());
        if (ImGui::Checkbox("Auto skin mask", &mask))
            settings.autoMask = mask;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##mask"))
            settings.autoMask = std::optional<bool> {};
        ImGui::TreePop();
    }

    ImGui::TextWrapped("Pass settings apply to SR and RR on DX12 and native Vulkan. The driver-proxy backend supports one pass.");

    ImGui::SeparatorText("Colour");

    if (ImGui::TreeNode("Skin and environment (final edit)"))
    {
        ImGui::TextWrapped("Select skin by colour and adjust the final NR effect separately for skin and scenery. Selection can be inaccurate; check Preview.");
        bool filter = config->DlssNrSkinProtection.value_or_default();
        if (ImGui::Checkbox("Separate skin / environment controls", &filter))
            config->DlssNrSkinProtection = filter;
        ImGui::BeginDisabled(!filter);
        bool tone = config->DlssNrSkinToneEnabled.value_or_default();
        if (ImGui::Checkbox("Allow skin tone / colour changes", &tone))
            config->DlssNrSkinToneEnabled = tone;
        HelpMarker("Allow NR colour changes in the selected skin region. Turn off to preserve its colour; detail can still change.");
        const auto slider = [](const char* label, auto& option) {
            float v = option.value_or_default();
            if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                option = v;
            ImGui::SameLine();
            const std::string resetId = std::string("Reset##") + label;
            if (ImGui::SmallButton(resetId.c_str()))
                option = 1.0f;
            HelpMarker("NR strength in this region. Higher: more effect. 0: none. Default: 1 (full).");
        };
        slider("Skin detail / lighting", config->DlssNrSkinDetail);
        ImGui::BeginDisabled(!tone);
        slider("Skin colour", config->DlssNrSkinColour);
        ImGui::EndDisabled();
        slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
        slider("Environment colour", config->DlssNrEnvironmentColour);
        bool preview = config->DlssNrShowSkinMask.value_or_default();
        if (ImGui::Checkbox("Preview colour-based mask", &preview))
            config->DlssNrShowSkinMask = preview;
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    ImGui::PopItemWidth();
}

// Compare and Debug.
static void RenderDebugPage(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    ImGui::SeparatorText("Compare");

    // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
    // clean way to A/B our own settings (a moving scene confounds every other comparison). See
    // design/frame-hold.md.
    bool held = config->DlssNrHoldFrame.value_or_default();
    if (ImGui::Checkbox("Hold frame", &held))
        config->DlssNrHoldFrame = held;

    HelpMarker("Freezes NR's input to compare settings. HUD may keep updating; SR/RR changes don't show.");

    bool frameStats = config->DlssNrFrameStats.value_or_default();
    if (ImGui::Checkbox("Log frame brightness stats", &frameStats))
        config->DlssNrFrameStats = frameStats;

    HelpMarker("Writes the frame's brightness stats to OptiScaler.log about every 2 s. For diagnosis.");

    RenderMeasureDetail();

    bool kernelProfile = config->DlssNrKernelProfile.value_or_default();
    if (ImGui::Checkbox("Log NR kernel profile", &kernelProfile))
        config->DlssNrKernelProfile = kernelProfile;

    HelpMarker("Writes where NR's GPU time goes to OptiScaler.log about every 4 s. For diagnosis.");

    static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
    int compare = (int) config->DlssNrCompare.value_or_default();
    if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
        config->DlssNrCompare = (uint32_t) compare;

    HelpMarker("Compare the original and NR result. Side by side fits both images; Wipe divides one full-size image.");

    if (compare != 0)
    {
        bool swap = config->DlssNrCompareSwap.value_or_default();
        if (ImGui::Checkbox("Swap sides", &swap))
            config->DlssNrCompareSwap = swap;
        HelpMarker("Swap the original and NR sides.");

        bool tags = config->DlssNrCompareTags.value_or_default();
        if (ImGui::Checkbox("Label the sides", &tags))
            config->DlssNrCompareTags = tags;

        HelpMarker("Display labels identifying the original and NR sides.");

        if (tags)
        {
            float tagScale = config->DlssNrTagScale.value_or_default();
            if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
        }

    }

    if (compare == 1)
    {
        float zoom = config->DlssNrCompareZoom.value_or_default();
        if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
            config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

        HelpMarker("Side by side only. 1: fits the whole image. 2: fills each half, cropping the sides.");
    }

    if (compare == 2)
    {
        float split = config->DlssNrCompareSplit.value_or_default();
        if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
            config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

        HelpMarker("Where the dividing line sits.");
    }

    static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                        "Difference (amplified)" };
    int debugView = (int) config->DlssNrDebugView.value_or_default();
    if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
        config->DlssNrDebugView = (uint32_t) debugView;

    HelpMarker("Shows the model input, its raw output, or the difference x20 (grey = no change).");

    ImGui::PopItemWidth();
}

static HeaderBanner::Inputs HeaderInputs(Config* config, HeaderBanner::Feature feature, bool upscalerFiles)
{
    using namespace HeaderBanner;

    const auto& state = State::Instance();
    const NativeApi api = CurrentNativeApi();

    Inputs in;
    in.feature = feature;
    in.upscalerFiles = upscalerFiles;
    in.gameCallsUpscaler = NativeGameCallsUpscaler(api);
    in.mode = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });
    // Optical F5Low is for D3D11, D3D12 and Vulkan games, and only helps where NR is switched on and has not failed
    // this session.
    in.nrAvailable = config->DlssNrEnabled.value_or_default() && FailureReason()[0] == 0;
    in.f5lowNrOnlyRunning = NativeNrOnlyRunning(api);
    in.nrEnabled = config->DlssNrEnabled.value_or_default() && FailureReason()[0] == 0;
    in.frameGeneration = state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();
    return in;
}

bool RenderHeaderBanner(Config* config, HeaderBanner::Feature feature, bool upscalerFiles,
                        const std::string& upscalerNames, const std::string& backendName, const ImVec4& offerColour,
                        void (*upscalerFileChecks)())
{
    using namespace HeaderBanner;

    const NativeApi api = CurrentNativeApi();
    const Banner banner = Decide(HeaderInputs(config, feature, upscalerFiles));

    switch (banner.line)
    {
    case Line::Blank:
    case Line::Frozen:
    case Line::SelectUpscaler:
    case Line::NoFiles:
        return false; // the menu's own lines
    case Line::OfferWithFiles:
        ImGui::TextColored(
            offerColour,
            "No upscaler call from the game. Pick %s as its upscaler and load a save, or use Optical F5Low.",
            upscalerNames.c_str());
        break;
    case Line::OfferNoFiles:
        ImGui::TextColored(offerColour, "No upscaler files found. Optical F5Low can still run NR without one.");
        break;
    case Line::F5LowNrAndFrameGen:
        ImGui::TextDisabled("Optical F5Low: NR and frame generation on the finished picture (%s gives frame generation "
                            "its input).",
                            backendName.c_str());
        break;
    case Line::F5LowNrNoFrameGen:
        ImGui::TextDisabled("Optical F5Low: NR on the finished picture, frame generation off.");
        break;
    case Line::F5LowFrameGenNoNr:
        ImGui::TextDisabled("Optical F5Low: frame generation on the finished picture (%s gives it its input), NR off.",
                            backendName.c_str());
        break;
    case Line::F5LowStabiliser:
        ImGui::TextDisabled("Optical F5Low: %s is ready to give frame generation its input; NR and frame generation "
                            "are off.",
                            backendName.c_str());
        break;
    case Line::F5LowNrOnly:
        ImGui::TextDisabled("Optical F5Low: NR on the finished picture.");
        break;
    case Line::F5LowStandsAside:
        ImGui::TextDisabled("The game's upscaler is on: Optical F5Low stands aside.");
        break;
    case Line::F5LowStatus:
        ImGui::TextDisabled("Optical F5Low:");
        ImGui::SameLine();
        NativeMotionStatus(api);
        break;
    }

    if (banner.line == Line::OfferWithFiles || banner.line == Line::OfferNoFiles)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");

        // The file checks stay behind the marker, as on the line this offer replaces: they are for troubleshooting
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("Menus and loading screens make no upscaler call either, so this can show there "
                                   "too.\nOptical F5Low runs NR on the finished picture, with or without the game's "
                                   "upscaler.");

            if (banner.line == Line::OfferWithFiles && upscalerFileChecks != nullptr)
            {
                ImGui::Spacing();
                upscalerFileChecks();
            }

            ImGui::EndTooltip();
        }
    }

    if (banner.action != Action::None)
    {
        ImGui::SameLine();

        if (ImGui::SmallButton(banner.action == Action::UseF5Low ? "Use Optical F5Low##headerf5low"
                                                                 : "Optical F5Low settings##headerf5low"))
            MenuPages::RequestPage(MenuPages::Page::NrF5Low);
    }

    return true;
}

void RenderF5LowUpscalerNote()
{
    ImGui::TextWrapped(
        "Driven by Optical F5Low (the game makes no upscaler call): this upscaler runs at the same size as the game's "
        "picture and only gives frame generation its input, unless Run the upscaler on the picture is ticked there.");

    if (ImGui::SmallButton("Optical F5Low settings##upscalernote"))
        MenuPages::RequestPage(MenuPages::Page::NrF5Low);

    ImGui::Spacing();
}

void RenderF5LowFrameGenHint(Config* config, bool noUpscalerFeature)
{
    using namespace HeaderBanner;

    // The same offer as the header's: only where Optical F5Low is off, NR can run and the game makes no upscaler call.
    if (!noUpscalerFeature || Decide(HeaderInputs(config, Feature::None, false)).action != Action::UseF5Low)
        return;

    ImGui::TextWrapped("No upscaler call from the game? Optical F5Low's NR + frame generation gives frame "
                       "generation its input.");

    if (ImGui::SmallButton("Use Optical F5Low##framegenhint"))
        MenuPages::RequestPage(MenuPages::Page::NrF5Low);

    ImGui::Spacing();
}

void UpdateF5LowHint(Config* config)
{
    // Once per session, and never again after it has been shown or once a game upscaler is in play.
    static bool shown = false;
    static bool gameUpscalerSeen = false;
    static double quietMs = 0.0;
    static double lastCallMs = 0.0;

    if (shown || gameUpscalerSeen || !config->F5LowHint.value_or_default())
        return;

    // Real time between presents (ImGui's DeltaTime stands still while no menu frame is drawn)
    const double now = Util::MillisecondsNow();
    const double stepMs = lastCallMs > 0.0 ? now - lastCallMs : 0.0;
    lastCallMs = now;

    const auto feature = State::Instance().currentFeature;
    const bool noFeature = feature == nullptr || !feature->IsInited();

    // The game called an upscaler, or one ran: it has its own, also when it turns it off in a menu later
    gameUpscalerSeen = HeaderBanner::GameUpscalerSeen(gameUpscalerSeen, NativeGameCallsUpscaler(CurrentNativeApi()),
                                                      !noFeature);

    if (gameUpscalerSeen)
        return;

    // The header's offer: Optical F5Low off, NR available, no upscaler call from the game, and no upscaler running.
    const bool offerStands =
        noFeature && HeaderBanner::Decide(HeaderInputs(config, HeaderBanner::Feature::None, false)).action ==
                         HeaderBanner::Action::UseF5Low;

    // A minute of the game running with the offer standing (a loading stall counts little): see HeaderBanner.h
    quietMs = HeaderBanner::HintQuietAfter(quietMs, offerStands, stepMs);

    if (!HeaderBanner::HintDue(quietMs))
        return;

    shown = true;
    ImGui::InsertNotification({ ImGuiToastType::Info, 15000,
                                "No upscaler call from this game so far.\nOptical F5Low can run NR without one: open "
                                "the menu, Neural Rendering, Optical F5Low." });
}

void RenderMenu(Config* config, float menuResScale, MenuPages::Page page)
{
    using MenuPages::Page;

    // The Enable checkbox and the running status head every Neural Rendering page.
    RenderEnableToggle(config);
    const NrCommon nr = ReadNrCommon(config);

    if (page == Page::NrStatus)
        RenderRunningStatus(config, nr);
    else
        RenderStatusLine(nr);

    switch (page)
    {
    case Page::NrOptions:
        RenderOptionsPage(config, menuResScale, nr);
        break;
    case Page::NrInput:
        RenderInputPage(config, menuResScale, nr);
        break;
    case Page::NrOutput:
        RenderOutputPage(config, menuResScale, nr);
        break;
    case Page::NrPasses:
        RenderPassesPage(config, menuResScale);
        break;
    case Page::NrDebug:
        RenderDebugPage(config, menuResScale);
        break;
    case Page::NrStatus:
        RenderStatusPage(config, menuResScale, nr);
        break;
    case Page::NrF5Low:
        RenderF5LowPage(config, nr);
        break;
    default:
        IM_ASSERT(false && "not a Neural Rendering page");
        break;
    }
}

} // namespace DlssNr
