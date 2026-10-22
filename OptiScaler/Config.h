#pragma once

#include "SysUtils.h"
#include "State.h"
#include "native/NativeLowLatencyRule.h"
#include "motion/MotionProfiles.h"
#include "shaders/dlssnr/DlssNr_InputStyles.h"
#include "framegen/mfgunlock/Mode.h"

#include <optional>
#include <filesystem>

enum HasDefaultValue
{
    WithDefault,
    NoDefault,
    SoftDefault // Change always gets saved to the config
};

template <class T, HasDefaultValue defaultState = WithDefault> class CustomOptional : public std::optional<T>
{
  private:
    T _defaultValue;
    std::optional<T> _configIni;
    bool _volatile;

  public:
    CustomOptional(T defaultValue)
        requires(defaultState != NoDefault)
        : std::optional<T>(), _defaultValue(std::move(defaultValue)), _configIni(std::nullopt), _volatile(false)
    {
    }

    CustomOptional()
        requires(defaultState == NoDefault)
        : std::optional<T>(), _defaultValue(T {}), _configIni(std::nullopt), _volatile(false)
    {
    }

    // Prevents a change from being saved to ini
    constexpr void set_volatile_value(const T& value)
    {
        if (!_volatile)
        { // make sure the previously set value is saved
            if (this->has_value())
                _configIni = this->value();
            else
                _configIni = std::nullopt;
        }
        _volatile = true;
        std::optional<T>::operator=(value);
    }

    // Use this when first setting a CustomOptional
    constexpr void set_from_config(const std::optional<T>& opt)
    {
        if (!this->has_value())
        {
            _configIni = opt;
            std::optional<T>::operator=(opt);
        }
    }

    constexpr CustomOptional& operator=(const T& value)
    {
        _volatile = false;
        std::optional<T>::operator=(value);
        return *this;
    }

    constexpr CustomOptional& operator=(T&& value)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(value));
        return *this;
    }

    constexpr CustomOptional& operator=(const std::optional<T>& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(opt);
        return *this;
    }

    constexpr CustomOptional& operator=(std::optional<T>&& opt)
    {
        _volatile = false;
        std::optional<T>::operator=(std::move(opt));
        return *this;
    }

    // Needed for string literals for some reason
    constexpr CustomOptional& operator=(const char* value)
        requires std::same_as<T, std::string>
    {
        _volatile = false;
        std::optional<T>::operator=(T(value));
        return *this;
    }

    constexpr T value_or_default() const&
        requires(defaultState != NoDefault)
    {
        return this->has_value() ? this->value() : _defaultValue;
    }

    constexpr T value_or_default() &&
        requires(defaultState != NoDefault) {
            return this->has_value() ? std::move(this->value()) : std::move(_defaultValue);
        }

        constexpr std::optional<T> value_for_config()
            requires(defaultState == WithDefault)
    {
        if (_volatile)
        {
            if (_configIni != _defaultValue)
                return _configIni;

            return std::nullopt;
        }

        if (!this->has_value() || *this == _defaultValue)
            return std::nullopt;

        return this->value();
    }

    constexpr std::optional<T> value_for_config()
        requires(defaultState != WithDefault)
    {
        if (_volatile)
            return _configIni;

        if (this->has_value())
            return this->value();

        return std::nullopt;
    }

    constexpr T value_for_config_or(T other)
    {
        auto option = value_for_config();

        if (option.has_value())
            return option.value();
        else
            return other;
    }
};

constexpr inline int UnboundKey = -1;
constexpr uint32_t NV_PRESET_LATEST = 0x00FFFFFF;

enum FpsOverlayPos : uint32_t
{
    FpsOverlayPos_TopLeft,
    FpsOverlayPos_TopRight,
    FpsOverlayPos_BottomLeft,
    FpsOverlayPos_BottomRight,
    FpsOverlayPos_COUNT,
};

enum FpsOverlay : uint32_t
{
    FpsOverlay_JustFPS,
    FpsOverlay_Simple,
    FpsOverlay_Detailed,
    FpsOverlay_DetailedGraph,
    FpsOverlay_Full,
    FpsOverlay_FullGraph,
    FpsOverlay_ReflexTimings,
    FpsOverlay_COUNT,
};

// Output scaling downscaler
enum class Scaler : uint32_t
{
    FSR1 = 0,
    Bicubic = 1,
    CatmullRom = 2,
    Lanczos2 = 3,
    Lanczos3 = 4,
    Kaiser2 = 5,
    Kaiser3 = 6,
    Magic = 7,
    Count
};

enum class ForceReflex : uint32_t
{
    InGame,
    ForceDisable,
    ForceEnable,
    Count
};

enum class LFXMode : uint32_t
{
    Conservative,
    Aggressive,
    ReflexIDs,
    Count
};

enum class LowLatencyInput : uint32_t
{
    None,
    Auto,
    AntiLag2,
    Reflex,
    XeLL,
    UeLowLatency,
    OptiScaler, // OptiScaler's own Streamline (DLSSG output) when the game has no low latency of its own
    _
};

enum class LowLatencyMode : uint32_t
{
    None,
    Auto,
    LatencyFlex,
    AntiLag2,
    XeLL,
    AntiLagVk,
    Reflex,
    FpsLimiter,
    _
};

class Config
{
  public:
    Config();

    // Init flags
    CustomOptional<bool, NoDefault> DepthInverted;
    CustomOptional<bool, NoDefault> AutoExposure;
    CustomOptional<bool, NoDefault> HDR;
    CustomOptional<bool, NoDefault> JitterCancellation;
    CustomOptional<bool, NoDefault> DisplayResolution;
    CustomOptional<bool, NoDefault> DisableReactiveMask;
    CustomOptional<float> DlssReactiveMaskBias { 0.45f };

    // Logging
    CustomOptional<bool> LogToFile { false };
    CustomOptional<bool> LogToConsole { false };
    CustomOptional<bool> LogToDebug { false };
    CustomOptional<bool> LogToNGX { false };
    CustomOptional<bool> D3D12Dred { false }; // [Log] D3D12 device-removed breadcrumbs on the D3D11 bridge's device
    CustomOptional<bool> CrashDump { false }; // [Log] write OptiScaler_crash.dmp when the game crashes (read once at start)
    CustomOptional<bool> OpenConsole { false };
    CustomOptional<bool> DebugWait { false }; // not in ini
    CustomOptional<int> LogLevel { 0 };
    CustomOptional<std::wstring> LogFileName { L"OptiScaler.log" };
    CustomOptional<bool> LogSingleFile { true };
    CustomOptional<bool> LogAsync { false };
    CustomOptional<int> LogAsyncThreads { 4 };

    // XeSS
    CustomOptional<bool> BuildPipelines { true };
    CustomOptional<int32_t> NetworkModel { 0 };
    CustomOptional<bool> CreateHeaps { true };

    // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) --- removable as one block -----------------
    // DLSS Neural Rendering: a detail-synthesis pass over the upscaler's output. Off by default -- it is
    // an undocumented feature driven directly through its snippet, not something NVIDIA exposes.
    CustomOptional<bool> DlssNrEnabled { false };
    // Run the NR pass on the upscaler's colour input, at render resolution, immediately before SR.
    // Off preserves the v0.2.0 post-upscale placement. Ray Reconstruction already denoises the frame
    // before either NR seam runs, so this has no effect while RR is active -- RR always forces
    // post-SR placement regardless of this setting.
    CustomOptional<bool> DlssNrRunBeforeSr { false };
    CustomOptional<bool> DlssNrFinishedPicture { false };
    // Generate NR before SR, upscale its signed contribution with a private DLSS feature,
    // and apply it after the game's upscaler. Takes precedence over RunBeforeSR; opt-in.
    CustomOptional<bool> DlssNrDeferredDlss { false };
    CustomOptional<bool> DlssNrResidualFg { false }; // forced off: not read from the ini, not in the menu
    CustomOptional<uint32_t> DlssNrPrecision { 0 }; // 0 NVIDIA FP8 (default), 4 Experimental NVFP4 hybrid
    CustomOptional<uint32_t> DlssNrVitEvery { 2 };  // NVIDIA model: compute the ViT bottleneck every N-th frame (1 = always; 2 = every other frame, default), reuse it in between
    CustomOptional<uint32_t> DlssNrVitEveryPlain { 2 }; // the same for the plain fp16 kernel set (used by some modified DLSS-NR DLLs); VitEvery is the fp8 set
    // Reuse detail between frames (dlssnr/DlssNrDetailReuse.h, shaders/dlssnr/DlssNr_DetailReuse.inl): run the model
    // every other frame and move the last frame's detail onto the new input in between. D3D12, NR after SR only, and
    // off while frame generation is on. Experimental, off by default.
    CustomOptional<bool> DlssNrDetailReuse { false };
    CustomOptional<bool> DlssNrDetailReuseDebug { false }; // paint where the moved detail was dropped (magenta)
    // Full frames: how far the model's new detail is pulled toward the moved previous detail (0..1), so full and
    // reused frames differ less. 0 = off.
    CustomOptional<float> DlssNrDetailReuseSteady { 0.0f };
    // The model every N frames: 2 (every other frame) or 3 (two reused frames after each full one).
    CustomOptional<uint32_t> DlssNrDetailReuseInterval { 2 };
    // Frames the jump of the model's new detail is spread over, on the display only (dlssnr/DlssNrDetailReuse.h):
    // 0 = auto (every other frame: 1, none, as before; every 3rd frame: 3), 1 = none, 2..3.
    CustomOptional<uint32_t> DlssNrDetailReuseSpread { 0 };
    // Reused frames: where the moved detail was dropped, how much of the trusted detail of neighbours on the same
    // surface goes in instead (0..1). Stops dropped areas flashing to the un-NR'd image with several passes.
    CustomOptional<float> DlssNrDetailReuseFill { 1.0f };
    // How far a moved sample is trusted (shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl, MovedFrom):
    // trust = depthTrust * colourTrust, and these four set both halves. Defaults are the values the feature shipped
    // with, which have never been measured against a game; they are here so that can finally be done in one, without
    // a rebuild. Moving them changes the picture on reused frames, and on full frames too wherever Steady is on,
    // since Steady pulls the full frame toward the same estimate.
    CustomOptional<float> DlssNrDetailReuseDepthTolerance { 0.051f }; // relative depth outside the saved 2x2 range still trusted fully; trust is gone at twice it. Fill uses three times it as its neighbour depth window, so this widens that too. Raised from 0.02 on 2026-10-01, see DlssNr_DetailReuseConstants.h
    CustomOptional<float> DlssNrDetailReuseClipGamma { 1.25f };      // half-width of the colour box around the current 3x3 mean, in standard deviations
    CustomOptional<float> DlssNrDetailReuseClipFalloff { 1.0f };     // distance outside that box, in standard deviations, over which trust fades to zero
    CustomOptional<float> DlssNrDetailReuseSigmaFloor { 0.01f };     // smallest standard deviation used (proxy units), so flat areas do not reject on noise
    // Keep reusing detail while frame generation is on. On by default since the edges of the screen stopped flickering
    // in fast motion (DlssNrDetailReuseMaxDropped): before that, reuse turned itself off here, which also meant its
    // saving quietly disappeared for anyone using frame generation. Full and reused frames still cost differently, so
    // the frame times alternate; a limiter just below the average rate evens them out.
    CustomOptional<bool> DlssNrDetailReuseWithFg { true };
    // Reuse runs only while the rendered frame rate is at least this (0 = no minimum): at low frame rates things move
    // farther between frames and the moved detail trails.
    CustomOptional<float> DlssNrDetailReuseMinFps { 25.0f };
    // Reuse pauses while more than this much of the picture (percent) arrives with no detail to move -- running or
    // turning fast brings in more from off-screen than Fill reaches into, and those frames flicker at the edges. It
    // comes back once the share has stayed at or under this for 0.3 s. 0 = never paused. Above 50 nothing would ever
    // pause, so that is the top of the range. (DlssNrDetailReuse::MotionGuard; the default is from in-game
    // measurements: a Witcher 3 run measured 3% while calm and 21% while running.)
    CustomOptional<float> DlssNrDetailReuseMaxDropped { 10.0f };
    // Scene cuts the game does not flag (dlssnr/DlssNrSceneCut.h, shaders/dlssnr/DlssNr_SceneCut.inl), D3D12 game input:
    // 0 off, 1 found and counted only (log, menu; the picture is untouched), 2 also acted on (Reuse detail drops the
    // moved detail on the cut frame, and the model resets when the answer arrives, a few frames later). 1 by default
    // until in-game counts show how often games leave cuts unflagged and that nothing else reads as one.
    CustomOptional<uint32_t> DlssNrSceneCut { 1 };
    // Edit cache (dlssnr/DlssNrEditCache.h, shaders/dlssnr/DlssNr_EditCache.inl): run the model one frame in N and show
    // its last answer, carried along the game's vectors, in between. D3D12, NR after SR only; not together with
    // DetailReuse (the menu's Reuse detail offers one or the other). Experimental, off by default.
    CustomOptional<bool> DlssNrEditCache { false };
    CustomOptional<uint32_t> DlssNrEditCacheInterval { 4 };  // the model every N frames, 2..8
    CustomOptional<uint32_t> DlssNrEditCacheCrossfade { 0 }; // frames a refresh takes to reach the new answer, 1..8;
                                                             // 0 = follow the interval (DlssNrEditCache::CrossfadeFor)
    CustomOptional<uint32_t> DlssNrEditCacheDebug { 0 };     // 1: dropped pixels red, 2: the edit's age
    CustomOptional<bool> DlssNrResidualFgApproxCamera { false }; // forced off, as DlssNrResidualFg
    // Toggles the pass in game. Unbound by default -- a key that does something unexpected is worse
    // than one that does nothing.
    CustomOptional<int> DlssNrToggleKey { UnboundKey };
    // HDR output: flips [HDR] Convert live (the A/B against how Windows shows the game). Unbound by default.
    CustomOptional<int> HdrCompareKey { UnboundKey };
    // The model's render preset hint: 1 Preset A (the main model), 2 Preset B (the small model, 310.9 DLLs only); 0 and
    // any other value: the DLL's choice, the small model on 310.9 DLLs. 310.8 DLLs have only the main model.
    CustomOptional<uint32_t> DlssNrPreset { 0 };
    CustomOptional<float> DlssNrIntensity { 1.0f };
    // 0 default (standard), 1 natural, 2 cinematic -- the model's own processing profiles.
    CustomOptional<uint32_t> DlssNrStyle { 0 };
    // Optional per-pass model profiles. Pass 1 uses Preset/Style above; an absent override inherits
    // pass 1. Keeping inheritance explicit preserves every existing configuration and lets changing
    // the base profile update the whole stack unless a later pass was deliberately specialised.
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass2Style;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Preset;
    CustomOptional<uint32_t, NoDefault> DlssNrPass3Style;
    CustomOptional<float> DlssNrLocalStructure { 1.0f };
    CustomOptional<float> DlssNrLocalTone { 1.0f };
    // -1 means follow local structure, which is the model's own default. It is not a strength of zero.
    CustomOptional<float> DlssNrSkinStructure { -1.0f };
    CustomOptional<bool> DlssNrAutoMask { true };
    // Optional final-composition filter, not NVIDIA's semantic auto mask.
    CustomOptional<bool> DlssNrSkinProtection { false };
    CustomOptional<bool> DlssNrSkinToneEnabled { true };
    CustomOptional<float> DlssNrSkinDetail { 1.0f };
    CustomOptional<float> DlssNrSkinColour { 1.0f };
    CustomOptional<float> DlssNrEnvironmentDetail { 1.0f };
    CustomOptional<float> DlssNrEnvironmentColour { 1.0f };
    CustomOptional<bool> DlssNrShowSkinMask { false };
    CustomOptional<float, NoDefault> DlssNrPass2Intensity;
    CustomOptional<float, NoDefault> DlssNrPass2LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass2LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass2SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass2AutoMask;
    CustomOptional<float, NoDefault> DlssNrPass3Intensity;
    CustomOptional<float, NoDefault> DlssNrPass3LocalStructure;
    CustomOptional<float, NoDefault> DlssNrPass3LocalTone;
    CustomOptional<float, NoDefault> DlssNrPass3SkinStructure;
    CustomOptional<bool, NoDefault> DlssNrPass3AutoMask;
    CustomOptional<bool> DlssNrUnlockPasses { false };
    struct NrExtraPass
    {
        CustomOptional<uint32_t, NoDefault> style;
        CustomOptional<float, NoDefault> intensity, structure, tone, skin;
        CustomOptional<bool, NoDefault> autoMask;
    };
    NrExtraPass DlssNrExtraPasses[27]; // pass 4..30; legacy pass 2/3 keys stay compatible

    // How much of the model's edit reaches the frame. Separated because detail synthesis is a luminance
    // edit and any colour shift is usually the part you do not want, and allowed past 1.0 because
    // exaggerating an edit is the only honest way to see whether there is one.
    CustomOptional<float> DlssNrTransferStrength { 1.0f };
    CustomOptional<float> DlssNrColourStrength { 1.0f };

    // Replace modes only (DlssNrReversibleMode 2/4): how much native high-frequency detail is
    // restored below 100% model resolution, where Replace has no native-resolution fallback the
    // way the composed modes do. 0 = today's behaviour, unchanged.
    CustomOptional<float> DlssNrReplaceDetailStrength { 0.5f };

    // The proxy curve (Final Image Composition), values in shaders/dlssnr/DlssNr_ProxyCurve.h. 0 = soft
    // knee + our composition (default); 1/2 = unclipped Neutwo proxy + composition / pure-inverse replace;
    // 3/4 = the balanced (hybrid) curve, the same two ways; 5 HLG and 6 PQ + composition; 7 linear +
    // composition (what game integrations hand the model). Out-of-range values fall back to 0.
    CustomOptional<uint32_t> DlssNrReversibleMode { 0 };

    // How the NR pass decodes the game's colour. 0 Auto (the game's DLSS HDR flag + the output format, as before),
    // 1 linear HDR, 2 tone-mapped sRGB, 3 tone-mapped gamma 2.2, 4 PQ. Values are DlssNr_ColourEncoding.h's.
    CustomOptional<uint32_t> DlssNrColourEncoding { 0 };

    // The game's HDR paper white in nits, the white NR is given on a display-referred HDR picture (forced PQ, or Finished
    // Picture on an HDR10 / scRGB screen), where no exposure applies. 203 = ITU-R BT.2408's reference white, the value
    // used before this existed. Bounded 80..1000 at the point of use (DlssNr_ExposureState.h).
    CustomOptional<float> DlssNrDisplayPaperWhiteNits { 203.0f };

    // LUT-apply epic (dlssnr-lut-apply), Story 2: a .cube file graded onto the
    // NR input image before the model ever sees it. Empty (the default) is the feature not existing -- no
    // parse, no GPU texture, no dispatch. Accepts a path outside OptiScaler\LUTs too.
    CustomOptional<std::string> DlssNrLutFile { std::string() };
    // How much of the LUT's grade reaches the image, 0..1. Default is fully graded once a LutFile is set.
    CustomOptional<float> DlssNrLutStrength { 1.0f };

    // Whether the model's edit is applied. Off keeps the pass running (so Hold frame works) but shows
    // the clean upscaler frame -- for A/B'ing NR on/off on a frozen frame. Default true.
    CustomOptional<bool> DlssNrApplyModel { true };

    // Frame hold: freeze the NR pass's input so a live setting change re-renders the SAME frame -- the
    // only clean way to A/B our settings. A live testing toggle, not really a saved preference; off by
    // default. See dlssnr/design/frame-hold.md.
    CustomOptional<bool> DlssNrHoldFrame { false };


    // The most the pass may brighten a pixel, in Composed mode -- a detail pass has no business
    // restyling a light source, whatever the model returns. Darkening in Composed is intentionally
    // uncapped. Replace mode reads the same number but still bounds both directions (a different
    // guard, for a different reason -- see ApplyReplaceGuard's own comment in dlssnr.hlsl).
    CustomOptional<float> DlssNrMaxRatio { 2.0f };

    // How a model that worked below the frame's size is brought back. 0 classic, 1 matched
    // residual. Only has an effect when Model resolution is under 100%.
    CustomOptional<uint32_t> DlssNrTransfer { 1 }; // 11: Superior Scaling
    CustomOptional<uint32_t> DlssNrSuperiorScalingPreset { 1 }; // private SR A=1 / K=11
    CustomOptional<bool> DlssNrSuperiorResidualDlss { true };   // experimental: skip private SR, keep NR/guided shaping
    CustomOptional<bool> DlssNrSuperiorSkipMvUnits { false };
    CustomOptional<bool> DlssNrSuperiorSkipInterPass { false };
    CustomOptional<bool> DlssNrSuperiorSkipCarrier { false };
    CustomOptional<bool> DlssNrSuperiorSkipGuided { false };
    CustomOptional<bool> DlssNrSuperiorSkipFinalShaping { false };
    CustomOptional<bool> DlssNrSuperiorSkipCompressionLow { false };
    CustomOptional<bool> DlssNrSuperiorSkipWarpCache { false };
    CustomOptional<bool> DlssNrSuperiorFullGuided { false };

    // Measure the white point from the frame instead of taking it from the slider. On a frame the
    // game already tone mapped there is nothing to measure and this has no effect.
    //
    // Off by default, because it is not finished. The pass writes its result back into the same buffer
    // the meter reads, so with the pass running the meter is partly measuring its own output and the
    // two chase each other: Enshrouded, one session, 1545 samples spanning 0.01 to 97.9 with 57 jumps
    // beyond 1.5x in a single frame. Measured in the same spot seconds apart, 41.31 with the pass off
    // against 0.46 with it on. That is visible as the picture pumping and occasionally flickering.
    //
    // The slider is the supported control until the loop is broken. This stays as an opt-in so the
    // behaviour can still be looked at.


    // Take the white point from the game's own exposure texture instead of measuring or guessing.
    // Off by default until it has been seen to work in more than one game.
    CustomOptional<bool> DlssNrWhitePointFromExposure { true };

    // Ask the model, once, whether it will run on Direct3D 11 without the bridge.
    //
    // Off by default and deliberately so. Everything else this pass does reads memory it already owns;
    // this one initialises an NVIDIA subsystem on the game's live D3D11 device, in a process where the
    // D3D12 NGX instance is already running. It should return an error code and nothing more, but
    // "should" is doing work in that sentence and it ships into games nobody can test first.
    CustomOptional<bool> DlssNrProbeD3D11 { false };

    // Diagnostic: once every 120 frames, log what the frame handed to NR looks like (format, luminance
    // percentiles over a 64x64 tile grid in scene units, the game's exposure texture, the white point in
    // force and the sRGB brightness the proxy would have). Costs two tiny dispatches and two 16 KB
    // readbacks per sample; off by default.
    CustomOptional<bool> DlssNrFrameStats { false };

    // Diagnostic: log which NVIDIA kernels an NR evaluation launches (fp8-named or the plain fp16 ones) and how the GPU
    // time splits between kernel groups. 3 evaluations out of every 240 are timestamped; off by default.
    CustomOptional<bool> DlssNrKernelProfile { false };

    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified.
    CustomOptional<uint32_t> DlssNrDebugView { 0 };

    // Showing the pass against itself, without having to toggle it and remember what the last frame
    // looked like. 0 off, 1 side by side, 2 a wipe.
    //
    // Side by side squeezes the whole frame into each half, so it is a comparison rather than
    // something to play in. The wipe cuts one frame and resamples nothing, so it is; the split is a
    // stored setting and stays where it was put once the menu closes.
    CustomOptional<uint32_t> DlssNrCompare { 0 };
    CustomOptional<float> DlssNrCompareSplit { 0.5f };

    // Side by side only. 1 fits the whole frame at its right shape and accepts the bars; 2 fills
    // the half and crops the sides off instead.
    CustomOptional<float> DlssNrCompareZoom { 1.0f };

    // Which side the edited frame sits on, in both comparison modes.
    CustomOptional<bool> DlssNrCompareSwap { false };

    // Labels drawn onto the two sides of a comparison, so a screenshot still says which is which.
    // Drawn into the frame's own plane with a clip per side: in the wipe they are revealed and hidden
    // by the split exactly as the images are, and there is nothing to drag.
    CustomOptional<bool> DlssNrCompareTags { false };
    CustomOptional<float> DlssNrTagScale { 1.5f };

    // The fraction of the frame's resolution the model works at. The frame itself is never reduced --
    // only the model's contribution is computed small and enlarged, so the picture underneath is
    // untouched whatever this is set to. 1.0 is full resolution and behaves exactly as before. Below
    // 1.0 the model's answer is enlarged back to native with the filter chosen by
    // DlssNrReducedUpscaleMethod before the resolve. Above 1.0 the answer is averaged back down with
    // DlssNrScalingDownscaler's chosen filter, as before.
    CustomOptional<float> DlssNrWorkingScale { 1.0f };

    // Below 100% model resolution only: how the model's answer is enlarged back to native
    // before the resolve. 0 = Bilinear -- the pre-SGSR1 implicit sampler tap the resolve
    // already falls back to whenever SGSR1 can't build; cheapest, softest. 1 = SGSR1
    // (default) -- edge-directed upscale on the answer. No effect at 100% model resolution
    // or above. Out-of-range persisted values are clamped to 0 on load (Config.cpp).
    //
    // The proxy (model input) was previously also enlargeable with SGSR1 (old methods 2/3),
    // sharper in Composed but only when applied symmetrically to both textures -- applied to
    // just one side it fed a filter mismatch into the model/proxy comparison instead of real
    // detail, and had no effect on Replace at all (Replace never reads the proxy). Removed
    // rather than kept as an unclear trade-off; see plans/2026-09-18-dlssnr-enlarge-filter-simplify.md.
    CustomOptional<uint32_t> DlssNrReducedUpscaleMethod { 1 };

    // Experimental input styles: how the picture the model is shown is restyled before the model runs, as one card in
    // the menu (shaders/dlssnr/DlssNr_InputStyles.h). They run in this order, each on the one before's result:
    // DlssNrInputFlatten: 0 = off (default), 1 = watercolour (a Kuwahara filter: texture flattened into washes, edges
    // kept), 2 = smooth canvas (a cross bilateral filter: skin like an untextured old-game surface, contours kept);
    // its strength blends with the picture as it was (0..1).
    // DlssNrInputStructureHint: local contrast raised on luminance, CAS-like, overshoot bounded; strength 0..3.
    // DlssNrInputStyleFacesOnly: both only where the colour looks like skin (the same colour test as
    // SkinProtection). The resolve compares the answer against the same restyled picture, so Composed hands back only
    // what the model painted; Replace shows the model's painting as it is. Skipped at model resolution above 100% and on
    // the deferred path. The ini also keeps the old single InputStyle / InputStyleStrength (read when none of the new
    // keys is set, and written as the first style that runs, for a build from before).
    CustomOptional<uint32_t> DlssNrInputFlatten { DlssNrInput::kFlattenOff };
    CustomOptional<float> DlssNrInputFlattenStrength { 1.0f };
    CustomOptional<bool> DlssNrInputStructureHint { false };
    CustomOptional<float> DlssNrInputStructureStrength { 1.0f };
    CustomOptional<bool> DlssNrInputStyleFacesOnly { false };

    DlssNrInput::Styles DlssNrInputStyles() const
    {
        DlssNrInput::Styles s;
        s.flatten = DlssNrInputFlatten.value_or_default();
        s.flattenStrength = DlssNrInputFlattenStrength.value_or_default();
        s.structure = DlssNrInputStructureHint.value_or_default();
        s.structureStrength = DlssNrInputStructureStrength.value_or_default();
        s.facesOnly = DlssNrInputStyleFacesOnly.value_or_default();
        return s;
    }

    void SetDlssNrInputStyles(const DlssNrInput::Styles& s)
    {
        DlssNrInputFlatten = s.flatten;
        DlssNrInputFlattenStrength = s.flattenStrength;
        DlssNrInputStructureHint = s.structure;
        DlssNrInputStructureStrength = s.structureStrength;
        DlssNrInputStyleFacesOnly = s.facesOnly;
    }

    // Post-SR placement only: derive the working scale from the render:output ratio the upscaler
    // itself already reconstructed detail at, instead of the manual slider above. That output already
    // reconstructed detail at that ratio, so running NR at the same reduced scale on it is effectively
    // free -- no manual tuning needed. Has no effect while NR runs before SR (RunBeforeSr on, and not
    // overridden by Ray Reconstruction); the manual slider applies as usual.
    CustomOptional<bool> DlssNrModelResolutionAuto { false };

    // Compress screen edges (shaders/dlssnr/DlssNr_Spatial.h): NR works on a smaller picture whose middle stays 1:1 and
    // whose edges are squeezed, and the answer is stretched back. Off by default. Centre and work are percentages of the
    // frame on each axis (centre < work <= 100); offset moves the centre band and shift moves pixels between the two
    // edges, both in percent of the frame. Out-of-range values leave compression off (the menu says why).
    CustomOptional<bool> DlssNrSpatialCompression { false };
    CustomOptional<float> DlssNrSpatialCenterX { 80.0f };
    CustomOptional<float> DlssNrSpatialCenterY { 80.0f };
    CustomOptional<float> DlssNrSpatialWorkX { 90.0f };
    CustomOptional<float> DlssNrSpatialWorkY { 90.0f };
    CustomOptional<float> DlssNrSpatialOffsetX { 0.0f };
    CustomOptional<float> DlssNrSpatialOffsetY { 0.0f };
    CustomOptional<float> DlssNrSpatialShiftX { 0.0f };
    CustomOptional<float> DlssNrSpatialShiftY { 0.0f };

    // Filter used for NR supersampling (working scale > 1): the model runs above native, and this is
    // the downscaler that averages its answer back to native. Independent of OutputScalingDownscaler
    // so NR and Output Scaling can run different filters at once. Lanczos3 is the sharp default.
    CustomOptional<Scaler> DlssNrScalingDownscaler { Scaler::Lanczos3 };

    // Ask the driver's own nvngx.dll whether it will dispatch Neural Rendering, once per session.
    //
    // Everything here drives the model's DLL directly through a forwarder, because the model refuses
    // callers whose module path does not contain "nvngx.dll". But the model ships inside the driver
    // store, and NVIDIA does not ship a feature DLL that no dispatcher can reach -- so the driver's
    // nvngx.dll may well know feature 18 already. If it does, the forwarder is unnecessary, the
    // signature question disappears, and users stop needing a 165 MB copy in every game folder.
    //
    // Off by default: it is a diagnostic, not a feature.
    CustomOptional<bool> DlssNrProxyProbe { false };

    // Run Neural Rendering through the driver's own nvngx.dll rather than through the forwarder.
    //
    // This is how DLSS itself is called. The forwarder exists only because driving the model
    // directly trips its caller check, and a probe showed the driver dispatches feature 18 already:
    // asking for 18 answers differently from asking for a feature that does not exist. OptiScaler
    // also already tells the driver where to look, since NVNGX_FeatureInfo_Paths carries the game
    // and OptiScaler folders into Init_Ext.
    //
    // Off until it is shown to produce the same picture. If it does, the forwarder can go.
    CustomOptional<bool> DlssNrUseProxy { false };

    // Force the exposure scan on (it looks for the exposure the game computed but never handed to the upscaler), for
    // diagnosis: the log compares every candidate, and nothing it finds moves the picture through this flag.
    //
    // The scan also runs without it: the white point source Auto, the default, searches in a D3D12 game that hands over
    // no exposure, and a match it finds moves the white point (user decision 2026-10-19/20; DlssNr_ExposureAuto.h).
    // That carries the risk this flag once kept opt-in: reading a resource the game owns means assuming what state it
    // is in, and unlike depth and motion vectors -- where NGX documents the contract -- a buffer found by its shape
    // comes with no promise at all. UNORDERED_ACCESS is the reasonable assumption, since every candidate got here by
    // having a UAV made on it. Auto keeps the exposure short: it lets go of every candidate it has finished with (a
    // game's teardown with its resources still pinned removed the device in Cyberpunk 2077), and of all of them in a
    // game that hands its exposure over. Under Auto this flag does not stop that.
    CustomOptional<bool> DlssNrScanExposure { false };

    // Write the exposure scan's recording (<log>.exposurescan.csv: every candidate beside NR's brightness reading) for
    // diagnosis. Turns the scan on. Separate from ScanExposure so a scan that runs for the picture never writes a file.
    CustomOptional<bool> DlssNrRecordExposureScan { false };

    // Where the white point comes from. One control, because there is one answer.
    //
    //   0  the paper white slider, and nothing else
    //   1  the exposure the game hands the upscaler
    //   2  a buffer the scan found, anchored to a white point the user chose once
    //   3  OptiScaler automatic exposure calculated from the original linear-HDR frame
    //
    // This replaces two independent checkboxes that could both be on. They were made exclusive by
    // greying, which deadlocked -- each disabled the other, so once both were set the only way out
    // was a button the notice never mentioned -- and then by clearing, which silently undid a
    // setting the user had made. Both were attempts to stop an illegal state being REACHED. A single
    // choice cannot reach it: there is nothing to keep consistent, because there is only one value.
    //   4  auto (the default): Game exposure when the game hands one over, else Automatic (user decision 2026-10-14),
    //      with Game exposure's scale per game (GameExposureScale auto). shaders/dlssnr/DlssNr_ExposureDefault.h; every
    //      reader goes through DlssNr::WhitePointSourceEffective (DlssNr_GameDefaults.h).
    // Before 2026-10-14 the default was 3 (Automatic, at +1.5 EV): Game exposure on the game's-setting scale left NBA
    // 2K27 far too dark, which the per-game scale now covers.
    CustomOptional<uint32_t> DlssNrWhitePointSource { 4 };

    // Game exposure's scale: what its Trim multiplies. 0 = with the game's exposure (PreExposure / exposure, the value
    // the game reports to DLSS SR), 1 = the game's colour as is (a base of exactly 1, as games with built-in DLSS-NR
    // feed the model), 2 = auto (the default): as is for a game measured to need it (NBA 2K27), else the game's
    // setting. Only read with Game exposure in force. See DlssNr_GameScale.h and DlssNr_ExposureDefault.h.
    CustomOptional<uint32_t> DlssNrGameExposureScale { 2 };

    // OptiScaler-owned automatic exposure controls. When active, automatic exposure uses the
    // linear-HDR NR input. Finished-picture mode bypasses this calculation and keeps its own
    // display white-point override.
    // AutoExposureTrim is a multiplier on the white point (higher = darker model input). While unset (ini `auto`) the
    // Trim used is the default (DlssNrAutoTrim::Effective, shaders/dlssnr/DlssNr_AutoTrimDefault.h):
    // 1.77 (+1.5 EV) for every game; the 5.0 below is only the menu's 0 EV
    // point. The menu shows it as "Model input brightness" in stops around 5x: EV = -log2(trim / 5), so 0 EV = 5x, + is brighter.
    // AutoExposureShadowProtection is the menu's "Ignore bright highlights" (percent).
    CustomOptional<float> DlssNrAutoExposureTrim { 5.0f };
    CustomOptional<float> DlssNrAutoExposureShadowProtection { 100.0f };
    // AutoExposureMeter is the menu's "Meter": 0 the plain average with the highlight knee above (AutoExposureShadowProtection),
    // 1 the log-average between two percentiles of brightness, as Unreal and Unity HDRP meter (the Low / High percent are the
    // window). Default 1 (user decision 2026-10-03, after NBA 2K27: "looking good, the image quality even upped"); an ini
    // that sets 0 keeps the plain average. See shaders/dlssnr/DlssNr_ExposureMeter.h.
    CustomOptional<uint32_t> DlssNrAutoExposureMeter { 1 };
    CustomOptional<float> DlssNrAutoExposureMeterLowPercent { 10.0f };
    CustomOptional<float> DlssNrAutoExposureMeterHighPercent { 90.0f };
    // NativeDepthFinder: watch the game's depth buffers and pick the scene's, the first stage of DLSS-NR without a game
    // upscaler call (D3D11 and D3D12; one key for both, which API runs is decided by which device the game creates).
    // Observes only. Startup only. Optional: without it NativeInput/NativeUpscaler run on motion only. It is also
    // what notices the game calling its own upscaler (GameCallsUpscaler); without it nothing stands aside for that.
    // Turned on and off by the menu's mode selector (dlssnr/DlssNr_NativeMode.h), and set on its own by "Use the
    // game's depth" under the menu's Advanced section. See resource_tracking/GenericDepth_Dx12.h and
    // GenericDepth_Dx11.h.
    CustomOptional<bool> DlssNrNativeDepthFinder { false };
    // NativeDepthVkCopyUsage: Vulkan only. With NativeDepthFinder on, depth images the game makes for rendering are made
    // copyable (TRANSFER_SRC added to their usage), or the finder could not copy the scene's depth: a Vulkan game seldom
    // asks for that itself. Can cost a little depth compression on some GPUs. Off: Vulkan depth reads as not readable,
    // and Optical F5Low runs on motion only. Startup only (images are made once). See resource_tracking/GenericDepth_Vk.h.
    CustomOptional<bool> DlssNrNativeDepthVkCopyUsage { true };
    // NativeDepthResolveInList: D3D12 only. With NativeDepthFinder on, a multisampled depth buffer is resolved inside
    // the game's own command list (ResolveSubresourceRegion MIN/MAX) where the device and format allow it, instead of
    // a full multisampled copy and a compute resolve afterwards. Off: always the copy and compute resolve, for a driver
    // that resolves depth wrongly (flat depth in the finder's preview). See native/DepthSnapshotDx12.h.
    CustomOptional<bool> DlssNrNativeDepthResolveInList { true };
    // NativeDxvkVulkan: experimental. A D3D11 game running on dxvk (doitsujin/dxvk: its D3D11 calls become Vulkan ones)
    // normally gets Optical F5Low's D3D11 driver (native/NativeDriverDx11.h), the same shared-texture bridge to a private
    // D3D12 device a real D3D11 game gets. On: that D3D11 driver stands aside, and the Vulkan driver (native/NativeDriverVk.h,
    // resource_tracking/GenericDepth_Vk.h -- the same code a native Vulkan game uses) reads dxvk's own Vulkan calls instead,
    // and frame generation (FGInput=Upscaler) goes through the Vulkan present bridge (native/VkPresentBridge.h) on dxvk's
    // Vulkan swapchain, its D3D12 swapchain from Windows' own DXGI, instead of the D3D11 bridge, which cannot work on dxvk
    // (dxvk's shared handles do not open on a native D3D12 device). Off by default. See misc/IdentifyGpu.h (usesDxvk).
    CustomOptional<bool> DlssNrNativeDxvkVulkan { false };
    // NativeDepthWarmupFrames: presented frames to watch before the finder trusts that the game makes no upscaler call of its
    // own (it stands down for good the moment one is seen). NativeDepthOverlay: debug; copies the picked depth buffer at its
    // busiest clear and shows it in the DLSS-NR menu (D3D12 only; no preview round-trip exists for D3D11 yet). That copy is
    // recorded into the game's own command list.
    CustomOptional<uint32_t> DlssNrNativeDepthWarmupFrames { 300 };
    CustomOptional<bool> DlssNrNativeDepthOverlay { false };
    // NativeDebugView: show the pictures of the depth finder and the motion estimate on the menu's Optical F5Low page
    // (the depth preview, the motion and trust pictures, the trust view chooser; D3D12 only), set by "Show the motion
    // and trust pictures" there. The controls (flow tuning, the depth overlay switch) show either way. Off, the depth
    // overlay's copy is not made (read at the start for that).
    CustomOptional<bool> DlssNrNativeDebugView { false };
    // NativeMotionProfile: the Optical F5Low page's "Motion profile", a named set of the motion estimate's knobs for a
    // kind of game (motion/MotionProfiles.h): default, anime, realistic, sportswide, sportspan or custom. Custom is
    // NativeMotionProfileCustom, the Flow tuning menu's values as a key=value list (written when a Flow tuning control
    // moves). Applied when the motion estimate starts and when the menu changes it.
    CustomOptional<native::motion::Profile> DlssNrNativeMotionProfile { native::motion::Profile::Default };
    CustomOptional<std::string> DlssNrNativeMotionProfileCustom { std::string() };
    // NativeMotion: estimates the optical flow of the finished picture on the GPU while the game makes no upscaler
    // call, feeding NativeInput/NativeUpscaler below (on its own it estimates but feeds nothing). Changes apply at
    // once. Set by the menu's mode selector (dlssnr/DlssNr_NativeMode.h). See native/NativeDriverDx12.h and
    // NativeDriverDx11.h.
    CustomOptional<bool> DlssNrNativeMotion { false };
    // NativeInput: with NativeMotion on and the game making no upscaler call, runs DLSS-NR on the finished picture with
    // the optical flow as its motion and the depth finder's depth when it has one (Story 4 of the native input
    // producer). Needs FinishedPicture and Enabled on as well. Lower GPU cost than NativeUpscaler below, with no frame
    // generation. Once FGInput=Upscaler has replaced a D3D11 or Vulkan game's swap chain it runs from that swap chain's
    // present (frame generation idles: nothing feeds it). The menu's "NR only" mode. Changes apply at once.
    CustomOptional<bool> DlssNrNativeInput { false };
    // NativeUpscaler: experimental. With NativeMotion on and the game making no upscaler call, presents the picture,
    // the finder's depth (all far when it has none) and the optical flow to OptiScaler's upscaler (Dx12Upscaler, FFX
    // when unset) as a synthetic upscaler call (render size == output size, no jitter: no reconstruction; the backend's
    // pass runs only with NativeUpscalerPass true), so frame generation with the Upscaler input works in a
    // game that never calls an upscaler. Takes
    // priority over NativeInput. Changes apply at once. In a D3D11 game, FGInput=Upscaler replaces the game's swap chain
    // with with_dx12::Dx11wDx12SC; this and NativeInput then run from its present (native/NativeDriverDx11.cpp's
    // OnFGPresent, with_dx12/dx11_with_dx12_sc.cpp's _ApplyNativeInputToFGBackBuffer), while Finished Picture NR (the
    // D3D12 swap chain path) does nothing there. The menu's "NR + frame generation" mode (Optical F5Low page): it
    // sets NativeDepthFinder, NativeMotion and this together, clears NativeInput, and switches NR Pass at: off Finished
    // Picture if it was on (see dlssnr/DlssNr_NativeMode.h). See native/VirtualUpscalerDriver.h.
    CustomOptional<bool> DlssNrNativeUpscaler { false };
    // NativeUpscalerPass: with NativeUpscaler on, whether the upscaler backend evaluates the picture (Neural Rendering on
    // or off). Auto (unset) and false: it does not. The backend only keeps frame generation's Upscaler input alive, and
    // with NR on, NR runs on the game's picture in place, as in NativeInput (native/VirtualUpscalerMode.h). true: the
    // backend's pass, NR (when on) on its output. The menu's "Run the upscaler on the picture".
    CustomOptional<bool> DlssNrNativeUpscalerPass { false };
    // NativeFrameGenerationOnly: a Vulkan game that calls DLSS, FSR or XeSS of its own gets OptiScaler's frame generation
    // from that call. The Vulkan present bridge (native/VkPresentBridge.h) puts frame generation's D3D12 swapchain on the
    // window, and the game's upscaler runs through a Vulkan-on-D3D12 backend (DLSS_on12, FFX_on12, FSR21_on12) on the
    // bridge's device, whose evaluate hands frame generation the game's motion vectors, depth and output
    // (upscalers/IFeature_VkwDx12.cpp). No Optical F5Low motion. The Optical F5Low page's "FG only (game's upscaler)" Mode.
    CustomOptional<bool> DlssNrNativeFrameGenerationOnly { false };
    // NativeLowLatencyBoost: Reflex's Boost with it (the GPU keeps its clocks up between frames), also for DLSS frame
    // generation's Streamline Reflex on an Optical F5Low game. NVIDIA Reflex only: XeLL, Anti-Lag 2 and LatencyFlex
    // ignore it (XeLL: "Boost is not supported as of now").
    CustomOptional<bool> DlssNrNativeLowLatencyBoost { false };
    // NativeLowLatencyDxvkGameThread: a D3D11 game on dxvk with the Vulkan present bridge runs Optical F5Low's low latency
    // (markers and Sleep) at the game's own D3D11 Present, on the game's thread, instead of at dxvk's Vulkan present on
    // dxvk's thread (native/NativeLowLatencyRule.h SleepSite). Read every present (no restart needed after a config
    // reload). Only dxvk through the Vulkan bridge looks at it.
    CustomOptional<bool> DlssNrNativeLowLatencyDxvkGameThread { true };
    // AutoExposureAdaptBrighterSeconds / AutoExposureAdaptDarkerSeconds are the menu's "Eye adaptation": how long
    // Automatic takes to follow a scene getting brighter / darker, as a time constant in seconds; 0 = at once. Faster to
    // brighter, as in Unreal and Unity HDRP. See shaders/dlssnr/DlssNr_ExposureAdapt.h.
    CustomOptional<float> DlssNrAutoExposureAdaptBrighterSeconds { 0.5f };
    CustomOptional<float> DlssNrAutoExposureAdaptDarkerSeconds { 1.5f };
    // The single speed both used to share: read, and copied into both new keys when they are absent. Never written.
    CustomOptional<float> DlssNrAutoExposureAdaptSeconds { 1.0f };
    // Automatic follows the game's own exposure times a calibration learned against Automatic's meter (Vulkan: a few
    // frames behind). See DlssNr_FollowGame.h. Unset (auto) = on for a known unexposed game (DlssNr_AutoTrimDefault.h
    // kUnexposedGames), off otherwise; a saved true or false is kept as it is.
    CustomOptional<bool, NoDefault> DlssNrAutoExposureFollowGame;

    // Base-white-point-dependent Trim calibration tables, serialized as baseWhitePoint:trim pairs. Ini-only: the
    // menu no longer edits them, but a table already in the ini still applies (and disables the Trim slider).
    CustomOptional<std::string> DlssNrGameExposureTrimAnchors { std::string() };
    CustomOptional<std::string> DlssNrAutoExposureTrimAnchors { std::string() };

    // Calibration preview only; deliberately not persisted. Nothing in the menu sets these any more.
    CustomOptional<bool> DlssNrGameExposureTrimPreview { false };
    CustomOptional<bool> DlssNrAutoExposureTrimPreview { false };

    // A lamp in the corner showing what the scan currently thinks the light is doing: red for dark,
    // green for full light, and the shades between. Off by default; it is for watching the thing
    // work, not for playing with.
    CustomOptional<bool> DlssNrScanMeter { false };

    // Anchoring the scan (WhitePointSource 2): the white point that looked right, and the scan's value at that moment.
    //
    // The absolute white point cannot be derived from a buffer whose units are unknown. What CAN be
    // derived is every value after the first: if the scan's number halves, the scene got twice as
    // bright, and the white point follows -- whatever the number actually means, because only the
    // ratio is used and the units cancel.
    //
    // So the user sets it once, in one lighting condition, and presses a button. After that it stays
    // correct through every cave and every noon without being touched again. Which is also the shape
    // that makes per-game profiles work: one person anchors a game, everybody else gets the number.
    //
    // Zero means not anchored, and then nothing happens at all.
    CustomOptional<float> DlssNrScanAnchorValue { 0.0f };       // legacy single anchor, migrated then unused
    CustomOptional<float> DlssNrScanAnchorWhitePoint { 0.0f };  // legacy single anchor, migrated then unused

    // The multi-point anchor table, serialised as "scan:white;scan:white;..." ascending. See
    // dlssnr/design/multi-point-anchoring.md. Replaces the single pair above; a pre-existing single
    // anchor is migrated into a one-row table on first load.
    CustomOptional<std::string> DlssNrScanAnchors { std::string() };

    // Whether the scan's number rises or falls with the light.
    //
    // A found buffer carries no contract. Most engines store an exposure -- a multiplier that goes
    // DOWN as the scene gets brighter -- but some store its reciprocal, and nothing in the buffer
    // says which. Rather than guess and be silently wrong in half the games, this is one click: if
    // the picture moves the wrong way, flip it.
    CustomOptional<bool> DlssNrScanInverted { false };







    // The trim on an exposure-derived white point, kept apart from the manual divisor on purpose.
    //
    // These are two different quantities that happened to share one slider: the manual path wants an
    // absolute divisor on an open-ended linear buffer, which in Nioh 3 is about 240, and the exposure
    // path wants a multiplier on a number the game already supplied, where anything far from 1 is
    // a sign the read is wrong rather than a preference. Sharing one stored value meant touching the
    // slider in one mode silently destroyed the number found in the other.
    //
    // 1.0 is the identity: take the game's exposure exactly as given. That is the "safe value", and
    // it is safe by construction rather than by being written down somewhere.
    // Game exposure Trim, shown in the menu as "Model input brightness": EV = -log2(trim), so 0 EV = 1x.
    CustomOptional<float> DlssNrWhitePointTrim { 1.0f };

    // The scan's trim, kept apart from the exposure texture's.
    //
    // They are trims on different things and a value found against one is meaningless against the
    // other. Sharing one slider meant switching source silently carried a number across, so a
    // picture that had been tuned came back wrong for a reason nothing on screen explained.
    CustomOptional<float> DlssNrScanTrim { 1.0f };

    // How many sequential model layers to run between one encode and one final composition. Each extra
    // layer consumes the preceding model output and owns a persistent feature/history. The implementation
    // deliberately caps this at three and never evaluates a feature on the command list that created it.
    //
    // 1 is what the model was trained for and what every published number describes. Above that it
    // is being asked to enhance its own output, which is outside its training distribution: detail
    // compounds, and so does anything it got wrong. Two often looks richer; three is the guarded
    // ceiling because further layers converge while still paying the full cost.
    //
    // The cost is exactly linear -- the model is 98% of the frame's expense and every pass pays it
    // again -- so 3 costs three times, near enough. There is no shortcut and no amortisation: the
    // passes are sequential and each one needs the last one's output.
    CustomOptional<uint32_t> DlssNrPasses { 1 };

    // How much of an interpass boundary's raw answer the next pass actually receives.
    // next_input = proxy + PassFeedback * (restored - proxy). 1.0 (default) is exactly today's
    // behaviour: the next pass gets the full restored answer, same as before this control existed.
    //
    // Every pass below this one has already stepped once further off the training distribution
    // than the model was built for (see DlssNrPasses above) -- this is the dial on how far. Lower
    // values keep each pass's input closer to a plausible "frame the model hasn't already touched",
    // at the cost of a smaller cumulative edit. Has no effect at Passes == 1 (no boundary exists).
    CustomOptional<float> DlssNrPassFeedback { 1.0f };

    // Which depth convention the model is told the guide uses.
    //
    //   0  what the game's own DLSS feature was created with, which is what it means for the upscaler
    //   1  force normal
    //   2  force inverted
    //
    // Writes one set of matched before/after frames per session, without anyone having to ask. The
    // folder is cleared at the start of each run, so it holds one session's worth and never grows.
    CustomOptional<bool> DlssNrAutoCapture { true };





    // The manual white point (WhitePointSource 0, and the fallback while Game exposure has no reading yet): what the
    // model considers "white" on a linear HDR frame. Higher means highlights sit lower on the curve and the model
    // treats them as less extreme. Not applied to a tone-mapped frame (passed through as it is) or to display HDR
    // (white at the paper white); there it only scales the debug views.
    CustomOptional<float> DlssNrWhitePointScale { 1.0f };





    // --- end DLSS 5 Neural Rendering -------------------------------------------------------------

    // DLSS
    CustomOptional<bool> DLSSEnabled { true };
    CustomOptional<bool> RenderPresetOverride { false };
    CustomOptional<uint32_t> RenderPresetForAll { 0 };
    CustomOptional<uint32_t> RenderPresetDLAA { 0 };
    CustomOptional<uint32_t> RenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> RenderPresetQuality { 0 };
    CustomOptional<uint32_t> RenderPresetBalanced { 0 };
    CustomOptional<uint32_t> RenderPresetPerformance { 0 };
    CustomOptional<uint32_t> RenderPresetUltraPerformance { 0 };

    // DLSSD
    CustomOptional<bool> DLSSDRenderPresetOverride { false };
    CustomOptional<uint32_t> DLSSDRenderPresetForAll { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetDLAA { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetQuality { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetBalanced { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetPerformance { 0 };
    CustomOptional<uint32_t> DLSSDRenderPresetUltraPerformance { 0 };

    // Nukems
    CustomOptional<bool> NvngxFGMakeDepthCopy { false };

    // Libraries
    CustomOptional<std::wstring, NoDefault> MainDllPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12Path;
    CustomOptional<std::wstring, NoDefault> FfxDx12SRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12FGPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RRPath;
    CustomOptional<std::wstring, NoDefault> FfxDx12RCPath;
    CustomOptional<std::wstring, NoDefault> FfxVkPath;
    CustomOptional<std::wstring, NoDefault> XeSSLibrary;
    CustomOptional<std::wstring, NoDefault> XeFGLibrary;
    CustomOptional<std::wstring, NoDefault> XeLLLibrary;
    CustomOptional<std::wstring, NoDefault> XeSSDx11Library;
    CustomOptional<std::wstring, NoDefault> NvngxPath;
    CustomOptional<std::wstring, NoDefault> NVNGX_DLSS_Library;
    CustomOptional<std::wstring, NoDefault> DLSSFeaturePath;
    CustomOptional<std::wstring, NoDefault> NvapiDllPath;

    // Sharpness
    CustomOptional<SharpenShader> SharpnessShader { SharpenShader::RCAS };
    CustomOptional<bool> OverrideSharpness { false };
    CustomOptional<float> Sharpness { 0.4f };

    // RCAS
    CustomOptional<bool> RcasEnabled { false };
    CustomOptional<bool> ContrastEnabled { false };
    CustomOptional<float> Contrast { -0.3f };

    // DA Sharpening
    CustomOptional<float, NoDefault> DADepthScale;
    CustomOptional<float, NoDefault> DADepthBias;
    CustomOptional<bool, NoDefault> DAClampOutput;

    // MAS
    CustomOptional<bool> MotionSharpnessEnabled { false };
    CustomOptional<bool> MotionSharpnessDebug { false };
    CustomOptional<float> MotionSharpness { 0.2f };
    CustomOptional<float> MotionThreshold { 0.0f };
    CustomOptional<float> MotionScaleLimit { 10.0f };

    // Magnifier
    CustomOptional<bool> MagnifierEnabled { false };
    CustomOptional<float> MagnifierSize { 15.f }; // % of screen Height
    CustomOptional<int> MagnifierZoomFactor { 4 };
    CustomOptional<float> MagnifierBorderSize { 0.3f };   // % of screen Height
    CustomOptional<float> MagnifierCursorOffsetX { 0.f }; // Pixels
    CustomOptional<float> MagnifierCursorOffsetY { 0.f }; // Pixels
    CustomOptional<float, NoDefault> MagnifierStaticPosX; // % of screen Width, static pos enabled if both are defined
    CustomOptional<float, NoDefault> MagnifierStaticPosY; // % of screen Height

    // Menu
    CustomOptional<float, NoDefault> MenuScale;
    CustomOptional<float, NoDefault> MenuWidth; // Main window size in pixels at Menu Scale 1.0
    CustomOptional<float, NoDefault> MenuHeight;
    CustomOptional<std::string, NoDefault> MenuPage; // Last page open in the main window, see menu/MenuPages.h
    // One toast per session pointing to the Optical F5Low page, see DlssNr::UpdateF5LowHint
    CustomOptional<bool> F5LowHint { true };
    CustomOptional<bool> OverlayMenu { true };
    CustomOptional<int> ShortcutKey { VK_INSERT };
    CustomOptional<bool> ExtendedLimits { false };
    CustomOptional<bool> ShowFps { false };
    /// 0 Top Left, 1 Top Right, 2 Bottom Left, 3 Bottom Right
    CustomOptional<FpsOverlayPos> FpsOverlayPosition { FpsOverlayPos_TopLeft };
    /// 0 Only FPS, 1 +Avg FPS & Upscaler info 2 +Frame Time,
    /// 3 +Upscaler Time, 4 +Frame Time Graph, 5 +Upscaler Time Graph
    /// 6 +Reflex timings
    CustomOptional<FpsOverlay> FpsOverlayType { FpsOverlay_JustFPS };
    CustomOptional<int> FpsShortcutKey { VK_PRIOR };
    CustomOptional<int> FpsCycleShortcutKey { VK_NEXT };
    CustomOptional<bool> FpsOverlayHorizontal { false };
    CustomOptional<float> FpsOverlayAlpha { 0.4f };
    CustomOptional<float, NoDefault> FpsScale; // No value means same as MenuScale
    CustomOptional<bool> UseHQFont { true };
    CustomOptional<bool> DisableSplash { false };
    CustomOptional<float> FontSize { 14.0f };
    CustomOptional<std::wstring, NoDefault> TTFFontPath;
    CustomOptional<int> FGShortcutKey { VK_END };
    CustomOptional<bool> LightTheme { false };
    CustomOptional<bool> ModernTheme { false };
    CustomOptional<bool> OverlaysUseTheme { false };
    CustomOptional<float> MenuAccentColorR { 0.00f };
    CustomOptional<float> MenuAccentColorG { 0.40f };
    CustomOptional<float> MenuAccentColorB { 0.77f };
    CustomOptional<float> MenuBGColorR { 0.0f };
    CustomOptional<float> MenuBGColorG { 0.0f };
    CustomOptional<float> MenuBGColorB { 0.0f };
    CustomOptional<float> MenuBGColorA { 0.99f };

    // Hooks
    CustomOptional<bool> HookOriginalNvngxOnly { false };
    CustomOptional<bool> EarlyHooking { false };
    CustomOptional<bool, NoDefault> D3D11FeatureLevelElevation; // unset: elevate unless a game quirk skips it
    CustomOptional<bool> UseNtdllHooks { true };

    // Upscale Ratio Override
    CustomOptional<bool> UpscaleRatioOverrideEnabled { false };
    CustomOptional<float> UpscaleRatioOverrideValue { 1.3f };

    // DRS
    CustomOptional<bool> DrsMinOverrideEnabled { false };
    CustomOptional<bool> DrsMaxOverrideEnabled { false };

    // Quality Overrides
    CustomOptional<bool> QualityRatioOverrideEnabled { false };
    CustomOptional<float> QualityRatio_DLAA { 1.0f };
    CustomOptional<float> QualityRatio_UltraQuality { 1.3f };
    CustomOptional<float> QualityRatio_Quality { 1.5f };
    CustomOptional<float> QualityRatio_Balanced { 1.7f };
    CustomOptional<float> QualityRatio_Performance { 2.0f };
    CustomOptional<float> QualityRatio_UltraPerformance { 3.0f };

    // ProcessFilter
    CustomOptional<std::wstring, NoDefault> TargetProcess;
    CustomOptional<std::wstring> ProcessExclusionList = {
        L"crashpad_handler.exe|crashreport.exe|crashreporter.exe|crs-handler.exe|crs-uploader.exe|crs-video.exe|"
        L"unitycrashhandler64.exe|idtechlauncher.exe|cefviewwing.exe|ace-setup64.exe|ace-service64.exe|"
        L"qtwebengineprocess.exe|platformprocess.exe|bugsplathd64.exe|bssndrpt64.exe|pspcsdkappmgr.exe|pspcsdkcore.exe|"
        L"pspcsdkstttts.exe|pspcsdktelemetry.exe|pspcsdkui.exe|pspcsdkupdatechecker.exe|pspcsdkvoicechat.exe|"
        L"pspcsdkwebview.exe|windhawk.exe|vscodium.exe|crash_reporter.exe|steamerrorreporter64.exe|crashreportclient."
        L"exe|edcefcrashpadprocess.exe|edcefrenderprocess.exe"
    };

    // Hotfixes
    CustomOptional<bool> CheckForUpdate { true };
    CustomOptional<bool, SoftDefault> DisableOverlays { false };
    // misc/REFrameworkCompat.h; auto (no value) = on in RE Engine games with the REFramework fork that follows XeFG
    CustomOptional<bool, NoDefault> REFrameworkCompat; // true / false kept as set; no value = auto

    CustomOptional<bool> SimulateWaitableObject { false };

    CustomOptional<float, NoDefault> MipmapBiasOverride; // disabled by default
    CustomOptional<bool> MipmapBiasFixedOverride { false };
    CustomOptional<bool> MipmapBiasScaleOverride { false };
    CustomOptional<bool> MipmapBiasOverrideAll { false };

    CustomOptional<int, NoDefault> AnisotropyOverride; // disabled by default
    CustomOptional<bool> OverrideShaderSampler { true };
    CustomOptional<bool> AnisotropyModifyComp { true };
    CustomOptional<bool> AnisotropyModifyMinMax { true };
    CustomOptional<bool> AnisotropySkipPointFilter { true };

    CustomOptional<int, NoDefault> RoundInternalResolution; // disabled by default

    CustomOptional<int, NoDefault> SkipFirstFrames; // disabled by default
    CustomOptional<bool> RestoreComputeSignature { false };
    CustomOptional<bool> RestoreGraphicSignature { false };
    CustomOptional<bool> ExtendedStateRestore { false };

    CustomOptional<bool> UsePrecompiledShaders { true };

    CustomOptional<bool> UseGenericAppIdWithDlss { false };
    CustomOptional<bool> PreferDedicatedGpu { true };
    CustomOptional<bool> PreferFirstDedicatedGpu { false };

    CustomOptional<int32_t, NoDefault> ColorResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> MVResourceBarrier;       // disabled by default
    CustomOptional<int32_t, NoDefault> DepthResourceBarrier;    // disabled by default
    CustomOptional<int32_t, NoDefault> ExposureResourceBarrier; // disabled by default
    CustomOptional<int32_t, NoDefault> MaskResourceBarrier;     // disabled by default
    CustomOptional<int32_t, NoDefault> OutputResourceBarrier;   // disabled by default

    CustomOptional<bool> CreateD3D12DeviceForLuma { false };

    // Upscalers
    CustomOptional<Upscaler, SoftDefault> Dx11Upscaler { Upscaler::FSR22 };
    CustomOptional<Upscaler, SoftDefault> Dx12Upscaler { Upscaler::XeSS };
    CustomOptional<Upscaler, SoftDefault> VulkanUpscaler { Upscaler::FSR22 };

    // Output Scaling
    CustomOptional<bool> OutputScalingEnabled { false };
    CustomOptional<float> OutputScalingMultiplier { 1.5f };
    CustomOptional<Scaler> OutputScalingDownscaler { Scaler::FSR1 };

    // FSR
    CustomOptional<bool> FsrDebugView { false };
    CustomOptional<int> FfxUpscalerIndex { 0 };
    CustomOptional<int> FfxFGIndex { 0 };
    CustomOptional<bool> FsrUseMaskForTransparency { true };
    CustomOptional<bool> FsrNonLinearColorSpace { false };
    CustomOptional<bool> FsrNonLinearSRGB { false };
    CustomOptional<bool> FsrNonLinearPQ { false };
    CustomOptional<bool> FsrAgilitySDKUpgrade { false };

    // These default values will be overwritten at upscaler init time with optimized values
    CustomOptional<float> FsrVelocity { 1.0f };
    CustomOptional<float> FsrReactiveScale { 1.0f };
    CustomOptional<float> FsrShadingScale { 1.0f };
    CustomOptional<float> FsrAccAddPerFrame { 0.333f };
    CustomOptional<float> FsrMinDisOccAcc { -0.333f };

    // FSR4
    // Overrides the FSR 4 support detected for the GPU
    CustomOptional<FSR4Support> Fsr4GpuSupport { FSR4Support::None };
    CustomOptional<uint32_t, NoDefault> Fsr4Preset;
    CustomOptional<FSR4Model, NoDefault> Fsr4Model;
    CustomOptional<bool> Fsr4EnableWatermark { false };
    CustomOptional<bool> Fsr4DoNotLoadAmdxc64 { false };

    // FSR Common
    CustomOptional<float> FsrVerticalFov { 60.0f };
    CustomOptional<float> FsrHorizontalFov { 0.0f }; // off by default
    CustomOptional<float> FsrCameraNear { 0.1f };
    CustomOptional<float> FsrCameraFar { 100000.0f };
    CustomOptional<bool> FsrUseFsrInputValues { true };

    // dx11wdx12
    CustomOptional<bool> Dx11DelayedInit { false };
    CustomOptional<bool> DontUseNTShared { true };

    // vulkanwdx12
    CustomOptional<bool> VulkanUseCopyForInputs { false };
    CustomOptional<bool> VulkanUseCopyForOutput { false };

    // NVAPI Override
    CustomOptional<bool> DisableFlipMetering { false };

    // Spoofing
    CustomOptional<bool, SoftDefault> DxgiSpoofing { true };
    CustomOptional<bool> DxgiFactoryWrapping { false };
    CustomOptional<bool> StreamlineSpoofing { true };
    CustomOptional<std::string, NoDefault> DxgiBlacklist; // disabled by default
    CustomOptional<int, NoDefault> DxgiVRAM;              // disabled by default
    CustomOptional<bool> VulkanSpoofing { false };
    CustomOptional<bool> VulkanExtensionSpoofing { false };
    CustomOptional<int, NoDefault> VulkanVRAM; // disabled by default
    CustomOptional<bool> SpoofHAGS { false };
    CustomOptional<bool> SpoofFeatureLevel { false };
    CustomOptional<uint32_t> SpoofedVendorId { VendorId::Nvidia };
    CustomOptional<uint32_t> SpoofedDeviceId { 0x2684 };
    CustomOptional<uint32_t, NoDefault> TargetVendorId;
    CustomOptional<uint32_t, NoDefault> TargetDeviceId;
    CustomOptional<std::wstring> SpoofedGPUName { L"NVIDIA GeForce RTX 4090" };
    CustomOptional<bool> UESpoofIntelAtomics64 { false };
    CustomOptional<bool> SpoofRegistry { false };
    CustomOptional<bool> SpoofUser32 { false };
    CustomOptional<std::wstring> SpoofedDriver { L"32.0.15.9155" };

    // Plugins
    CustomOptional<std::wstring, NoDefault> PluginPath;
    CustomOptional<bool> LoadSpecialK { false };
    CustomOptional<bool> LoadReShade { false };
    CustomOptional<bool> LoadCustomAmdxc64OnRdna2 { false };
    CustomOptional<bool> LoadAsiPlugins { false };
    CustomOptional<int> LateAsiPluginsDelay { 30 };
    CustomOptional<std::wstring, NoDefault> DisabledAsi; // comma separated file names, see plugins/PluginRegistry.h

    // Frame Generation
    CustomOptional<FGInput> FGInput { FGInput::NoFG };
    CustomOptional<bool> ExternalFrameGeneration { false };
    CustomOptional<MfgUnlockMode> MfgUnlockXeFG { MfgUnlock::kDefaultMode };
    CustomOptional<MfgUnlockMode> MfgUnlockRTX40 { MfgUnlock::kDefaultMode };
    CustomOptional<MfgUnlockMode> MfgUnlockRTX2030 { MfgUnlock::kDefaultMode };
    // [DLSSG] AdaMfgUnlock / AmpereMfgUnlock is still true in the ini: those keys were replaced by [MFGUnlock] RTX40 /
    // RTX2030, and startup says so once. Read only, never saved.
    bool RemovedAdaMfgUnlockSet = false;
    bool RemovedAmpereMfgUnlockSet = false;
    CustomOptional<FGOutput> FGOutput { FGOutput::NoFG };
    // Keeps Streamline from picking up the driver's OTA/downloaded plugins; only files in
    // OptiScaler/streamline are used for sl.* plugins and nvngx_dlssg.dll
    CustomOptional<bool> FGStreamlineIgnoreOTA { false };
    CustomOptional<FGNvngxReplacement> FGNvngxReplacement { FGNvngxReplacement::None };
    CustomOptional<bool> FGDrawUIOverFG { false };
    CustomOptional<bool> FGUIPremultipliedAlpha { true };
    CustomOptional<bool> FGDisableHudless { false };
    CustomOptional<bool> FGDisableUI { false };
    CustomOptional<bool> FGSkipReset { false };
    CustomOptional<int> FGAllowedFrameAhead { 1 };
    CustomOptional<bool> FGDepthValidNow { false };
    CustomOptional<bool> FGVelocityValidNow { false };
    CustomOptional<bool> FGHudlessValidNow { false };
    CustomOptional<bool> FGOnlyAcceptFirstHudless { false };
    CustomOptional<bool> FGPreserveSwapChain { true };
    CustomOptional<bool> FGSkipResizeBuffers { false };
    CustomOptional<bool> FGModifyBufferState { false };
    CustomOptional<bool> FGModifySCIndex { false };
    CustomOptional<float> FGHudCutoff { 0.0f };
    CustomOptional<FrameTimeSource> FTInput { FrameTimeSource::Input };

    // OptiFG
    // Unset (auto) follows the game's DLSS FG with FG Input DLSSG: read it through IsFGEnabled(). SoftDefault so a
    // false set in the menu is saved and stops the following.
    CustomOptional<bool, SoftDefault> FGEnabled { false };
    CustomOptional<bool> FGUseMutexForSwapchain { true };
    CustomOptional<bool> FGMakeMVCopy { true };
    CustomOptional<bool> FGMakeDepthCopy { true };
    CustomOptional<bool> FGResourceFlip { false };
    CustomOptional<bool> FGResourceFlipOffset { false };
    CustomOptional<bool> FGAlwaysCaptureFSRFGSwapchain { false };

    CustomOptional<int, NoDefault> FGRectLeft;
    CustomOptional<int, NoDefault> FGRectTop;
    CustomOptional<int, NoDefault> FGRectWidth;
    CustomOptional<int, NoDefault> FGRectHeight;

    // OptiFG - Hudfix
    CustomOptional<bool> FGDisableHUDFix { false };
    CustomOptional<bool> FGHUDFix { false };
    CustomOptional<int> FGHUDLimit { 1 };
    CustomOptional<bool> FGHUDFixExtended { false };
    CustomOptional<bool> FGImmediateCapture { false };
    CustomOptional<bool> FGHudfixPersistentBindings { true };
    CustomOptional<bool> FGDontUseSwapchainBuffers { false };
    CustomOptional<bool> FGRelaxedResolutionCheck { false };
    CustomOptional<bool> FGHudfixDisableRTV { false };
    CustomOptional<bool> FGHudfixDisableSRV { false };
    CustomOptional<bool> FGHudfixDisableUAV { false };
    CustomOptional<bool> FGHudfixDisableOM { false };
    CustomOptional<bool> FGHudfixDisableDispatch { false };
    CustomOptional<bool> FGHudfixDisableDI { false };
    CustomOptional<bool> FGHudfixDisableDII { false };
    CustomOptional<bool> FGHudfixDisableSCR { true };
    CustomOptional<bool> FGHudfixDisableSGR { true };

    // OptiFG - Resource Tracking
    CustomOptional<bool> FGAlwaysTrackHeaps { false };
    CustomOptional<bool> FGResourceBlocking { false };
    CustomOptional<bool> FGUseShards { false };

    // OptiFG - DLSS-D Depth scale
    CustomOptional<bool> FGEnableDepthScale { false };
    CustomOptional<float> FGDepthScaleMax { 10000.0f };

    // FSR-FG
    CustomOptional<bool> FGDebugView { false };
    CustomOptional<bool> FGDebugResetLines { false };
    CustomOptional<bool> FGDebugTearLines { false };
    CustomOptional<bool> FGDebugPacingLines { false };
    CustomOptional<bool> FGAsync { false };
    CustomOptional<bool> FGFramePacingTuning { true };
    CustomOptional<float> FGFPTSafetyMarginInMs { 0.01f };
    CustomOptional<float> FGFPTVarianceFactor { 0.3f };
    CustomOptional<bool> FGFPTAllowHybridSpin { false };
    CustomOptional<int> FGFPTHybridSpinTime { 2 };
    CustomOptional<bool> FGFPTAllowWaitForSingleObjectOnFence { false };

    CustomOptional<bool> FSRFGSkipConfigForHudless { false };
    CustomOptional<bool> FSRFGSkipDispatchForHudless { false };
    CustomOptional<bool> FSRFGEnableWatermark { false };

    // XeFG
    CustomOptional<bool> FGXeFGIgnoreInitChecks { false };
    CustomOptional<int> FGXeFGMaxInterpolations { 5 };
    CustomOptional<int> FGXeFGInterpolationCount { 1 };
    CustomOptional<bool> FGXeFGDynamicMFG { false };          // Count picked per frame, up to the InterpolationCount
    CustomOptional<float> FGXeFGFramerateTargetDMFG { 0.0f }; // 0: the FPS limit, else just under the refresh rate
    CustomOptional<int> FGXeFGRoundingDMFG { 1 };             // 0 at most, 1 around, 2 at least the target
    CustomOptional<bool> FGXeFGDynamicAllow1X { true };       // Dynamic MFG may turn frame generation off (1X)
    CustomOptional<bool> FGXeFGTargetAsCapDMFG { false };     // The Dynamic MFG target is also the FPS limit
    CustomOptional<bool> FGXeFGUIComposition { false };
    CustomOptional<bool> FGXeFGDepthInverted { true };
    CustomOptional<bool> FGXeFGJitteredMV { false };
    CustomOptional<bool> FGXeFGHighResMV { false };
    CustomOptional<bool> FGXeFGDebugView { false };
    CustomOptional<bool> FGXeFGForceBorderless { false };
    // [XeFG] LowLatencyOutput, what lowers latency with XeFG (native/NativeLowLatencyRule.h XefgLatency): xell (default),
    // reflex (NVIDIA, real nvapi) or antilag2 (AMD, fakenvapi). XeLL stays in place for XeFG either way, with low latency
    // off when the other two are chosen. A choice the setup lacks reads as xell.
    CustomOptional<native::lowlatency::XefgLatency> FGXeFGLowLatencyOutput { native::lowlatency::XefgLatency::XeLL };

    // DLSSG
    CustomOptional<int> FGDLSSGInterpolationCount { 1 }; // For Opti's own SL instance
    CustomOptional<bool> FGDLSSGUseGamesReflexMarkers { true };
    CustomOptional<int, NoDefault>
        FGDLSSGOverrideInterpolationCount; // For overriding game's value sent to SL, could be Nvngx FG, could be noFG
                                           // but someone just uses real DLSSG
    CustomOptional<bool> FGDLSSGOverrideForceDMFG { false };   // Overrides game's DLSSG mode to Dynamic
    CustomOptional<bool> FGDLSSGForceDMFG { false };           // Overrides Opti's DLSSG mode to Dynamic
    CustomOptional<float> FGDLSSGFramerateTargetDMFG { 0.0f }; // 0.0 means auto-detects the display refresh rate

    // As per
    // https://github.com/artur-graniszewski/dlss-enabler-main/blob/a92464d468eb0d91ae17befa66c6bf6229f20b9f/Utils/DlssgProxy.cpp#L1033
    CustomOptional<uint32_t> NvngxFGDispatchFlags { 0x10000000 }; // IGNORE_UI_TEXTURE
    CustomOptional<bool> NvngxFGShowDebug { false };
    CustomOptional<bool> NvngxFGDisableHudless { false };

    // fakenvapi
    CustomOptional<bool> UseFakenvapi { true };
    CustomOptional<bool> ForceXeLL { false };
    CustomOptional<bool> FN_ForceLatencyFlex { false };
    CustomOptional<LFXMode> FN_LatencyFlexMode { LFXMode::Conservative };
    CustomOptional<ForceReflex> FN_ForceReflex { ForceReflex::InGame };

    // Inputs
    CustomOptional<bool> EnableDlssInputs { true };
    CustomOptional<bool> EnableXeSSInputs { true };
    CustomOptional<bool> UseFsr2Inputs { true };
    CustomOptional<bool> UseFsr2Dx11Inputs { false };
    CustomOptional<bool> UseFsr2VulkanInputs { false };
    CustomOptional<bool> Fsr2Pattern { false };
    CustomOptional<bool> UseFsr3Inputs { true };
    CustomOptional<bool> Fsr3Pattern { false };
    CustomOptional<bool> UseFfxInputs { true };
    CustomOptional<bool> EnableHotSwapping { false };
    CustomOptional<bool> EnableFsr2Inputs { true };
    CustomOptional<bool> EnableFsr3Inputs { true };
    CustomOptional<bool> EnableFfxInputs { true };

    // Framerate
    CustomOptional<float> FramerateLimit { 0.0f };
    // [Framerate] LowLatencyInput, the Framerate page's low latency "Input" (native/NativeLowLatencyRule.h): OptiScaler
    // calls the Reflex API itself, since a game that makes no upscaler call makes no Reflex call either, and XeFG's XeLL
    // needs the calls (native/NativeLowLatency.h). auto: the game's own Reflex when it makes the calls, else ours while
    // an Optical F5Low mode, XeFG or FSR FG runs; game: only the game's; optiscaler: ours in any game without Reflex;
    // off: no low latency, the game's Reflex is sent with it off. The old [DlssNr] NativeLowLatency is read when this
    // key is missing (auto, true = optiscaler, false = game) and still written beside it.
    CustomOptional<native::lowlatency::Input> FramerateLowLatencyInput { native::lowlatency::Input::Auto };
    // [Framerate] FrameCapOnly, the Framerate page's Output "Frame cap only" (ll_fps_limiter.h): no latency reduction,
    // FramerateLimit waits at the game's Sleep (or simulation start) instead of at present. Not with XeFG or on NVIDIA
    // Reflex (low_latency/input/OutputRule.h PlanOutput).
    CustomOptional<bool> FramerateFrameCapOnly { false };

    // HDR output for SDR games (output/SdrToHdr_*): the real swap chain becomes scRGB / HDR10 and one pass converts the
    // game's picture. Enabled applies at the next swap chain create / resize; the rest is read every frame.
    CustomOptional<bool> HdrOutputEnabled { false };  // [HDR] Enabled (the old ForceHDR=true is read into it)
    CustomOptional<bool> HdrOutputConvert { true };   // [HDR] Convert: the A/B toggle, never saved
    CustomOptional<bool> UseHDR10 { false };          // [HDR] UseHDR10: PQ instead of scRGB
    CustomOptional<float> HdrOutputPeakNits { 0.0f }; // [HDR] PeakNits: clamp, 0 = the display's MaxLuminance
    CustomOptional<float> HdrOutputGameNits { 203.0f }; // [HDR] GameNits: SDR white, 80..1000
    CustomOptional<int> HdrOutputDecode { 0 };        // [HDR] Decode: 0 = gamma 2.2, 1 = sRGB

    // V-Sync
    CustomOptional<bool> OverrideVsync { false };
    CustomOptional<bool, NoDefault> ForceVsync;
    CustomOptional<UINT> VsyncInterval { 0 };

    // Old configs for compat reasons
    CustomOptional<bool, NoDefault> _DONTUSE_Fsr4ForceEnableInt8;

    bool LoadFromPath(const wchar_t* InPath);
    bool SaveIni();
    bool SaveXeFG();

    void CheckUpscalerFiles();

    // FG on/off and frame count. With FG Input DLSSG (Streamline 2+) and Enabled unset they follow the game's DLSS
    // Frame Generation setting (framegen/FGFollowGameRule.h); otherwise FGEnabled and the output's menu count.
    static bool FGCanFollowGame();
    bool FGFollowsGame() const;
    bool IsFGEnabled() const;
    int FGInterpolationCount(int menuCount, int outputMax) const;

    std::vector<std::string> GetConfigLog();

    static Config* Instance();

  private:
    inline static Config* _config;
    inline static std::vector<std::string> _log;

    std::filesystem::path absoluteFileName;
    std::wstring fileName = L"OptiScaler.ini";

    bool Reload(std::filesystem::path iniPath);

    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    std::optional<std::wstring> readWString(std::string section, std::string key, bool lowercase = false);
    std::optional<float> readFloat(std::string section, std::string key);
    std::optional<int> readInt(std::string section, std::string key);
    std::optional<uint32_t> readUInt(std::string section, std::string key);
    std::optional<bool> readBool(std::string section, std::string key);

    template <typename Enum> std::optional<Enum> readEnum(std::string section, std::string key);
};
