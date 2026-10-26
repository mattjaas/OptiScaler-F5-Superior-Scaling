#include "pch.h"

#include "Config.h"

#include "Util.h"

#include "nvapi/fakenvapi.h"
#include <hooks/Streamline_Hooks.h>
#include <misc/IdentifyGpu.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>

#include <SimpleIni.h>

static CSimpleIniA ini;
// What readString and the rest read: ini, or for ReadDlssNr a profile or the saved ini (Config::LoadDlssNrProfile).
static CSimpleIniA* readSource = &ini;

static inline int64_t GetTicks()
{
    LARGE_INTEGER ticks;

    if (!QueryPerformanceCounter(&ticks))
        return 0;

    return ticks.QuadPart;
}

static inline bool isInteger(const std::string& str, int& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isUInt(const std::string& str, uint32_t& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isFloat(const std::string& str, float& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

Config::Config()
{
    absoluteFileName = Util::DllPath().parent_path() / fileName;
    Reload(absoluteFileName);
}

bool Config::Reload(std::filesystem::path iniPath)
{
    auto pathWStr = iniPath.wstring();

    LOG_INFO("Trying to load ini from: {0}", wstring_to_string(pathWStr));
    if (ini.LoadFile(iniPath.c_str()) == SI_OK)
    {
        State::Instance().nvngxIniDetected = exists(iniPath.parent_path() / "nvngx.ini");
        _log.clear();

        // Upscalers
        {
            // transform converts only when optional has a value
            Dx11Upscaler.set_from_config(readString("Upscalers", "Dx11Upscaler", true).transform(CodeToUpscaler));
            Dx12Upscaler.set_from_config(readString("Upscalers", "Dx12Upscaler", true).transform(CodeToUpscalerFfx));
            VulkanUpscaler.set_from_config(
                readString("Upscalers", "VulkanUpscaler", true).transform(CodeToUpscalerFfx));
        }

        // Frame Generation
        {
            FGEnabled.set_from_config(readBool("FrameGen", "Enabled"));
            ExternalFrameGeneration.set_from_config(readBool("FrameGen", "External"));
            const auto readMfgUnlockMode = [this](const char* key)
            {
                const std::string value = ini.GetValue("MFGUnlock", key, "auto");
                _log.push_back(std::format("MFGUnlock.{}: {}", key, value));

                bool valid = true;
                const auto mode = MfgUnlock::ParseMode(value, valid);
                if (!valid)
                    LOG_WARN("[MFGUnlock] {} has invalid value '{}'; using the default (off)", key, value);
                return mode;
            };
            MfgUnlockXeFG.set_from_config(readMfgUnlockMode("XeFG"));
            MfgUnlockRTX40.set_from_config(readMfgUnlockMode("RTX40"));
            MfgUnlockRTX2030.set_from_config(readMfgUnlockMode("RTX2030"));
            // The old [DLSSG] unlock keys were replaced by [MFGUnlock]: they are only read to say so once at startup
            RemovedAdaMfgUnlockSet = readBool("DLSSG", "AdaMfgUnlock").value_or(false);
            RemovedAmpereMfgUnlockSet = readBool("DLSSG", "AmpereMfgUnlock").value_or(false);
            FGDebugView.set_from_config(readBool("FrameGen", "DebugView"));

            if (auto FGInputString = readString("FrameGen", "FGInput"); FGInputString.has_value())
            {
                if (lstrcmpiA(FGInputString.value().c_str(), "nofg") == 0)
                    FGInput.set_from_config(FGInput::NoFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "upscaler") == 0)
                    FGInput.set_from_config(FGInput::Upscaler);
                else if (lstrcmpiA(FGInputString.value().c_str(), "nvngxfg") == 0)
                    FGInput.set_from_config(FGInput::NvngxFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "dlssg") == 0)
                    FGInput.set_from_config(FGInput::DLSSG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "fsrfg") == 0)
                    FGInput.set_from_config(FGInput::FSRFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "fsrfg30") == 0)
                    FGInput.set_from_config(FGInput::FSRFG30);

                if (lstrcmpiA(FGInputString.value().c_str(), "nukems") == 0)
                {
                    FGInput.set_from_config(FGInput::NvngxFG);
                    ini.SetValue("FrameGen", "FGNvngxReplacement", "nukems");
                }
            }

            if (auto FGOutputString = readString("FrameGen", "FGOutput");
                FGInput.value_or_default() != FGInput::NvngxFG && FGOutputString.has_value())
            {
                if (lstrcmpiA(FGOutputString.value().c_str(), "nofg") == 0)
                    FGOutput.set_from_config(FGOutput::NoFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "fsrfg") == 0)
                    FGOutput.set_from_config(FGOutput::FSRFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "xefg") == 0)
                    FGOutput.set_from_config(FGOutput::XeFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "dlssg") == 0)
                    FGOutput.set_from_config(FGOutput::DLSSG);
            }

            FGStreamlineIgnoreOTA.set_from_config(readBool("FrameGen", "StreamlineIgnoreOTA"));

            const bool canUseNvngxReplacement =
                FGInput.value_or_default() == FGInput::NvngxFG || FGOutput.value_or_default() == FGOutput::DLSSG;

            if (auto FGNvngxReplacementString = readString("FrameGen", "FGNvngxReplacement");
                canUseNvngxReplacement && FGNvngxReplacementString.has_value())
            {
                if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "none") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::None);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "nukems") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Nukems);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "arturs") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Arturs);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "ffx") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::FFX);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "combo") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Combo);
            }

            if (auto forceXell = readBool("fakenvapi", "ForceXeLL"); forceXell.has_value() && forceXell.value())
            {
                FGInput.set_volatile_value(FGInput::ForceXeLL);
                FGOutput.set_volatile_value(FGOutput::XeFG);
            }

            auto ftInput = readInt("FrameGen", "FTInput");
            if (ftInput.has_value() && ftInput.value() >= 0 &&
                ftInput.value() <= (FGOutput.value_or_default() == FGOutput::XeFG ? 2 : 1))
            {
                FTInput.set_from_config(static_cast<FrameTimeSource>(ftInput.value()));
            }

            FGDrawUIOverFG.set_from_config(readBool("FrameGen", "DrawUIOverFG"));
            FGUIPremultipliedAlpha.set_from_config(readBool("FrameGen", "UIPremultipliedAlpha"));
            FGDisableHudless.set_from_config(readBool("FrameGen", "DisableHudless"));
            FGDisableUI.set_from_config(readBool("FrameGen", "DisableUI"));
            FGSkipReset.set_from_config(readBool("FrameGen", "SkipReset"));
            FGRectLeft.set_from_config(readInt("FrameGen", "RectLeft"));
            FGRectTop.set_from_config(readInt("FrameGen", "RectTop"));
            FGRectWidth.set_from_config(readInt("FrameGen", "RectWidth"));
            FGRectHeight.set_from_config(readInt("FrameGen", "RectHeight"));

            FGAllowedFrameAhead.set_from_config(readInt("FrameGen", "AllowedFrameAhead"));
            if (FGAllowedFrameAhead.has_value() && (FGAllowedFrameAhead.value() < 1 || FGAllowedFrameAhead.value() > 3))
                FGAllowedFrameAhead.reset();

            FGDepthValidNow.set_from_config(readBool("FrameGen", "DepthValidNow"));
            FGVelocityValidNow.set_from_config(readBool("FrameGen", "VelocityValidNow"));
            FGHudlessValidNow.set_from_config(readBool("FrameGen", "HudlessValidNow"));
            FGOnlyAcceptFirstHudless.set_from_config(readBool("FrameGen", "OnlyAcceptFirstHudless"));
            FGPreserveSwapChain.set_from_config(readBool("FrameGen", "PreserveSwapChain"));
            FGSkipResizeBuffers.set_from_config(readBool("FrameGen", "SkipResizeBuffers"));
            FGModifyBufferState.set_from_config(readBool("FrameGen", "ModifyBufferState"));
            FGModifySCIndex.set_from_config(readBool("FrameGen", "ModifySCIndex"));
            FGHudCutoff.set_from_config(readFloat("FrameGen", "HudCutoff"));
        }

        // FSR FG
        {
            FGDebugTearLines.set_from_config(readBool("FSRFG", "DebugTearLines"));
            FGDebugResetLines.set_from_config(readBool("FSRFG", "DebugResetLines"));
            FGDebugPacingLines.set_from_config(readBool("FSRFG", "DebugPacingLines"));
            FGAsync.set_from_config(readBool("FSRFG", "AllowAsync"));
            FGUseMutexForSwapchain.set_from_config(readBool("FSRFG", "UseMutexForSwapchain"));
            FGFramePacingTuning.set_from_config(readBool("FSRFG", "FramePacingTuning"));
            FGFPTSafetyMarginInMs.set_from_config(readFloat("FSRFG", "FPTSafetyMarginInMs"));
            FGFPTVarianceFactor.set_from_config(readFloat("FSRFG", "FPTVarianceFactor"));
            FGFPTAllowHybridSpin.set_from_config(readBool("FSRFG", "FPTHybridSpin"));
            FGFPTHybridSpinTime.set_from_config(readInt("FSRFG", "FPTHybridSpinTime"));
            FGFPTAllowWaitForSingleObjectOnFence.set_from_config(readBool("FSRFG", "FPTWaitForSingleObjectOnFence"));
            FSRFGEnableWatermark.set_from_config(readBool("FSRFG", "EnableWatermark"));
            FSRFGDisableFlipMetering.set_from_config(readBool("FSRFG", "DisableFlipMetering"));
        }

        // OptiFG
        {
            FGDisableHUDFix.set_from_config(readBool("OptiFG", "DisableHUDFix"));
            FGHUDFix.set_from_config(readBool("OptiFG", "HUDFix"));
            FGHUDLimit.set_from_config(readInt("OptiFG", "HUDLimit"));
            FGHUDFixExtended.set_from_config(readBool("OptiFG", "HUDFixExtended"));
            FGHUDFixUITexture.set_from_config(readBool("OptiFG", "HUDFixUITexture"));
            FGHUDFixUIUnblend.set_from_config(readBool("OptiFG", "HUDFixUIUnblend"));
            FGImmediateCapture.set_from_config(readBool("OptiFG", "HUDFixImmediate"));
            FGHudfixPersistentBindings.set_from_config(readBool("OptiFG", "HUDFixPersistentBindings"));
            FGUseShards.set_from_config(readBool("OptiFG", "UseShards"));
            FGAlwaysTrackHeaps.set_from_config(readBool("OptiFG", "AlwaysTrackHeaps"));
            FGResourceBlocking.set_from_config(readBool("OptiFG", "ResourceBlocking"));
            FGMakeDepthCopy.set_from_config(readBool("OptiFG", "MakeDepthCopy"));
            FGMakeMVCopy.set_from_config(readBool("OptiFG", "MakeMVCopy"));
            FGHudfixDisableRTV.set_from_config(readBool("OptiFG", "HudfixDisableRTV"));
            FGHudfixDisableSRV.set_from_config(readBool("OptiFG", "HudfixDisableSRV"));
            FGHudfixDisableUAV.set_from_config(readBool("OptiFG", "HudfixDisableUAV"));
            FGHudfixDisableOM.set_from_config(readBool("OptiFG", "HudfixDisableOM"));
            FGHudfixDisableDispatch.set_from_config(readBool("OptiFG", "HudfixDisableDispatch"));
            FGHudfixDisableDI.set_from_config(readBool("OptiFG", "HudfixDisableDI"));
            FGHudfixDisableDII.set_from_config(readBool("OptiFG", "HudfixDisableDII"));
            FGHudfixDisableSCR.set_from_config(readBool("OptiFG", "HudfixDisableSCR"));
            FGHudfixDisableSGR.set_from_config(readBool("OptiFG", "HudfixDisableSGR"));

            FGEnableDepthScale.set_from_config(readBool("OptiFG", "EnableDepthScale"));
            FGDepthScaleMax.set_from_config(readFloat("OptiFG", "DepthScaleMax"));

            FGDontUseSwapchainBuffers.set_from_config(readBool("OptiFG", "HUDFixDontUseSwapchainBuffers"));
            FGRelaxedResolutionCheck.set_from_config(readBool("OptiFG", "HUDFixRelaxedResolutionCheck"));

            FGResourceFlip.set_from_config(readBool("OptiFG", "ResourceFlip"));
            FGResourceFlipOffset.set_from_config(readBool("OptiFG", "ResourceFlipOffset"));

            FGAlwaysCaptureFSRFGSwapchain.set_from_config(readBool("OptiFG", "AlwaysCaptureFSRFGSwapchain"));
        }

        {
            FGXeFGMaxInterpolations.set_from_config(readInt("XeFG", "MaxInterpolations"));
            if (FGXeFGMaxInterpolations.has_value() &&
                (FGXeFGMaxInterpolations.value() < 1 || FGXeFGMaxInterpolations.value() > 5))
                FGXeFGMaxInterpolations.reset();

            FGXeFGInterpolationCount.set_from_config(readInt("XeFG", "InterpolationCount"));
            if (FGXeFGInterpolationCount.has_value() && FGXeFGInterpolationCount.value() < 1)
                FGXeFGInterpolationCount.reset();

            FGXeFGDynamicMFG.set_from_config(readBool("XeFG", "DynamicMFG"));
            FGXeFGFramerateTargetDMFG.set_from_config(readFloat("XeFG", "FramerateTargetDMFG"));
            if (FGXeFGFramerateTargetDMFG.has_value() && FGXeFGFramerateTargetDMFG.value() < 10.0f)
                FGXeFGFramerateTargetDMFG.reset();

            FGXeFGRoundingDMFG.set_from_config(readInt("XeFG", "RoundingDMFG"));
            if (FGXeFGRoundingDMFG.has_value() && (FGXeFGRoundingDMFG.value() < 0 || FGXeFGRoundingDMFG.value() > 2))
                FGXeFGRoundingDMFG.reset();

            FGXeFGDynamicAllow1X.set_from_config(readBool("XeFG", "DynamicAllow1X"));
            FGXeFGTargetAsCapDMFG.set_from_config(readBool("XeFG", "TargetAsCapDMFG"));

            FGXeFGIgnoreInitChecks.set_from_config(readBool("XeFG", "IgnoreInitChecks"));
            FGXeFGUIComposition.set_from_config(readBool("XeFG", "UIComposition"));
            FGXeFGDepthInverted.set_from_config(readBool("XeFG", "DepthInverted"));
            FGXeFGJitteredMV.set_from_config(readBool("XeFG", "JitteredMV"));
            FGXeFGHighResMV.set_from_config(readBool("XeFG", "HighResMV"));
            FGXeFGDebugView.set_from_config(readBool("XeFG", "DebugView"));
            FGXeFGForceBorderless.set_from_config(readBool("XeFG", "ForceBorderless"));

            // A word we do not know leaves it unset (xell)
            if (const auto output = readString("XeFG", "LowLatencyOutput", true); output.has_value())
            {
                auto latency = native::lowlatency::XefgLatency::XeLL;

                if (native::lowlatency::XefgLatencyFromIniText(output->c_str(), &latency))
                    FGXeFGLowLatencyOutput.set_from_config(latency);
            }
        }

        {
            FGDLSSGInterpolationCount.set_from_config(readInt("DLSSG", "InterpolationCount"));
            if (FGDLSSGInterpolationCount.has_value() &&
                (FGDLSSGInterpolationCount.value() < 1 || FGDLSSGInterpolationCount.value() > 6))
                FGDLSSGInterpolationCount.reset();

            FGDLSSGUseGamesReflexMarkers.set_from_config(readBool("DLSSG", "UseGamesReflexMarkers"));

            FGDLSSGOverrideInterpolationCount.set_from_config(readInt("DLSSG", "OverrideInterpolationCount"));
            if (FGDLSSGOverrideInterpolationCount.has_value() &&
                (FGDLSSGOverrideInterpolationCount.value() < 0 || FGDLSSGOverrideInterpolationCount.value() > 6))
                FGDLSSGOverrideInterpolationCount.reset();

            FGDLSSGFramerateTargetDMFG.set_from_config(readFloat("DLSSG", "FramerateTargetDMFG"));
            FGDLSSGOverrideForceDMFG.set_from_config(readBool("DLSSG", "OverrideForceDMFG"));
            FGDLSSGForceDMFG.set_from_config(readBool("DLSSG", "ForceDMFG"));
        }

        // FSR FG Inputs
        {
            FSRFGSkipConfigForHudless.set_from_config(readBool("FSRFGInputs", "SkipConfigForHudless"));
            FSRFGSkipDispatchForHudless.set_from_config(readBool("FSRFGInputs", "SkipDispatchForHudless"));
        }

        // Framerate
        {
            FramerateLimit.set_from_config(readFloat("Framerate", "FramerateLimit"));

            // The new key if it holds a word we know, else the one from before the Input choice (an ini that has
            // neither stays unset: Auto, and nothing is held for the config)
            // (readString reads "auto" as not there; an explicit auto must beat the old key, so the key's presence is
            // asked of the ini itself)
            const char* inputRaw = ini.GetValue("Framerate", "LowLatencyInput", nullptr);
            const bool inputPresent = inputRaw != nullptr;
            const auto legacy = readString("DlssNr", "NativeLowLatency", true);

            if (inputPresent || legacy.has_value())
            {
                std::string inputText = inputRaw != nullptr ? inputRaw : "";
                std::ranges::transform(inputText, inputText.begin(), [](unsigned char c) { return std::tolower(c); });

                FramerateLowLatencyInput.set_from_config(native::lowlatency::InputFromIniValues(
                    inputPresent, inputPresent ? inputText.c_str() : nullptr,
                    legacy.has_value() ? legacy->c_str() : nullptr));
            }

            FramerateFrameCapOnly.set_from_config(readBool("Framerate", "FrameCapOnly"));
        }

        // FSR Common
        {
            FsrVerticalFov.set_from_config(readFloat("FSR", "VerticalFov"));
            FsrHorizontalFov.set_from_config(readFloat("FSR", "HorizontalFov"));
            FsrCameraNear.set_from_config(readFloat("FSR", "CameraNear"));
            FsrCameraFar.set_from_config(readFloat("FSR", "CameraFar"));
            FsrUseFsrInputValues.set_from_config(readBool("FSR", "UseFsrInputValues"));
        }

        // FSR
        {
            FsrVelocity.set_from_config(readFloat("FSR", "VelocityFactor"));
            FsrReactiveScale.set_from_config(readFloat("FSR", "ReactiveScale"));
            FsrShadingScale.set_from_config(readFloat("FSR", "ShadingScale"));
            FsrAccAddPerFrame.set_from_config(readFloat("FSR", "AccAddPerFrame"));
            FsrMinDisOccAcc.set_from_config(readFloat("FSR", "MinDisOccAcc"));
            FsrDebugView.set_from_config(readBool("FSR", "DebugView"));
            FfxUpscalerIndex.set_from_config(readInt("FSR", "UpscalerIndex"));
            FfxFGIndex.set_from_config(readInt("FSR", "FGIndex"));
            FsrUseMaskForTransparency.set_from_config(readBool("FSR", "UseReactiveMaskForTransparency"));
            DlssReactiveMaskBias.set_from_config(readFloat("FSR", "DlssReactiveMaskBias"));

            if (auto v = readEnum<FSR4Support>("FSR", "Fsr4GpuSupport"))
                Fsr4GpuSupport.set_from_config(*v);
            else
                Fsr4GpuSupport.reset();

            if (auto v = readEnum<FSR4Model>("FSR", "Fsr4Model"))
                Fsr4Model.set_from_config(*v);
            else
                Fsr4Model.reset();

            Fsr4EnableWatermark.set_from_config(readBool("FSR", "Fsr4EnableWatermark"));
            Fsr4DoNotLoadAmdxc64.set_from_config(readBool("FSR", "Fsr4DoNotLoadAmdxc64"));

            if (auto setting = readInt("FSR", "Fsr4Preset"); setting.has_value() && setting >= 0 && setting <= 5)
                Fsr4Preset.set_from_config(setting);

            FsrNonLinearColorSpace.set_from_config(readBool("FSR", "FsrNonLinearColorSpace"));
            FsrNonLinearPQ.set_from_config(readBool("FSR", "FsrNonLinearPQ"));
            FsrNonLinearSRGB.set_from_config(readBool("FSR", "FsrNonLinearSRGB"));
            FsrAgilitySDKUpgrade.set_from_config(readBool("FSR", "FsrAgilitySDKUpgrade"));

            // Only sRGB or PQ should be enabled
            if (FsrNonLinearPQ.has_value() && FsrNonLinearPQ.value())
                FsrNonLinearSRGB.reset();
            else if (FsrNonLinearSRGB.has_value() && FsrNonLinearSRGB.value())
                FsrNonLinearPQ.reset();

            if (FsrNonLinearPQ.has_value() || FsrNonLinearSRGB.has_value())
                FsrNonLinearColorSpace.set_volatile_value(true);
        }

        // XeSS
        {
            BuildPipelines.set_from_config(readBool("XeSS", "BuildPipelines"));
            NetworkModel.set_from_config(readInt("XeSS", "NetworkModel"));
            CreateHeaps.set_from_config(readBool("XeSS", "CreateHeaps"));
        }

        // DLSS
        {
            // Don't enable again if set false because of no nvngx found
            DLSSEnabled.set_from_config(readBool("DLSS", "Enabled"));

            ReadDlssNr();
            UseGenericAppIdWithDlss.set_from_config(readBool("DLSS", "UseGenericAppIdWithDlss"));

            RenderPresetOverride.set_from_config(readBool("DLSS", "RenderPresetOverride"));

            constexpr size_t presetCount = 17;

            if (auto setting = readInt("DLSS", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraPerformance.set_from_config(setting);
        }
        // DLSSD
        {
            // Don't enable again if set false because of no nvngx found
            DLSSDRenderPresetOverride.set_from_config(readBool("DLSSD", "RenderPresetOverride"));

            constexpr size_t presetCount = 7;

            if (auto setting = readInt("DLSSD", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraPerformance.set_from_config(setting);
        }

        // NvngxFG
        {
            if (auto setting = readBool("Nukems", "MakeDepthCopy"); setting.has_value() && setting.value())
                NvngxFGMakeDepthCopy.set_from_config(setting); // For compat with older config
            else
                NvngxFGMakeDepthCopy.set_from_config(readBool("NvngxFG", "MakeDepthCopy"));

            NvngxFGDispatchFlags.set_from_config(readUInt("NvngxFG", "DispatchFlags"));
            NvngxFGShowDebug.set_from_config(readBool("NvngxFG", "ShowDebug"));
            NvngxFGDisableHudless.set_from_config(readBool("NvngxFG", "DisableHudless"));
        }

        // Logging
        {
            LogToFile.set_from_config(readBool("Log", "LogToFile"));
            LogLevel.set_from_config(readInt("Log", "LogLevel"));
            LogToConsole.set_from_config(readBool("Log", "LogToConsole"));
            LogToDebug.set_from_config(readBool("Log", "LogToDebug"));
            LogToNGX.set_from_config(readBool("Log", "LogToNGX"));
            D3D12Dred.set_from_config(readBool("Log", "D3D12Dred"));
            CrashDump.set_from_config(readBool("Log", "CrashDump"));
            OpenConsole.set_from_config(readBool("Log", "OpenConsole"));
            DebugWait.set_from_config(readBool("Log", "DebugWait"));
            LogSingleFile.set_from_config(readBool("Log", "SingleFile"));
            LogAsync.set_from_config(readBool("Log", "LogAsync"));
            LogAsyncThreads.set_from_config(readInt("Log", "LogAsyncThreads"));

            {
                auto setting = readString("Log", "LogFileName", false);

                if (setting.has_value() && setting.value().empty())
                    setting = std::nullopt;

                auto path = std::filesystem::path(setting.value_or(wstring_to_string(LogFileName.value_or_default())));
                auto filenameStem = path.stem();

                auto filename =
                    std::filesystem::path(LogSingleFile.value_or_default()
                                              ? filenameStem.wstring() + L".log"
                                              : filenameStem.wstring() + L"_" + std::to_wstring(GetTicks()) + L".log");

                if (setting.has_value())
                {
                    if (path.has_root_path())
                        LogFileName.set_from_config((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_from_config((Util::DllPath().parent_path() / filename).wstring());
                }
                else
                {
                    if (path.has_root_path())
                        LogFileName.set_volatile_value((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_volatile_value((Util::DllPath().parent_path() / filename).wstring());
                }
            }
        }

        // Sharpness
        {
            SharpnessShader.set_from_config(readString("Sharpness", "Shader", true).transform(CodeToSharpnessShader));
            OverrideSharpness.set_from_config(readBool("Sharpness", "OverrideSharpness"));

            if (auto setting = readFloat("Sharpness", "Sharpness"); setting.has_value())
                Sharpness.set_from_config(std::clamp(setting.value(), 0.0f, 1.3f));
        }

        // Menu
        {
            if (auto setting = readFloat("Menu", "Scale"); setting.has_value())
                MenuScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            if (auto setting = readFloat("Menu", "MenuWidth"); setting.has_value())
                MenuWidth.set_from_config(std::clamp(setting.value(), 100.0f, 8000.0f));

            if (auto setting = readFloat("Menu", "MenuHeight"); setting.has_value())
                MenuHeight.set_from_config(std::clamp(setting.value(), 100.0f, 8000.0f));

            MenuPage.set_from_config(readString("Menu", "MenuPage", true));

            // Don't enable again if set false because of Linux issue
            OverlayMenu.set_from_config(readBool("Menu", "OverlayMenu"));
            ShortcutKey.set_from_config(readInt("Menu", "ShortcutKey"));
            ExtendedLimits.set_from_config(readBool("Menu", "ExtendedLimits"));
            ShowFps.set_from_config(readBool("Menu", "ShowFps"));
            F5LowHint.set_from_config(readBool("Menu", "F5LowHint"));
            UseHQFont.set_from_config(readBool("Menu", "UseHQFont"));
            DisableSplash.set_from_config(readBool("Menu", "DisableSplash"));

            if (auto setting = readUInt("Menu", "FpsOverlayPos"); setting.has_value())
            {
                FpsOverlayPosition.set_from_config(
                    (FpsOverlayPos) std::clamp(setting.value(), 0U, FpsOverlayPos_COUNT - 1));
            }

            if (auto setting = readUInt("Menu", "FpsOverlayType"); setting.has_value())
            {
                FpsOverlayType.set_from_config((FpsOverlay) std::clamp(setting.value(), 0U, FpsOverlay_COUNT - 1));
            }

            FpsShortcutKey.set_from_config(readInt("Menu", "FpsShortcutKey"));
            FpsCycleShortcutKey.set_from_config(readInt("Menu", "FpsCycleShortcutKey"));
            FpsOverlayHorizontal.set_from_config(readBool("Menu", "FpsOverlayHorizontal"));
            FpsOverlayShowRatio.set_from_config(readBool("Menu", "FpsOverlayShowRatio"));

            if (auto setting = readFloat("Menu", "FpsOverlayAlpha"); setting.has_value())
                FpsOverlayAlpha.set_from_config(std::clamp(setting.value(), 0.0f, 1.0f));

            if (auto setting = readFloat("Menu", "FpsScale"); setting.has_value())
                FpsScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            FontSize.set_from_config(readFloat("Menu", "FontSize"));
            TTFFontPath.set_from_config(readWString("Menu", "TTFFontPath"));

            FGShortcutKey.set_from_config(readInt("Menu", "FGShortcutKey"));

            LightTheme.set_from_config(readBool("Menu", "LightTheme"));
            ModernTheme.set_from_config(readBool("Menu", "ModernTheme"));
            OverlaysUseTheme.set_from_config(readBool("Menu", "OverlaysUseTheme"));
            MenuAccentColorR.set_from_config(readFloat("Menu", "AccentColorR"));
            MenuAccentColorG.set_from_config(readFloat("Menu", "AccentColorG"));
            MenuAccentColorB.set_from_config(readFloat("Menu", "AccentColorB"));
            MenuBGColorR.set_from_config(readFloat("Menu", "BGColorR"));
            MenuBGColorG.set_from_config(readFloat("Menu", "BGColorG"));
            MenuBGColorB.set_from_config(readFloat("Menu", "BGColorB"));
            MenuBGColorA.set_from_config(readFloat("Menu", "BGColorA"));
        }

        // Hooks
        {
            HookOriginalNvngxOnly.set_from_config(readBool("Hooks", "HookOriginalNvngxOnly"));
            EarlyHooking.set_from_config(readBool("Hooks", "EarlyHooking"));
            D3D11FeatureLevelElevation.set_from_config(readBool("Hooks", "D3D11FeatureLevelElevation"));
            UseNtdllHooks.set_from_config(readBool("Hooks", "UseNtdllHooks"));
        }

        // RCAS
        {
            RcasEnabled.set_from_config(readBool("CAS", "Enabled"));

            MotionSharpnessEnabled.set_from_config(readBool("CAS", "MotionSharpnessEnabled"));

            if (auto setting = readFloat("CAS", "MotionSharpness"); setting.has_value())
                MotionSharpness.set_from_config(std::clamp(setting.value(), -1.3f, 1.3f));

            if (auto setting = readFloat("CAS", "MotionThreshold"); setting.has_value())
                MotionThreshold.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readFloat("CAS", "MotionScaleLimit"); setting.has_value())
                MotionScaleLimit.set_from_config(std::clamp(setting.value(), 0.01f, 100.0f));

            ContrastEnabled.set_from_config(readBool("CAS", "ContrastEnabled"));
            if (auto setting = readFloat("CAS", "Contrast"); setting.has_value())
                Contrast.set_from_config(std::clamp(setting.value(), -2.0f, 2.0f));

            DADepthScale.set_from_config(readFloat("CAS", "DADepthScale"));
            DADepthBias.set_from_config(readFloat("CAS", "DADepthBias"));
            DAClampOutput.set_from_config(readBool("CAS", "DAClampOutput"));

            MotionSharpnessDebug.set_from_config(readBool("CAS", "SharpenerDebug"));
        }

        // Magnifier
        {
            MagnifierEnabled.set_from_config(readBool("Magnifier", "Enabled"));

            if (auto setting = readFloat("Magnifier", "Size"); setting.has_value())
                MagnifierSize.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readInt("Magnifier", "ZoomFactor"); setting.has_value())
                MagnifierZoomFactor.set_from_config(std::clamp(setting.value(), 2, 20));

            if (auto setting = readFloat("Magnifier", "BorderSize"); setting.has_value())
                MagnifierBorderSize.set_from_config(std::clamp(setting.value(), 0.0f, 2.0f));

            if (auto setting = readFloat("Magnifier", "CursorOffsetX"); setting.has_value())
                MagnifierCursorOffsetX.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));
            if (auto setting = readFloat("Magnifier", "CursorOffsetY"); setting.has_value())
                MagnifierCursorOffsetY.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));

            if (auto setting = readFloat("Magnifier", "StaticPosX"); setting.has_value())
                MagnifierStaticPosX.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
            if (auto setting = readFloat("Magnifier", "StaticPosY"); setting.has_value())
                MagnifierStaticPosY.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
        }

        // Output Scaling
        {
            OutputScalingEnabled.set_from_config(readBool("OutputScaling", "Enabled"));

            if (auto v = readEnum<Scaler>("OutputScaling", "Downscaler"))
                OutputScalingDownscaler.set_from_config(*v);
            else
                OutputScalingDownscaler.reset();

            if (auto setting = readFloat("OutputScaling", "Multiplier"); setting.has_value())
                OutputScalingMultiplier.set_from_config(std::clamp(setting.value(), 0.5f, 3.0f));
        }

        // Init Flags
        {
            AutoExposure.set_from_config(readBool("InitFlags", "AutoExposure"));
            HDR.set_from_config(readBool("InitFlags", "HDR"));
            DepthInverted.set_from_config(readBool("InitFlags", "DepthInverted"));
            JitterCancellation.set_from_config(readBool("InitFlags", "JitterCancellation"));
            DisplayResolution.set_from_config(readBool("InitFlags", "DisplayResolution"));
            DynamicResolution.set_from_config(readBool("InitFlags", "DynamicResolution"));
            DisableReactiveMask.set_from_config(readBool("InitFlags", "DisableReactiveMask"));
        }

        // DRS
        {
            DrsMinOverrideEnabled.set_from_config(readBool("DRS", "DrsMinOverrideEnabled"));
            DrsMaxOverrideEnabled.set_from_config(readBool("DRS", "DrsMaxOverrideEnabled"));
        }

        // Upscale Ratio Override
        {
            UpscaleRatioOverrideEnabled.set_from_config(readBool("UpscaleRatio", "UpscaleRatioOverrideEnabled"));
            UpscaleRatioOverrideValue.set_from_config(readFloat("UpscaleRatio", "UpscaleRatioOverrideValue"));
        }

        // Quality Overrides
        {
            QualityRatioOverrideEnabled.set_from_config(readBool("QualityOverrides", "QualityRatioOverrideEnabled"));
            QualityRatio_DLAA.set_from_config(readFloat("QualityOverrides", "QualityRatioDLAA"));
            QualityRatio_UltraQuality.set_from_config(readFloat("QualityOverrides", "QualityRatioUltraQuality"));
            QualityRatio_Quality.set_from_config(readFloat("QualityOverrides", "QualityRatioQuality"));
            QualityRatio_Balanced.set_from_config(readFloat("QualityOverrides", "QualityRatioBalanced"));
            QualityRatio_Performance.set_from_config(readFloat("QualityOverrides", "QualityRatioPerformance"));
            QualityRatio_UltraPerformance.set_from_config(
                readFloat("QualityOverrides", "QualityRatioUltraPerformance"));
        }

        // Anisotropy
        {
            if (auto setting = readInt("Anisotropy", "AnisotropyOverride");
                setting.has_value() && setting.value() <= 16 && setting.value() >= 1)
                AnisotropyOverride.set_from_config(setting);

            if (AnisotropyOverride.has_value() && (AnisotropyOverride.value() > 16 || AnisotropyOverride.value() < 1))
                AnisotropyOverride.reset();

            AnisotropySkipPointFilter.set_from_config(readBool("Anisotropy", "SkipPointFilter"));
            AnisotropyModifyComp.set_from_config(readBool("Anisotropy", "AFModifyComparison"));
            AnisotropyModifyMinMax.set_from_config(readBool("Anisotropy", "AFModifyMinMax"));
        }

        // Mipmap
        {
            if (auto setting = readFloat("Mipmap", "MipmapBiasOverride");
                setting.has_value() && setting.value() <= 15.0 && setting.value() >= -15.0)
                MipmapBiasOverride.set_from_config(setting);

            // Unsure if that's needed but it resets invalid MipmapBiasOverride on config reload
            // Unexpected place for it but could be playing a role
            if (MipmapBiasOverride.has_value() &&
                (MipmapBiasOverride.value() > 15.0 || MipmapBiasOverride.value() < -15.0))
                MipmapBiasOverride.reset();

            MipmapBiasFixedOverride.set_from_config(readBool("Mipmap", "MipmapBiasFixedOverride"));
            MipmapBiasScaleOverride.set_from_config(readBool("Mipmap", "MipmapBiasScaleOverride"));
            MipmapBiasOverrideAll.set_from_config(readBool("Mipmap", "MipmapBiasOverrideAll"));
        }

        // Process Filter
        {
            ProcessExclusionList.set_from_config(readWString("ProcessFilter", "ProcessExclusionList", true));
            TargetProcess.set_from_config(readWString("ProcessFilter", "TargetProcessName", true));
        }

        // Hotfixes
        {
            CheckForUpdate.set_from_config(readBool("Hotfix", "CheckForUpdate"));
            DisableOverlays.set_from_config(readBool("Hotfix", "DisableOverlays"));
            REFrameworkCompat.set_from_config(readBool("Hotfix", "REFrameworkCompat"));

            SimulateWaitableObject.set_from_config(readBool("Hotfix", "SimulateWaitableObject"));

            RoundInternalResolution.set_from_config(readInt("Hotfix", "RoundInternalResolution"));

            RestoreComputeSignature.set_from_config(readBool("Hotfix", "RestoreComputeSignature"));
            RestoreGraphicSignature.set_from_config(readBool("Hotfix", "RestoreGraphicSignature"));
            ExtendedStateRestore.set_from_config(readBool("Hotfix", "ExtendedStateRestore"));
            PreferDedicatedGpu.set_from_config(readBool("Hotfix", "PreferDedicatedGpu"));
            PreferFirstDedicatedGpu.set_from_config(readBool("Hotfix", "PreferFirstDedicatedGpu"));
            SkipFirstFrames.set_from_config(readInt("Hotfix", "SkipFirstFrames"));
            UsePrecompiledShaders.set_from_config(readBool("Hotfix", "UsePrecompiledShaders"));
            ColorResourceBarrier.set_from_config(readInt("Hotfix", "ColorResourceBarrier"));
            MVResourceBarrier.set_from_config(readInt("Hotfix", "MotionVectorResourceBarrier"));
            DepthResourceBarrier.set_from_config(readInt("Hotfix", "DepthResourceBarrier"));
            MaskResourceBarrier.set_from_config(readInt("Hotfix", "ColorMaskResourceBarrier"));
            ExposureResourceBarrier.set_from_config(readInt("Hotfix", "ExposureResourceBarrier"));
            OutputResourceBarrier.set_from_config(readInt("Hotfix", "OutputResourceBarrier"));
            CreateD3D12DeviceForLuma.set_from_config(readBool("Hotfix", "CreateD3D12DeviceForLuma"));
        }

        // Dx11 with Dx12
        {
            Dx11DelayedInit.set_from_config(readInt("Dx11withDx12", "UseDelayedInit"));
            DontUseNTShared.set_from_config(readBool("Dx11withDx12", "DontUseNTShared"));
        }

        // NvApi
        {
            DisableFlipMetering.set_from_config(readBool("NvApi", "DisableFlipMetering"));
        }

        // Spoofing
        {
            DxgiSpoofing.set_from_config(readBool("Spoofing", "Dxgi"));
            DxgiFactoryWrapping.set_from_config(readBool("Spoofing", "DxgiFactoryWrapping"));
            DxgiBlacklist.set_from_config(readString("Spoofing", "DxgiBlacklist"));
            DxgiVRAM.set_from_config(readInt("Spoofing", "DxgiVRAM"));
            VulkanSpoofing.set_from_config(readBool("Spoofing", "Vulkan"));
            VulkanExtensionSpoofing.set_from_config(readBool("Spoofing", "VulkanExtensionSpoofing"));
            VulkanVRAM.set_from_config(readInt("Spoofing", "VulkanVRAM"));
            SpoofedGPUName.set_from_config(readWString("Spoofing", "SpoofedGPUName"));
            StreamlineSpoofing.set_from_config(readBool("Spoofing", "StreamlineSpoofing"));
            SpoofHAGS.set_from_config(readBool("Spoofing", "SpoofHAGS"));
            SpoofFeatureLevel.set_from_config(readBool("Spoofing", "D3DFeatureLevel"));
            SpoofedVendorId.set_from_config(readUInt("Spoofing", "SpoofedVendorId"));
            SpoofedDeviceId.set_from_config(readUInt("Spoofing", "SpoofedDeviceId"));
            TargetVendorId.set_from_config(readUInt("Spoofing", "TargetVendorId"));
            TargetDeviceId.set_from_config(readUInt("Spoofing", "TargetDeviceId"));
            UESpoofIntelAtomics64.set_from_config(readBool("Spoofing", "UEIntelAtomics"));
            SpoofRegistry.set_from_config(readBool("Spoofing", "Registry"));
            SpoofedDriver.set_from_config(readWString("Spoofing", "RegistryDriver"));
            SpoofUser32.set_from_config(readBool("Spoofing", "User32"));

            // Enable HAGS when DLSS-G will be used
            if (!SpoofHAGS.has_value())
            {
                SpoofHAGS.set_volatile_value(FGInput.value_or_default() == FGInput::NvngxFG ||
                                             FGInput.value_or_default() == FGInput::DLSSG);
            }
        }

        // fakenvapi
        {
            UseFakenvapi.set_from_config(readBool("fakenvapi", "UseFakenvapi"));
            ForceXeLL.set_from_config(readBool("fakenvapi", "ForceXeLL"));
            FN_ForceLatencyFlex.set_from_config(readBool("fakenvapi", "ForceLatencyFlex"));

            if (auto v = readEnum<LFXMode>("fakenvapi", "LatencyFlexMode"))
                FN_LatencyFlexMode.set_from_config(*v);
            else
                FN_LatencyFlexMode.reset();

            if (auto v = readEnum<ForceReflex>("fakenvapi", "ForceReflex"))
                FN_ForceReflex.set_from_config(*v);
            else
                FN_ForceReflex.reset();

            // DMFG is a mess with our reflex implementations, disable by default
            if (FGDLSSGOverrideForceDMFG.value_or_default() && !FN_ForceReflex.has_value())
                FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
        }

        // Inputs
        {
            EnableDlssInputs.set_from_config(readBool("Inputs", "EnableDlssInputs"));
            EnableXeSSInputs.set_from_config(readBool("Inputs", "EnableXeSSInputs"));

            EnableFsr2Inputs.set_from_config(readBool("Inputs", "EnableFsr2Inputs"));
            UseFsr2Inputs.set_from_config(readBool("Inputs", "UseFsr2Inputs"));
            UseFsr2Dx11Inputs.set_from_config(readBool("Inputs", "UseFsr2Dx11Inputs"));
            UseFsr2VulkanInputs.set_from_config(readBool("Inputs", "UseFsr2VulkanInputs"));
            Fsr2Pattern.set_from_config(readBool("Inputs", "Fsr2Pattern"));

            EnableFsr3Inputs.set_from_config(readBool("Inputs", "EnableFsr3Inputs"));
            UseFsr3Inputs.set_from_config(readBool("Inputs", "UseFsr3Inputs"));
            Fsr3Pattern.set_from_config(readBool("Inputs", "Fsr3Pattern"));

            EnableFfxInputs.set_from_config(readBool("Inputs", "EnableFfxInputs"));
            UseFfxInputs.set_from_config(readBool("Inputs", "UseFfxInputs"));
            EnableHotSwapping.set_from_config(readBool("Inputs", "EnableHotSwapping"));
        }

        // Plugins
        {
            PluginPath.set_from_config(readWString("Plugins", "Path"));
            LoadSpecialK.set_from_config(readBool("Plugins", "LoadSpecialK"));
            LoadReShade.set_from_config(readBool("Plugins", "LoadReShade"));
            LoadCustomAmdxc64OnRdna2.set_from_config(readBool("Plugins", "LoadCustomAmdxc64OnRdna2"));
            LoadAsiPlugins.set_from_config(readBool("Plugins", "LoadAsiPlugins"));
            LateAsiPluginsDelay.set_from_config(readInt("Plugins", "LateAsiPluginsDelay"));
            DisabledAsi.set_from_config(readWString("Plugins", "DisabledAsi"));
        }

        // HDR
        {
            HdrOutputEnabled.set_from_config(readBool("HDR", "Enabled"));

            if (readBool("HDR", "ForceHDR").value_or(false))
            {
                if (!HdrOutputEnabled.has_value())
                {
                    HdrOutputEnabled.set_from_config(true);
                    LOG_INFO("[HDR] ForceHDR is now 'HDR output' (Enabled=true)");
                }
                else
                {
                    LOG_INFO("[HDR] ForceHDR ignored, Enabled={} is set", HdrOutputEnabled.value());
                }
            }

            HdrOutputConvert.set_from_config(readBool("HDR", "Convert"));
            HdrCompareKey.set_from_config(readInt("HDR", "CompareKey"));
            UseHDR10.set_from_config(readBool("HDR", "UseHDR10"));
            HdrOutputPeakNits.set_from_config(readFloat("HDR", "PeakNits"));
            HdrOutputGameNits.set_from_config(readFloat("HDR", "GameNits"));
            HdrOutputDecode.set_from_config(readInt("HDR", "Decode"));

            // NaN / Inf would pass through the clamps and the pass would draw them: back to the default
            if (HdrOutputPeakNits.has_value() &&
                (!std::isfinite(HdrOutputPeakNits.value()) || HdrOutputPeakNits.value() < 0.0f))
                HdrOutputPeakNits.reset();

            if (HdrOutputGameNits.has_value() && !std::isfinite(HdrOutputGameNits.value()))
                HdrOutputGameNits.reset();

            if (HdrOutputGameNits.has_value())
                HdrOutputGameNits = std::clamp(HdrOutputGameNits.value(), 80.0f, 1000.0f);

            if (HdrOutputDecode.has_value() && (HdrOutputDecode.value() < 0 || HdrOutputDecode.value() > 1))
                HdrOutputDecode.reset();
        }

        // V-Sync
        {
            OverrideVsync.set_from_config(readBool("V-Sync", "OverrideVsync"));
            ForceVsync.set_from_config(readBool("V-Sync", "ForceVsync"));
            VsyncInterval.set_from_config(readInt("V-Sync", "SyncInterval"));
        }

        // Libraries
        {
            MainDllPath.set_from_config(readWString("Libraries", "OptiDllPath"));

            NvngxPath.set_from_config(readWString("Libraries", "NvngxPath"));
            NVNGX_DLSS_Library.set_from_config(readWString("Libraries", "NvngxDlssPath"));
            DLSSFeaturePath.set_from_config(readWString("Libraries", "NvngxFeaturePath"));
            NvapiDllPath.set_from_config(readWString("Libraries", "NvapiPath"));

            FfxDx12Path.set_from_config(readWString("Libraries", "FfxDx12Path"));
            FfxDx12SRPath.set_from_config(readWString("Libraries", "FfxDx12SRPath"));
            FfxDx12FGPath.set_from_config(readWString("Libraries", "FfxDx12FGPath"));
            FfxDx12RRPath.set_from_config(readWString("Libraries", "FfxDx12RRPath"));
            FfxDx12RCPath.set_from_config(readWString("Libraries", "FfxDx12RCPath"));
            FfxVkPath.set_from_config(readWString("Libraries", "FfxVkPath"));

            XeSSLibrary.set_from_config(readWString("Libraries", "XeSSPath"));
            XeFGLibrary.set_from_config(readWString("Libraries", "XeFGPath"));
            XeLLLibrary.set_from_config(readWString("Libraries", "XeLLPath"));
            XeSSDx11Library.set_from_config(readWString("Libraries", "XeSSDx11Path"));
        }

        // Reading old configs for compatibility reasons
        {
            _DONTUSE_Fsr4ForceEnableInt8.set_from_config(readBool("FSR", "Fsr4ForceEnableInt8"));

            // Named Fsr4ForceModel in older nightlies
            if (!Fsr4GpuSupport.has_value())
            {
                if (auto v = readEnum<FSR4Support>("FSR", "Fsr4ForceModel"))
                    Fsr4GpuSupport.set_from_config(*v);
            }
        }

        savedDlssNr = DlssNrSnapshot();

        return true;
    }

    // No ini: the saved NR settings are the defaults, else they read as changed until the first Save Settings
    savedDlssNr = DlssNrSnapshot();
    return false;
}

bool Config::LoadFromPath(const wchar_t* InPath)
{
    std::filesystem::path iniPath(InPath);
    auto newPath = iniPath / fileName;

    if (Reload(newPath))
    {
        absoluteFileName = newPath;
        return true;
    }

    return false;
}

std::string GetBoolValue(std::optional<bool> value)
{
    if (!value.has_value())
        return "auto";

    return value.value() ? "true" : "false";
}

template <typename T> std::string GetIntValue(std::optional<T> value, bool getHex = false)
{
    if (!value.has_value())
        return "auto";

    if constexpr (std::is_enum_v<T>)
    {
        using Underlying = std::underlying_type_t<T>;
        Underlying v = static_cast<Underlying>(value.value());

        if (getHex)
            return std::format("{:#x}", v);

        return std::to_string(v);
    }
    else
    {
        if (getHex)
            return std::format("{:#x}", value.value());

        return std::to_string(value.value());
    }
}

std::string GetFloatValue(std::optional<float> value)
{
    if (!value.has_value())
        return "auto";

    return std::to_string(value.value());
}

// [DlssNr], read through readSource: the ini at a load, a profile or the saved ini from the menu (Config::LoadDlssNrProfile).
// Each key is read with set_from_config, which fills only an empty setting.
void Config::ReadDlssNr()
{
    // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
    DlssNrEnabled.set_from_config(readBool("DlssNr", "Enabled"));
    DlssNrRunBeforeSr.set_from_config(readBool("DlssNr", "RunBeforeSR"));
    DlssNrFinishedPicture.set_from_config(readBool("DlssNr", "FinishedPicture"));
    DlssNrFinishedGameFgMode.set_from_config(readUInt("DlssNr", "FinishedGameFgMode"));
    if (DlssNrFinishedGameFgMode.value_or_default() > 1)
        DlssNrFinishedGameFgMode = 0u;
    DlssNrDeferredDlss.set_from_config(readBool("DlssNr", "DeferredDLSS"));
    // ResidualFG and ResidualFGApproxCamera are no longer read: NR every second frame through NVIDIA Frame
    // Generation is off for good, whatever an old ini says.
    DlssNrPrecision.set_from_config(readUInt("DlssNr", "Precision"));
    if (DlssNrPrecision.value_or_default() != 4) DlssNrPrecision = 0u;
    DlssNrVitEvery.set_from_config(readUInt("DlssNr", "VitEvery"));
    if (DlssNrVitEvery.value_or_default() < 1u || DlssNrVitEvery.value_or_default() > 2u)
        DlssNrVitEvery = std::clamp<uint32_t>(DlssNrVitEvery.value_or_default(), 1u, 2u);
    DlssNrVitEveryPlain.set_from_config(readUInt("DlssNr", "VitEveryPlain"));
    if (DlssNrVitEveryPlain.value_or_default() < 1u || DlssNrVitEveryPlain.value_or_default() > 2u)
        DlssNrVitEveryPlain = std::clamp<uint32_t>(DlssNrVitEveryPlain.value_or_default(), 1u, 2u);
    DlssNrDetailReuse.set_from_config(readBool("DlssNr", "DetailReuse"));
    DlssNrDetailReuseDebug.set_from_config(readBool("DlssNr", "DetailReuseDebug"));
    DlssNrDetailReuseSteady.set_from_config(readFloat("DlssNr", "DetailReuseSteady"));
    DlssNrDetailReuseFill.set_from_config(readFloat("DlssNr", "DetailReuseFill"));
    DlssNrDetailReuseInterval.set_from_config(readUInt("DlssNr", "DetailReuseInterval"));
    if (DlssNrDetailReuseInterval.has_value())
        DlssNrDetailReuseInterval = std::clamp(DlssNrDetailReuseInterval.value(), 2u, 3u);
    DlssNrDetailReuseSpread.set_from_config(readUInt("DlssNr", "DetailReuseSpread"));
    if (DlssNrDetailReuseSpread.has_value())
        DlssNrDetailReuseSpread = std::min(DlssNrDetailReuseSpread.value(), 3u);
    DlssNrDetailReuseDepthTolerance.set_from_config(readFloat("DlssNr", "DetailReuseDepthTolerance"));
    DlssNrDetailReuseClipGamma.set_from_config(readFloat("DlssNr", "DetailReuseClipGamma"));
    DlssNrDetailReuseClipFalloff.set_from_config(readFloat("DlssNr", "DetailReuseClipFalloff"));
    DlssNrDetailReuseSigmaFloor.set_from_config(readFloat("DlssNr", "DetailReuseSigmaFloor"));
    DlssNrDetailReuseWithFg.set_from_config(readBool("DlssNr", "DetailReuseWithFG"));
    DlssNrDetailReuseMinFps.set_from_config(readFloat("DlssNr", "DetailReuseMinFps"));
    DlssNrSceneCut.set_from_config(readUInt("DlssNr", "SceneCut"));
    if (DlssNrSceneCut.value_or_default() > 2u)
        DlssNrSceneCut = 2u;
    DlssNrEditCache.set_from_config(readBool("DlssNr", "EditCache"));
    DlssNrEditCacheInterval.set_from_config(readUInt("DlssNr", "EditCacheInterval"));
    if (DlssNrEditCacheInterval.has_value())
        DlssNrEditCacheInterval = std::clamp(DlssNrEditCacheInterval.value(), 2u, 8u);
    DlssNrEditCacheCrossfade.set_from_config(readUInt("DlssNr", "EditCacheCrossfade"));
    if (DlssNrEditCacheCrossfade.has_value())
        DlssNrEditCacheCrossfade = std::min(DlssNrEditCacheCrossfade.value(), 8u); // 0 = follow the interval
    DlssNrEditCacheDebug.set_from_config(readUInt("DlssNr", "EditCacheDebug"));
    if (DlssNrEditCacheDebug.value_or_default() > 2u)
        DlssNrEditCacheDebug = 0u;
    DlssNrDetailReuseMaxDropped.set_from_config(readFloat("DlssNr", "DetailReuseMaxDropped"));
    if (DlssNrDetailReuseMaxDropped.has_value() &&
        (!std::isfinite(DlssNrDetailReuseMaxDropped.value()) || DlssNrDetailReuseMaxDropped.value() < 0.0f ||
         DlssNrDetailReuseMaxDropped.value() > 50.0f))
        DlssNrDetailReuseMaxDropped = std::isfinite(DlssNrDetailReuseMaxDropped.value())
                                          ? std::clamp(DlssNrDetailReuseMaxDropped.value(), 0.0f, 50.0f)
                                          : 10.0f;
    if (DlssNrDetailReuseMinFps.has_value() &&
        (!std::isfinite(DlssNrDetailReuseMinFps.value()) || DlssNrDetailReuseMinFps.value() < 0.0f ||
         DlssNrDetailReuseMinFps.value() > 240.0f))
        DlssNrDetailReuseMinFps = std::isfinite(DlssNrDetailReuseMinFps.value())
                                      ? std::clamp(DlssNrDetailReuseMinFps.value(), 0.0f, 240.0f)
                                      : 25.0f;
    if (DlssNrDetailReuseFill.has_value() &&
        (!std::isfinite(DlssNrDetailReuseFill.value()) || DlssNrDetailReuseFill.value() < 0.0f ||
         DlssNrDetailReuseFill.value() > 1.0f))
        DlssNrDetailReuseFill = std::isfinite(DlssNrDetailReuseFill.value())
                                    ? std::clamp(DlssNrDetailReuseFill.value(), 0.0f, 1.0f)
                                    : 1.0f;
    if (DlssNrDetailReuseSteady.has_value() &&
        (!std::isfinite(DlssNrDetailReuseSteady.value()) || DlssNrDetailReuseSteady.value() < 0.0f ||
         DlssNrDetailReuseSteady.value() > 1.0f))
        DlssNrDetailReuseSteady = std::isfinite(DlssNrDetailReuseSteady.value())
                                      ? std::clamp(DlssNrDetailReuseSteady.value(), 0.0f, 1.0f)
                                      : 0.0f;
    if (DlssNrDetailReuseDepthTolerance.has_value() &&
        (!std::isfinite(DlssNrDetailReuseDepthTolerance.value()) ||
         DlssNrDetailReuseDepthTolerance.value() < 0.0f || DlssNrDetailReuseDepthTolerance.value() > 1.0f))
    {
        // Out of range is clamped, but a value that is not a number is CLEARED rather than set to a
        // literal: the default lives in one place (kDlssNrDetailReuseDepthTolerance) and a copy here would
        // silently go stale the next time it moves, with SaveIni then writing the stale value back.
        if (std::isfinite(DlssNrDetailReuseDepthTolerance.value()))
            DlssNrDetailReuseDepthTolerance = std::clamp(DlssNrDetailReuseDepthTolerance.value(), 0.0f, 1.0f);
        else
            DlssNrDetailReuseDepthTolerance = std::optional<float> {};
    }
    if (DlssNrDetailReuseClipGamma.has_value() &&
        (!std::isfinite(DlssNrDetailReuseClipGamma.value()) || DlssNrDetailReuseClipGamma.value() < 0.0f ||
         DlssNrDetailReuseClipGamma.value() > 10.0f))
        DlssNrDetailReuseClipGamma = std::isfinite(DlssNrDetailReuseClipGamma.value())
                                         ? std::clamp(DlssNrDetailReuseClipGamma.value(), 0.0f, 10.0f)
                                         : 1.25f;
    // Never 0: the shader divides by max(clipFalloff, 1e-3), so at 0 trust would stop fading and go binary.
    if (DlssNrDetailReuseClipFalloff.has_value() &&
        (!std::isfinite(DlssNrDetailReuseClipFalloff.value()) || DlssNrDetailReuseClipFalloff.value() < 0.01f ||
         DlssNrDetailReuseClipFalloff.value() > 10.0f))
        DlssNrDetailReuseClipFalloff = std::isfinite(DlssNrDetailReuseClipFalloff.value())
                                           ? std::clamp(DlssNrDetailReuseClipFalloff.value(), 0.01f, 10.0f)
                                           : 1.0f;
    if (DlssNrDetailReuseSigmaFloor.has_value() &&
        (!std::isfinite(DlssNrDetailReuseSigmaFloor.value()) || DlssNrDetailReuseSigmaFloor.value() < 0.0f ||
         DlssNrDetailReuseSigmaFloor.value() > 1.0f))
        DlssNrDetailReuseSigmaFloor = std::isfinite(DlssNrDetailReuseSigmaFloor.value())
                                          ? std::clamp(DlssNrDetailReuseSigmaFloor.value(), 0.0f, 1.0f)
                                          : 0.01f;
    DlssNrToggleKey.set_from_config(readInt("DlssNr", "ToggleKey"));
    DlssNrTransferStrength.set_from_config(readFloat("DlssNr", "TransferStrength"));
    DlssNrColourStrength.set_from_config(readFloat("DlssNr", "ColourStrength"));
    DlssNrReplaceDetailStrength.set_from_config(readFloat("DlssNr", "ReplaceDetailStrength"));
    DlssNrMaxRatio.set_from_config(readFloat("DlssNr", "MaxRatio"));
    DlssNrTransfer.set_from_config(readUInt("DlssNr", "Transfer"));
    DlssNrSuperiorScalingPreset.set_from_config(readUInt("DlssNr", "SuperiorScalingPreset"));

    DlssNrWhitePointFromExposure.set_from_config(readBool("DlssNr", "WhitePointFromExposure"));
    DlssNrProbeD3D11.set_from_config(readBool("DlssNr", "ProbeD3D11"));
    DlssNrFrameStats.set_from_config(readBool("DlssNr", "FrameStats"));
    DlssNrKernelProfile.set_from_config(readBool("DlssNr", "KernelProfile"));
    DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));
    DlssNrCompare.set_from_config(readUInt("DlssNr", "Compare"));
    DlssNrCompareSplit.set_from_config(readFloat("DlssNr", "CompareSplit"));
    DlssNrCompareZoom.set_from_config(readFloat("DlssNr", "CompareZoom"));
    DlssNrCompareSwap.set_from_config(readBool("DlssNr", "CompareSwap"));
    DlssNrCompareTags.set_from_config(readBool("DlssNr", "CompareTags"));
    DlssNrTagScale.set_from_config(readFloat("DlssNr", "TagScale"));
    DlssNrWorkingScale.set_from_config(readFloat("DlssNr", "WorkingScale"));
    DlssNrReducedUpscaleMethod.set_from_config(readUInt("DlssNr", "ReducedUpscaleMethod"));
    // Only 0 (Bilinear) and 1 (SGSR1) exist -- an old config or hand-edited ini past that
    // range falls back to Bilinear rather than being read raw by the enlarge-filter checks.
    if (DlssNrReducedUpscaleMethod.has_value() && DlssNrReducedUpscaleMethod.value() > 1u)
        DlssNrReducedUpscaleMethod = 0u;
    DlssNrModelResolutionAuto.set_from_config(readBool("DlssNr", "ModelResolutionAuto"));
    {
        const auto flatten = readUInt("DlssNr", "InputFlatten");
        const auto flattenStrength = readFloat("DlssNr", "InputFlattenStrength");
        const auto structure = readBool("DlssNr", "InputStructureHint");
        const auto structureStrength = readFloat("DlssNr", "InputStructureStrength");
        DlssNrInputStyleFacesOnly.set_from_config(readBool("DlssNr", "InputStyleFacesOnly"));

        const bool newKeys = flatten.has_value() || flattenStrength.has_value() || structure.has_value() ||
                             structureStrength.has_value();
        const auto single = readUInt("DlssNr", "InputStyle");

        if (!newKeys && single.has_value() && single.value() >= 1u && single.value() <= 3u)
        {
            // An ini from before the card: its one style, at its strength
            const float strength = readFloat("DlssNr", "InputStyleStrength").value_or(1.0f);
            SetDlssNrInputStyles(DlssNrInput::FromSingle(single.value(), strength,
                                                         DlssNrInputStyleFacesOnly.value_or_default()));
        }
        else
        {
            DlssNrInputFlatten.set_from_config(flatten);
            if (DlssNrInputFlatten.has_value() && DlssNrInputFlatten.value() > DlssNrInput::kFlattenSmoothCanvas)
                DlssNrInputFlatten.reset();
            DlssNrInputFlattenStrength.set_from_config(flattenStrength);
            DlssNrInputStructureHint.set_from_config(structure);
            DlssNrInputStructureStrength.set_from_config(structureStrength);
        }

        if (DlssNrInputFlattenStrength.has_value())
            DlssNrInputFlattenStrength = std::clamp(DlssNrInputFlattenStrength.value(), 0.0f, 1.0f);
        if (DlssNrInputStructureStrength.has_value())
            DlssNrInputStructureStrength = std::clamp(DlssNrInputStructureStrength.value(), 0.0f, 3.0f);
    }
    DlssNrSpatialCompression.set_from_config(readBool("DlssNr", "SpatialCompression"));
    DlssNrSpatialCenterX.set_from_config(readFloat("DlssNr", "SpatialCenterX"));
    DlssNrSpatialCenterY.set_from_config(readFloat("DlssNr", "SpatialCenterY"));
    DlssNrSpatialWorkX.set_from_config(readFloat("DlssNr", "SpatialWorkX"));
    DlssNrSpatialWorkY.set_from_config(readFloat("DlssNr", "SpatialWorkY"));
    DlssNrSpatialOffsetX.set_from_config(readFloat("DlssNr", "SpatialOffsetX"));
    DlssNrSpatialOffsetY.set_from_config(readFloat("DlssNr", "SpatialOffsetY"));
    DlssNrSpatialShiftX.set_from_config(readFloat("DlssNr", "SpatialShiftX"));
    DlssNrSpatialShiftY.set_from_config(readFloat("DlssNr", "SpatialShiftY"));

    if (auto v = readEnum<Scaler>("DlssNr", "ScalingDownscaler"))
        DlssNrScalingDownscaler.set_from_config(*v);
    else
        DlssNrScalingDownscaler.reset();
    DlssNrProxyProbe.set_from_config(readBool("DlssNr", "ProxyProbe"));
    DlssNrUseProxy.set_from_config(readBool("DlssNr", "UseProxy"));
    DlssNrScanExposure.set_from_config(readBool("DlssNr", "ScanExposure"));
    DlssNrRecordExposureScan.set_from_config(readBool("DlssNr", "RecordExposureScan"));
    DlssNrWhitePointSource.set_from_config(readUInt("DlssNr", "WhitePointSource"));
    DlssNrGameExposureScale.set_from_config(readUInt("DlssNr", "GameExposureScale"));
    if (DlssNrGameExposureScale.has_value() && DlssNrGameExposureScale.value() > 2)
        DlssNrGameExposureScale.reset();
    DlssNrAutoExposureTrim.set_from_config(readFloat("DlssNr", "AutoExposureTrim"));
    DlssNrAutoExposureShadowProtection.set_from_config(readFloat("DlssNr", "AutoExposureShadowProtection"));
    DlssNrAutoExposureMeter.set_from_config(readUInt("DlssNr", "AutoExposureMeter"));
    DlssNrAutoExposureMeterLowPercent.set_from_config(readFloat("DlssNr", "AutoExposureMeterLowPercent"));
    DlssNrAutoExposureMeterHighPercent.set_from_config(readFloat("DlssNr", "AutoExposureMeterHighPercent"));
    DlssNrNativeDepthFinder.set_from_config(readBool("DlssNr", "NativeDepthFinder"));
    DlssNrNativeDepthVkCopyUsage.set_from_config(readBool("DlssNr", "NativeDepthVkCopyUsage"));
    DlssNrNativeDepthResolveInList.set_from_config(readBool("DlssNr", "NativeDepthResolveInList"));
    DlssNrNativeDxvkVulkan.set_from_config(readBool("DlssNr", "NativeDxvkVulkan"));
    DlssNrNativeDepthWarmupFrames.set_from_config(readUInt("DlssNr", "NativeDepthWarmupFrames"));
    DlssNrNativeDepthOverlay.set_from_config(readBool("DlssNr", "NativeDepthOverlay"));
    DlssNrNativeDebugView.set_from_config(readBool("DlssNr", "NativeDebugView"));
    DlssNrNativeMotionProfile.set_from_config(
        readString("DlssNr", "NativeMotionProfile", true).transform(native::motion::ProfileFromIniText));
    DlssNrNativeMotionProfileCustom.set_from_config(readString("DlssNr", "NativeMotionProfileCustom"));
    DlssNrNativeMotion.set_from_config(readBool("DlssNr", "NativeMotion"));
    DlssNrNativeInput.set_from_config(readBool("DlssNr", "NativeInput"));
    DlssNrNativeUpscaler.set_from_config(readBool("DlssNr", "NativeUpscaler"));
    DlssNrNativeUpscalerPass.set_from_config(readBool("DlssNr", "NativeUpscalerPass"));
    DlssNrNativeFrameGenerationOnly.set_from_config(readBool("DlssNr", "NativeFrameGenerationOnly"));
    DlssNrNativeLowLatencyBoost.set_from_config(readBool("DlssNr", "NativeLowLatencyBoost"));
    DlssNrNativeLowLatencyDxvkGameThread.set_from_config(readBool("DlssNr", "NativeLowLatencyDxvkGameThread"));
    DlssNrAutoExposureAdaptBrighterSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptBrighterSeconds"));
    DlssNrAutoExposureAdaptDarkerSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptDarkerSeconds"));
    DlssNrAutoExposureAdaptSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptSeconds"));
    // An ini from before the two speeds were split keeps its one speed both ways.
    if (readSource == &ini && DlssNrAutoExposureAdaptSeconds.has_value()) // not from a profile: left-out settings
    {
        if (!DlssNrAutoExposureAdaptBrighterSeconds.has_value())
            DlssNrAutoExposureAdaptBrighterSeconds = DlssNrAutoExposureAdaptSeconds.value();
        if (!DlssNrAutoExposureAdaptDarkerSeconds.has_value())
            DlssNrAutoExposureAdaptDarkerSeconds = DlssNrAutoExposureAdaptSeconds.value();
    }
    DlssNrAutoExposureFollowGame.set_from_config(readBool("DlssNr", "AutoExposureFollowGame"));
    DlssNrGameExposureTrimAnchors.set_from_config(readString("DlssNr", "GameExposureTrimAnchors"));
    DlssNrAutoExposureTrimAnchors.set_from_config(readString("DlssNr", "AutoExposureTrimAnchors"));

    // Migrate the retired flag only when the new key was genuinely absent, which is why this
    // has to run AFTER the read above -- set_from_config assigns only into an empty optional,
    // so an ini that carries both keys keeps its explicit WhitePointSource.
    if (readSource == &ini && !DlssNrWhitePointSource.has_value() && DlssNrWhitePointFromExposure.has_value())
        DlssNrWhitePointSource = DlssNrWhitePointFromExposure.value() ? 1u : 0u;
    DlssNrScanMeter.set_from_config(readBool("DlssNr", "ScanMeter"));
    DlssNrScanTrim.set_from_config(readFloat("DlssNr", "ScanTrim"));
    DlssNrPasses.set_from_config(readUInt("DlssNr", "Passes"));
    DlssNrPassFeedback.set_from_config(readFloat("DlssNr", "PassFeedback"));
    DlssNrScanAnchorValue.set_from_config(readFloat("DlssNr", "ScanAnchorValue"));
    DlssNrScanAnchorWhitePoint.set_from_config(readFloat("DlssNr", "ScanAnchorWhitePoint"));
    DlssNrScanAnchors.set_from_config(readString("DlssNr", "ScanAnchors"));
    DlssNrScanInverted.set_from_config(readBool("DlssNr", "ScanInverted"));
    DlssNrWhitePointTrim.set_from_config(readFloat("DlssNr", "WhitePointTrim"));
    DlssNrAutoCapture.set_from_config(readBool("DlssNr", "AutoCapture"));
    DlssNrWhitePointScale.set_from_config(readFloat("DlssNr", "WhitePointScale"));
    DlssNrPreset.set_from_config(readUInt("DlssNr", "Preset"));
    DlssNrIntensity.set_from_config(readFloat("DlssNr", "Intensity"));
    DlssNrStyle.set_from_config(readUInt("DlssNr", "Style"));
    DlssNrPass2Preset.set_from_config(readUInt("DlssNr", "Pass2Preset"));
    DlssNrPass2Style.set_from_config(readUInt("DlssNr", "Pass2Style"));
    DlssNrPass3Preset.set_from_config(readUInt("DlssNr", "Pass3Preset"));
    DlssNrPass3Style.set_from_config(readUInt("DlssNr", "Pass3Style"));
    DlssNrLocalStructure.set_from_config(readFloat("DlssNr", "LocalStructure"));
    DlssNrLocalTone.set_from_config(readFloat("DlssNr", "LocalTone"));
    DlssNrSkinStructure.set_from_config(readFloat("DlssNr", "SkinStructure"));
    DlssNrAutoMask.set_from_config(readBool("DlssNr", "AutoMask"));
    DlssNrSkinProtection.set_from_config(readBool("DlssNr", "SkinProtection"));
    DlssNrSkinToneEnabled.set_from_config(readBool("DlssNr", "SkinToneEnabled"));
    DlssNrSkinDetail.set_from_config(readFloat("DlssNr", "SkinDetail"));
    DlssNrSkinColour.set_from_config(readFloat("DlssNr", "SkinColour"));
    DlssNrEnvironmentDetail.set_from_config(readFloat("DlssNr", "EnvironmentDetail"));
    DlssNrEnvironmentColour.set_from_config(readFloat("DlssNr", "EnvironmentColour"));
    DlssNrShowSkinMask.set_from_config(readBool("DlssNr", "ShowSkinMask"));
    DlssNrPass2Intensity.set_from_config(readFloat("DlssNr", "Pass2Intensity"));
    DlssNrPass2LocalStructure.set_from_config(readFloat("DlssNr", "Pass2LocalStructure"));
    DlssNrPass2LocalTone.set_from_config(readFloat("DlssNr", "Pass2LocalTone"));
    DlssNrPass2SkinStructure.set_from_config(readFloat("DlssNr", "Pass2SkinStructure"));
    DlssNrPass2AutoMask.set_from_config(readBool("DlssNr", "Pass2AutoMask"));
    DlssNrPass3Intensity.set_from_config(readFloat("DlssNr", "Pass3Intensity"));
    DlssNrPass3LocalStructure.set_from_config(readFloat("DlssNr", "Pass3LocalStructure"));
    DlssNrPass3LocalTone.set_from_config(readFloat("DlssNr", "Pass3LocalTone"));
    DlssNrPass3SkinStructure.set_from_config(readFloat("DlssNr", "Pass3SkinStructure"));
    DlssNrPass3AutoMask.set_from_config(readBool("DlssNr", "Pass3AutoMask"));
    DlssNrUnlockPasses.set_from_config(readBool("DlssNr", "UnlockPasses"));
    for (unsigned int i = 0; i < 27; ++i)
    {
        auto& pass = DlssNrExtraPasses[i];
        pass.style.set_from_config(readUInt("DlssNr", std::format("Pass{}Style", i + 4).c_str()));
        pass.intensity.set_from_config(readFloat("DlssNr", std::format("Pass{}Intensity", i + 4).c_str()));
        pass.structure.set_from_config(readFloat("DlssNr", std::format("Pass{}LocalStructure", i + 4).c_str()));
        pass.tone.set_from_config(readFloat("DlssNr", std::format("Pass{}LocalTone", i + 4).c_str()));
        pass.skin.set_from_config(readFloat("DlssNr", std::format("Pass{}SkinStructure", i + 4).c_str()));
        pass.autoMask.set_from_config(readBool("DlssNr", std::format("Pass{}AutoMask", i + 4).c_str()));
    }
    DlssNrReversibleMode.set_from_config(readUInt("DlssNr", "ReversibleMode"));
    if (!DlssNrProxyCurve::Valid(DlssNrReversibleMode.value_or_default())) // not a curve: soft knee, saved as auto
        DlssNrReversibleMode.reset();
    DlssNrColourEncoding.set_from_config(readUInt("DlssNr", "ColourEncoding"));
    if (DlssNrColourEncoding.value_or_default() > 4u) // not a Colour encoding: back to Auto, saved as auto
        DlssNrColourEncoding.reset();
    DlssNrDisplayPaperWhiteNits.set_from_config(readFloat("DlssNr", "DisplayPaperWhiteNits"));
    DlssNrLutFile.set_from_config(readString("DlssNr", "LutFile"));
    DlssNrLutStrength.set_from_config(readFloat("DlssNr", "LutStrength"));
    if (DlssNrLutStrength.has_value() && (!std::isfinite(DlssNrLutStrength.value()) ||
                                          DlssNrLutStrength.value() < 0.0f || DlssNrLutStrength.value() > 1.0f))
        DlssNrLutStrength =
            std::isfinite(DlssNrLutStrength.value()) ? std::clamp(DlssNrLutStrength.value(), 0.0f, 1.0f) : 1.0f;
    DlssNrApplyModel.set_from_config(readBool("DlssNr", "ApplyModel"));
    DlssNrHoldFrame.set_from_config(readBool("DlssNr", "HoldFrame"));
}

// [DlssNr] as SaveIni writes it, from the config given (not Instance(): a load snapshots it while the config is being
// built), into the ini given: OptiScaler.ini, or a profile / snapshot (DlssNr_Profiles.h).
static void WriteDlssNr(Config& c, CSimpleIniA& out)
{
    // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
    out.SetValue("DlssNr", "Enabled", GetBoolValue(c.DlssNrEnabled.value_for_config()).c_str());
    out.SetValue("DlssNr", "FinishedPicture", GetBoolValue(c.DlssNrFinishedPicture.value_for_config()).c_str());
    out.SetValue("DlssNr", "FinishedGameFgMode", GetIntValue(c.DlssNrFinishedGameFgMode.value_for_config()).c_str());
    out.SetValue("DlssNr", "RunBeforeSR",
                 GetBoolValue(c.DlssNrRunBeforeSr.value_for_config()).c_str());
    out.SetValue("DlssNr", "DeferredDLSS",
                 GetBoolValue(c.DlssNrDeferredDlss.value_for_config()).c_str());
    out.SetValue("DlssNr", "Precision", GetIntValue(c.DlssNrPrecision.value_for_config()).c_str());
    out.SetValue("DlssNr", "VitEvery", GetIntValue(c.DlssNrVitEvery.value_for_config()).c_str());
    out.SetValue("DlssNr", "VitEveryPlain", GetIntValue(c.DlssNrVitEveryPlain.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuse", GetBoolValue(c.DlssNrDetailReuse.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseDebug", GetBoolValue(c.DlssNrDetailReuseDebug.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseSteady", GetFloatValue(c.DlssNrDetailReuseSteady.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseFill", GetFloatValue(c.DlssNrDetailReuseFill.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseInterval", GetIntValue(c.DlssNrDetailReuseInterval.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseSpread", GetIntValue(c.DlssNrDetailReuseSpread.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseDepthTolerance", GetFloatValue(c.DlssNrDetailReuseDepthTolerance.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseClipGamma", GetFloatValue(c.DlssNrDetailReuseClipGamma.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseClipFalloff", GetFloatValue(c.DlssNrDetailReuseClipFalloff.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseSigmaFloor", GetFloatValue(c.DlssNrDetailReuseSigmaFloor.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseWithFG", GetBoolValue(c.DlssNrDetailReuseWithFg.value_for_config()).c_str());
    out.SetValue("DlssNr", "SceneCut", GetIntValue(c.DlssNrSceneCut.value_for_config()).c_str());
    out.SetValue("DlssNr", "EditCache", GetBoolValue(c.DlssNrEditCache.value_for_config()).c_str());
    out.SetValue("DlssNr", "EditCacheInterval", GetIntValue(c.DlssNrEditCacheInterval.value_for_config()).c_str());
    out.SetValue("DlssNr", "EditCacheCrossfade", GetIntValue(c.DlssNrEditCacheCrossfade.value_for_config()).c_str());
    out.SetValue("DlssNr", "EditCacheDebug", GetIntValue(c.DlssNrEditCacheDebug.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseMinFps", GetFloatValue(c.DlssNrDetailReuseMinFps.value_for_config()).c_str());
    out.SetValue("DlssNr", "DetailReuseMaxDropped", GetFloatValue(c.DlssNrDetailReuseMaxDropped.value_for_config()).c_str());
    {
        auto toggle = c.DlssNrToggleKey.value_for_config();
        out.SetValue("DlssNr", "ToggleKey", GetIntValue(toggle, toggle > 0).c_str());
    }
    out.SetValue("DlssNr", "TransferStrength",
                 GetFloatValue(c.DlssNrTransferStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "ColourStrength",
                 GetFloatValue(c.DlssNrColourStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "ReplaceDetailStrength",
                 GetFloatValue(c.DlssNrReplaceDetailStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "MaxRatio", GetFloatValue(c.DlssNrMaxRatio.value_for_config()).c_str());
    out.SetValue("DlssNr", "Transfer", GetIntValue(c.DlssNrTransfer.value_for_config()).c_str());
    out.SetValue("DlssNr", "SuperiorScalingPreset",
                 GetIntValue(c.DlssNrSuperiorScalingPreset.value_for_config()).c_str());

    out.SetValue("DlssNr", "ProbeD3D11",
                 GetBoolValue(c.DlssNrProbeD3D11.value_for_config()).c_str());
    out.SetValue("DlssNr", "FrameStats",
                 GetBoolValue(c.DlssNrFrameStats.value_for_config()).c_str());
    out.SetValue("DlssNr", "KernelProfile",
                 GetBoolValue(c.DlssNrKernelProfile.value_for_config()).c_str());
    out.SetValue("DlssNr", "WhitePointFromExposure",
                 GetBoolValue(c.DlssNrWhitePointFromExposure.value_for_config()).c_str());
    out.SetValue("DlssNr", "DebugView", GetIntValue(c.DlssNrDebugView.value_for_config()).c_str());
    out.SetValue("DlssNr", "Compare", GetIntValue(c.DlssNrCompare.value_for_config()).c_str());
    out.SetValue("DlssNr", "CompareSplit",
                 GetFloatValue(c.DlssNrCompareSplit.value_for_config()).c_str());
    out.SetValue("DlssNr", "CompareZoom",
                 GetFloatValue(c.DlssNrCompareZoom.value_for_config()).c_str());
    out.SetValue("DlssNr", "CompareSwap",
                 GetBoolValue(c.DlssNrCompareSwap.value_for_config()).c_str());
    out.SetValue("DlssNr", "CompareTags",
                 GetBoolValue(c.DlssNrCompareTags.value_for_config()).c_str());
    out.SetValue("DlssNr", "TagScale",
                 GetFloatValue(c.DlssNrTagScale.value_for_config()).c_str());
    out.SetValue("DlssNr", "WorkingScale", GetFloatValue(c.DlssNrWorkingScale.value_for_config()).c_str());
    out.SetValue("DlssNr", "ReducedUpscaleMethod",
                 GetIntValue(c.DlssNrReducedUpscaleMethod.value_for_config()).c_str());
    out.SetValue("DlssNr", "ModelResolutionAuto",
                 GetBoolValue(c.DlssNrModelResolutionAuto.value_for_config()).c_str());
    out.SetValue("DlssNr", "InputFlatten", GetIntValue(c.DlssNrInputFlatten.value_for_config()).c_str());
    out.SetValue("DlssNr", "InputFlattenStrength",
                 GetFloatValue(c.DlssNrInputFlattenStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "InputStructureHint",
                 GetBoolValue(c.DlssNrInputStructureHint.value_for_config()).c_str());
    out.SetValue("DlssNr", "InputStructureStrength",
                 GetFloatValue(c.DlssNrInputStructureStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "InputStyleFacesOnly",
                 GetBoolValue(c.DlssNrInputStyleFacesOnly.value_for_config()).c_str());

    // The old single key for a build from before the card: the first style that runs (auto when none does)
    {
        const auto first = DlssNrInput::ToSingle(c.DlssNrInputStyles());
        out.SetValue("DlssNr", "InputStyle", first.style == 0u ? "auto" : std::to_string(first.style).c_str());
        out.SetValue("DlssNr", "InputStyleStrength",
                     first.style == 0u ? "auto" : GetFloatValue(std::optional<float>(first.strength)).c_str());
    }
    out.SetValue("DlssNr", "SpatialCompression",
                 GetBoolValue(c.DlssNrSpatialCompression.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialCenterX", GetFloatValue(c.DlssNrSpatialCenterX.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialCenterY", GetFloatValue(c.DlssNrSpatialCenterY.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialWorkX", GetFloatValue(c.DlssNrSpatialWorkX.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialWorkY", GetFloatValue(c.DlssNrSpatialWorkY.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialOffsetX", GetFloatValue(c.DlssNrSpatialOffsetX.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialOffsetY", GetFloatValue(c.DlssNrSpatialOffsetY.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialShiftX", GetFloatValue(c.DlssNrSpatialShiftX.value_for_config()).c_str());
    out.SetValue("DlssNr", "SpatialShiftY", GetFloatValue(c.DlssNrSpatialShiftY.value_for_config()).c_str());
    out.SetValue("DlssNr", "ScalingDownscaler", GetIntValue(c.DlssNrScalingDownscaler).c_str());
    out.SetValue("DlssNr", "AutoCapture", GetBoolValue(c.DlssNrAutoCapture.value_for_config()).c_str());

    // These were read every launch but never written, so nothing set through the menu survived a
    // restart -- the white-point source, both trims, the anchor, the pass count and the rest all
    // reset to default on the next run.
    out.SetValue("DlssNr", "WhitePointSource", GetIntValue(c.DlssNrWhitePointSource.value_for_config()).c_str());
    out.SetValue("DlssNr", "GameExposureScale",
                 GetIntValue(c.DlssNrGameExposureScale.value_for_config()).c_str());
    out.SetValue("DlssNr", "WhitePointTrim", GetFloatValue(c.DlssNrWhitePointTrim.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureTrim", GetFloatValue(c.DlssNrAutoExposureTrim.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureShadowProtection",
                 GetFloatValue(c.DlssNrAutoExposureShadowProtection.value_for_config()).c_str());
    // The one speed both used to share is only read (and copied into both); drop it so a choice equal to a default is not
    // overridden by it on the next load.
    out.Delete("DlssNr", "AutoExposureAdaptSeconds");
    out.SetValue("DlssNr", "AutoExposureMeter", GetIntValue(c.DlssNrAutoExposureMeter.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureMeterLowPercent",
                 GetFloatValue(c.DlssNrAutoExposureMeterLowPercent.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureMeterHighPercent",
                 GetFloatValue(c.DlssNrAutoExposureMeterHighPercent.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDepthFinder",
                 GetBoolValue(c.DlssNrNativeDepthFinder.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDepthVkCopyUsage",
                 GetBoolValue(c.DlssNrNativeDepthVkCopyUsage.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDepthResolveInList",
                 GetBoolValue(c.DlssNrNativeDepthResolveInList.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDxvkVulkan", GetBoolValue(c.DlssNrNativeDxvkVulkan.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDepthWarmupFrames",
                 GetIntValue(c.DlssNrNativeDepthWarmupFrames.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDepthOverlay",
                 GetBoolValue(c.DlssNrNativeDepthOverlay.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeDebugView",
                 GetBoolValue(c.DlssNrNativeDebugView.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeMotionProfile",
                 native::motion::ProfileIniText(c.DlssNrNativeMotionProfile.value_or_default()));
    out.SetValue("DlssNr", "NativeMotionProfileCustom",
                 c.DlssNrNativeMotionProfileCustom.value_for_config_or("").c_str());
    out.SetValue("DlssNr", "NativeMotion", GetBoolValue(c.DlssNrNativeMotion.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeInput", GetBoolValue(c.DlssNrNativeInput.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeUpscaler",
                 GetBoolValue(c.DlssNrNativeUpscaler.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeUpscalerPass",
                 GetBoolValue(c.DlssNrNativeUpscalerPass.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeFrameGenerationOnly",
                 GetBoolValue(c.DlssNrNativeFrameGenerationOnly.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeLowLatencyBoost",
                 GetBoolValue(c.DlssNrNativeLowLatencyBoost.value_for_config()).c_str());
    out.SetValue("DlssNr", "NativeLowLatencyDxvkGameThread",
                 GetBoolValue(c.DlssNrNativeLowLatencyDxvkGameThread.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureAdaptBrighterSeconds",
                 GetFloatValue(c.DlssNrAutoExposureAdaptBrighterSeconds.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureAdaptDarkerSeconds",
                 GetFloatValue(c.DlssNrAutoExposureAdaptDarkerSeconds.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoExposureFollowGame",
                 GetBoolValue(c.DlssNrAutoExposureFollowGame.value_for_config()).c_str());
    out.SetValue("DlssNr", "GameExposureTrimAnchors",
                 c.DlssNrGameExposureTrimAnchors.value_for_config_or("").c_str());
    out.SetValue("DlssNr", "AutoExposureTrimAnchors",
                 c.DlssNrAutoExposureTrimAnchors.value_for_config_or("").c_str());
    out.SetValue("DlssNr", "ScanTrim", GetFloatValue(c.DlssNrScanTrim.value_for_config()).c_str());
    out.SetValue("DlssNr", "ScanAnchorValue", GetFloatValue(c.DlssNrScanAnchorValue.value_for_config()).c_str());
    out.SetValue("DlssNr", "ScanAnchorWhitePoint", GetFloatValue(c.DlssNrScanAnchorWhitePoint.value_for_config()).c_str());
    out.SetValue("DlssNr", "ScanAnchors", c.DlssNrScanAnchors.value_for_config_or("").c_str());
    out.SetValue("DlssNr", "ScanInverted", GetBoolValue(c.DlssNrScanInverted.value_for_config()).c_str());
    out.SetValue("DlssNr", "ScanMeter", GetBoolValue(c.DlssNrScanMeter.value_for_config()).c_str());
    out.SetValue("DlssNr", "Passes", GetIntValue(c.DlssNrPasses.value_for_config()).c_str());
    out.SetValue("DlssNr", "PassFeedback", GetFloatValue(c.DlssNrPassFeedback.value_for_config()).c_str());
    out.SetValue("DlssNr", "UseProxy", GetBoolValue(c.DlssNrUseProxy.value_for_config()).c_str());
    out.SetValue("DlssNr", "ProxyProbe", GetBoolValue(c.DlssNrProxyProbe.value_for_config()).c_str());
    // ScanExposure and RecordExposureScan are developer overrides with no menu control; persist them so a set ini keeps
    // them.
    out.SetValue("DlssNr", "ScanExposure", GetBoolValue(c.DlssNrScanExposure.value_for_config()).c_str());
    out.SetValue("DlssNr", "RecordExposureScan",
                 GetBoolValue(c.DlssNrRecordExposureScan.value_for_config()).c_str());
    out.SetValue("DlssNr", "WhitePointScale",
                 GetFloatValue(c.DlssNrWhitePointScale.value_for_config()).c_str());
    out.SetValue("DlssNr", "Preset", GetIntValue(c.DlssNrPreset.value_for_config()).c_str());
    out.SetValue("DlssNr", "Intensity", GetFloatValue(c.DlssNrIntensity.value_for_config()).c_str());
    out.SetValue("DlssNr", "Style", GetIntValue(c.DlssNrStyle.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2Preset",
                 GetIntValue(c.DlssNrPass2Preset.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2Style",
                 GetIntValue(c.DlssNrPass2Style.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3Preset",
                 GetIntValue(c.DlssNrPass3Preset.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3Style",
                 GetIntValue(c.DlssNrPass3Style.value_for_config()).c_str());
    out.SetValue("DlssNr", "LocalStructure",
                 GetFloatValue(c.DlssNrLocalStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "LocalTone", GetFloatValue(c.DlssNrLocalTone.value_for_config()).c_str());
    out.SetValue("DlssNr", "SkinStructure",
                 GetFloatValue(c.DlssNrSkinStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "AutoMask", GetBoolValue(c.DlssNrAutoMask.value_for_config()).c_str());
    out.SetValue("DlssNr", "SkinProtection", GetBoolValue(c.DlssNrSkinProtection.value_for_config()).c_str());
    out.SetValue("DlssNr", "SkinToneEnabled", GetBoolValue(c.DlssNrSkinToneEnabled.value_for_config()).c_str());
    out.SetValue("DlssNr", "SkinDetail", GetFloatValue(c.DlssNrSkinDetail.value_for_config()).c_str());
    out.SetValue("DlssNr", "SkinColour", GetFloatValue(c.DlssNrSkinColour.value_for_config()).c_str());
    out.SetValue("DlssNr", "EnvironmentDetail", GetFloatValue(c.DlssNrEnvironmentDetail.value_for_config()).c_str());
    out.SetValue("DlssNr", "EnvironmentColour", GetFloatValue(c.DlssNrEnvironmentColour.value_for_config()).c_str());
    out.SetValue("DlssNr", "ShowSkinMask", GetBoolValue(c.DlssNrShowSkinMask.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2Intensity", GetFloatValue(c.DlssNrPass2Intensity.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2LocalStructure", GetFloatValue(c.DlssNrPass2LocalStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2LocalTone", GetFloatValue(c.DlssNrPass2LocalTone.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2SkinStructure", GetFloatValue(c.DlssNrPass2SkinStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass2AutoMask", GetBoolValue(c.DlssNrPass2AutoMask.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3Intensity", GetFloatValue(c.DlssNrPass3Intensity.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3LocalStructure", GetFloatValue(c.DlssNrPass3LocalStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3LocalTone", GetFloatValue(c.DlssNrPass3LocalTone.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3SkinStructure", GetFloatValue(c.DlssNrPass3SkinStructure.value_for_config()).c_str());
    out.SetValue("DlssNr", "Pass3AutoMask", GetBoolValue(c.DlssNrPass3AutoMask.value_for_config()).c_str());
    out.SetValue("DlssNr", "UnlockPasses", GetBoolValue(c.DlssNrUnlockPasses.value_for_config()).c_str());
    for (unsigned int i = 0; i < 27; ++i)
    {
        auto& pass = c.DlssNrExtraPasses[i];
        out.SetValue("DlssNr", std::format("Pass{}Style", i + 4).c_str(), GetIntValue(pass.style.value_for_config()).c_str());
        out.SetValue("DlssNr", std::format("Pass{}Intensity", i + 4).c_str(), GetFloatValue(pass.intensity.value_for_config()).c_str());
        out.SetValue("DlssNr", std::format("Pass{}LocalStructure", i + 4).c_str(), GetFloatValue(pass.structure.value_for_config()).c_str());
        out.SetValue("DlssNr", std::format("Pass{}LocalTone", i + 4).c_str(), GetFloatValue(pass.tone.value_for_config()).c_str());
        out.SetValue("DlssNr", std::format("Pass{}SkinStructure", i + 4).c_str(), GetFloatValue(pass.skin.value_for_config()).c_str());
        out.SetValue("DlssNr", std::format("Pass{}AutoMask", i + 4).c_str(), GetBoolValue(pass.autoMask.value_for_config()).c_str());
    }
    out.SetValue("DlssNr", "ReversibleMode", GetIntValue(c.DlssNrReversibleMode.value_for_config()).c_str());
    out.SetValue("DlssNr", "ColourEncoding", GetIntValue(c.DlssNrColourEncoding.value_for_config()).c_str());
    out.SetValue("DlssNr", "DisplayPaperWhiteNits",
                 GetFloatValue(c.DlssNrDisplayPaperWhiteNits.value_for_config()).c_str());
    out.SetValue("DlssNr", "LutFile", c.DlssNrLutFile.value_for_config_or("").c_str());
    out.SetValue("DlssNr", "LutStrength", GetFloatValue(c.DlssNrLutStrength.value_for_config()).c_str());
    out.SetValue("DlssNr", "ApplyModel", GetBoolValue(c.DlssNrApplyModel.value_for_config()).c_str());
    out.SetValue("DlssNr", "HoldFrame", GetBoolValue(c.DlssNrHoldFrame.value_for_config()).c_str());
}

// --- NR profiles (dlssnr/DlssNr_Profiles.h) ---

// The kept keys of one ini's [DlssNr] section
static DlssNrProfiles::Section KeptSection(const CSimpleIniA& from)
{
    DlssNrProfiles::Section section;
    CSimpleIniA::TNamesDepend keys;
    from.GetAllKeys("DlssNr", keys);

    for (const auto& key : keys)
    {
        if (DlssNrProfiles::IsKept(key.pItem))
            section[key.pItem] = from.GetValue("DlssNr", key.pItem, "auto");
    }

    return section;
}

DlssNrProfiles::Section Config::DlssNrSnapshot()
{
    CSimpleIniA current;
    WriteDlssNr(*this, current);
    return KeptSection(current);
}

// Taken in the snapshot's own form right after a load and a Save Settings: the ini's text can differ from what the
// writer gives for the same value (0.5 against 0.500000, a default written out), and would read as a change.
DlssNrProfiles::Section Config::SavedDlssNrSection() { return savedDlssNr; }

// A profile may come from someone else, so only a real one is read: not larger than any real profile, not a link, with
// a [Profile] section of a version this build knows, and at least one kept setting. Anything else would silently put
// every setting back to its default.
std::optional<DlssNrProfiles::File> Config::ReadDlssNrProfile(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > DlssNrProfiles::kMaxFileBytes || DlssNrProfiles::IsLink(path))
        return std::nullopt;

    CSimpleIniA file;
    if (file.LoadFile(path.c_str()) != SI_OK)
        return std::nullopt;

    const char* version = file.GetValue("Profile", "Version", nullptr);
    if (version == nullptr || std::atoi(version) < 1 || std::atoi(version) > DlssNrProfiles::kFileVersion)
        return std::nullopt;

    DlssNrProfiles::File profile { KeptSection(file), file.GetValue("Profile", "SavedFrom", "") };
    if (profile.settings.empty())
        return std::nullopt;
    return profile;
}

bool Config::SaveDlssNrProfile(const std::filesystem::path& path, std::string& error)
{
    CSimpleIniA out;
    out.SetValue(
        "Profile", "Version", std::to_string(DlssNrProfiles::kFileVersion).c_str(),
        "; OptiScaler NR profile: Neural Rendering settings for the game in SavedFrom. Load it from the menu,\n"
        "; Neural Rendering > Status & Presets > NR Profiles.");
    out.SetValue("Profile", "SavedFrom", State::Instance().gameExe.c_str());

    CSimpleIniA all;
    WriteDlssNr(*this, all);
    for (const auto& [key, value] : KeptSection(all))
    {
        // The LUT by file name only: a shared profile must not carry this PC's folders (see ProfileLut)
        const bool lut = DlssNrProfiles::EqualNoCase(key, "LutFile") && !value.empty();
        out.SetValue("DlssNr", key.c_str(),
                     lut ? std::filesystem::path(value).filename().string().c_str() : value.c_str());
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "Could not make the folder: " + ec.message();
        return false;
    }

    // A link (or junction) as the folder or the file would send the save somewhere else
    if (DlssNrProfiles::IsLink(path.parent_path()) || DlssNrProfiles::IsLink(path))
    {
        error = "NRProfiles or the file is a link; not saved.";
        return false;
    }

    if (out.SaveFile(path.c_str()) < 0)
    {
        error = "Could not write the file.";
        return false;
    }

    LOG_INFO("NR profile saved: {}", wstring_to_string(path.wstring()));
    return true;
}

// A profile's LUT is looked for by its file name in this game's OptiScaler\LUTs only, never at a folder the file names:
// a shared profile could otherwise point the game at someone else's folders or a network path. Not there: LUT off.
// The name is read wide: path::string() throws on a name the code page cannot hold.
static std::string ProfileLut(Config& c, std::string& lut)
{
    if (lut.empty() || DlssNrProfiles::EqualNoCase(lut, "auto"))
        return {};

    std::string shown = "(a name with other characters)";

    try
    {
        const std::filesystem::path name = std::filesystem::path(lut).filename();
        const auto ascii = DlssNrProfiles::AsciiName(name.wstring());
        if (ascii)
            shown = *ascii;

        const std::filesystem::path here =
            std::filesystem::path(c.MainDllPath.value_or(Util::DllPath().parent_path().wstring())) / L"LUTs" / name;
        std::error_code ec;

        if (ascii && !ascii->empty() && *ascii != "." && *ascii != ".." && here.parent_path().filename() == L"LUTs" &&
            std::filesystem::is_regular_file(here, ec) && !DlssNrProfiles::IsLink(here))
        {
            lut = here.string();
            return {};
        }
    }
    catch (const std::exception&)
    {
        // the LUTs folder's own path cannot be held as text here: LUT off, as for a missing file
    }

    lut.clear();
    return "LUT " + shown + " is not in OptiScaler\\LUTs: LUT off.";
}

bool Config::LoadDlssNrProfile(const std::filesystem::path& path, std::string& error, std::string& note)
{
    auto profile = ReadDlssNrProfile(path);
    if (!profile)
    {
        error = "Not an NR profile this build can read (or made by a newer build).";
        return false;
    }

    if (const auto lut = profile->settings.find("LutFile"); lut != profile->settings.end())
        note = ProfileLut(*this, lut->second);

    const size_t defaulted = ApplyDlssNrSection(profile->settings);
    LOG_INFO("NR profile loaded: {} (saved in {}; {} settings, {} at their default)", wstring_to_string(path.wstring()),
             profile->savedFrom.empty() ? "?" : profile->savedFrom, profile->settings.size(), defaulted);
    return true;
}

void Config::RevertDlssNrToSaved()
{
    const size_t defaulted = ApplyDlssNrSection(SavedDlssNrSection());
    LOG_INFO("NR settings back to the saved ini ({} at their default)", defaulted);
}

// Sets every kept setting from the section: cleared first, since set_from_config fills only an empty setting, so a
// key the section does not have ends at its default and a profile gives the same picture whatever was set before.
// Returns how many kept settings ended at their default.
size_t Config::ApplyDlssNrSection(const DlssNrProfiles::Section& section)
{
#define NR_PROFILE_FIELD(key, field) { key, [](Config& c) { c.field.reset(); } }
    static const std::pair<std::string_view, void (*)(Config&)> fields[] = {
        NR_PROFILE_FIELD("RunBeforeSR", DlssNrRunBeforeSr),
        NR_PROFILE_FIELD("DeferredDLSS", DlssNrDeferredDlss),
        NR_PROFILE_FIELD("FinishedPicture", DlssNrFinishedPicture),
        NR_PROFILE_FIELD("FinishedGameFgMode", DlssNrFinishedGameFgMode),
        NR_PROFILE_FIELD("Precision", DlssNrPrecision),
        NR_PROFILE_FIELD("Preset", DlssNrPreset),
        NR_PROFILE_FIELD("WorkingScale", DlssNrWorkingScale),
        NR_PROFILE_FIELD("ModelResolutionAuto", DlssNrModelResolutionAuto),
        NR_PROFILE_FIELD("ReducedUpscaleMethod", DlssNrReducedUpscaleMethod),
        NR_PROFILE_FIELD("Transfer", DlssNrTransfer),
        NR_PROFILE_FIELD("SuperiorScalingPreset", DlssNrSuperiorScalingPreset),
        NR_PROFILE_FIELD("ScalingDownscaler", DlssNrScalingDownscaler),
        NR_PROFILE_FIELD("VitEvery", DlssNrVitEvery),
        NR_PROFILE_FIELD("VitEveryPlain", DlssNrVitEveryPlain),
        NR_PROFILE_FIELD("DetailReuse", DlssNrDetailReuse),
        NR_PROFILE_FIELD("DetailReuseSteady", DlssNrDetailReuseSteady),
        NR_PROFILE_FIELD("DetailReuseFill", DlssNrDetailReuseFill),
        NR_PROFILE_FIELD("DetailReuseInterval", DlssNrDetailReuseInterval),
        NR_PROFILE_FIELD("DetailReuseSpread", DlssNrDetailReuseSpread),
        NR_PROFILE_FIELD("DetailReuseDepthTolerance", DlssNrDetailReuseDepthTolerance),
        NR_PROFILE_FIELD("DetailReuseClipGamma", DlssNrDetailReuseClipGamma),
        NR_PROFILE_FIELD("DetailReuseClipFalloff", DlssNrDetailReuseClipFalloff),
        NR_PROFILE_FIELD("DetailReuseSigmaFloor", DlssNrDetailReuseSigmaFloor),
        NR_PROFILE_FIELD("DetailReuseWithFG", DlssNrDetailReuseWithFg),
        NR_PROFILE_FIELD("DetailReuseMinFps", DlssNrDetailReuseMinFps),
        NR_PROFILE_FIELD("DetailReuseMaxDropped", DlssNrDetailReuseMaxDropped),
        NR_PROFILE_FIELD("SceneCut", DlssNrSceneCut),
        NR_PROFILE_FIELD("EditCache", DlssNrEditCache),
        NR_PROFILE_FIELD("EditCacheInterval", DlssNrEditCacheInterval),
        NR_PROFILE_FIELD("EditCacheCrossfade", DlssNrEditCacheCrossfade),
        NR_PROFILE_FIELD("TransferStrength", DlssNrTransferStrength),
        NR_PROFILE_FIELD("ColourStrength", DlssNrColourStrength),
        NR_PROFILE_FIELD("ReplaceDetailStrength", DlssNrReplaceDetailStrength),
        NR_PROFILE_FIELD("MaxRatio", DlssNrMaxRatio),
        NR_PROFILE_FIELD("ReversibleMode", DlssNrReversibleMode),
        NR_PROFILE_FIELD("Passes", DlssNrPasses),
        NR_PROFILE_FIELD("PassFeedback", DlssNrPassFeedback),
        NR_PROFILE_FIELD("UnlockPasses", DlssNrUnlockPasses),
        NR_PROFILE_FIELD("Style", DlssNrStyle),
        NR_PROFILE_FIELD("Intensity", DlssNrIntensity),
        NR_PROFILE_FIELD("LocalStructure", DlssNrLocalStructure),
        NR_PROFILE_FIELD("LocalTone", DlssNrLocalTone),
        NR_PROFILE_FIELD("SkinStructure", DlssNrSkinStructure),
        NR_PROFILE_FIELD("AutoMask", DlssNrAutoMask),
        NR_PROFILE_FIELD("Pass2Preset", DlssNrPass2Preset),
        NR_PROFILE_FIELD("Pass2Style", DlssNrPass2Style),
        NR_PROFILE_FIELD("Pass2Intensity", DlssNrPass2Intensity),
        NR_PROFILE_FIELD("Pass2LocalStructure", DlssNrPass2LocalStructure),
        NR_PROFILE_FIELD("Pass2LocalTone", DlssNrPass2LocalTone),
        NR_PROFILE_FIELD("Pass2SkinStructure", DlssNrPass2SkinStructure),
        NR_PROFILE_FIELD("Pass2AutoMask", DlssNrPass2AutoMask),
        NR_PROFILE_FIELD("Pass3Preset", DlssNrPass3Preset),
        NR_PROFILE_FIELD("Pass3Style", DlssNrPass3Style),
        NR_PROFILE_FIELD("Pass3Intensity", DlssNrPass3Intensity),
        NR_PROFILE_FIELD("Pass3LocalStructure", DlssNrPass3LocalStructure),
        NR_PROFILE_FIELD("Pass3LocalTone", DlssNrPass3LocalTone),
        NR_PROFILE_FIELD("Pass3SkinStructure", DlssNrPass3SkinStructure),
        NR_PROFILE_FIELD("Pass3AutoMask", DlssNrPass3AutoMask),
        NR_PROFILE_FIELD("SkinProtection", DlssNrSkinProtection),
        NR_PROFILE_FIELD("SkinToneEnabled", DlssNrSkinToneEnabled),
        NR_PROFILE_FIELD("SkinDetail", DlssNrSkinDetail),
        NR_PROFILE_FIELD("SkinColour", DlssNrSkinColour),
        NR_PROFILE_FIELD("EnvironmentDetail", DlssNrEnvironmentDetail),
        NR_PROFILE_FIELD("EnvironmentColour", DlssNrEnvironmentColour),
        NR_PROFILE_FIELD("InputFlatten", DlssNrInputFlatten),
        NR_PROFILE_FIELD("InputFlattenStrength", DlssNrInputFlattenStrength),
        NR_PROFILE_FIELD("InputStructureHint", DlssNrInputStructureHint),
        NR_PROFILE_FIELD("InputStructureStrength", DlssNrInputStructureStrength),
        NR_PROFILE_FIELD("InputStyleFacesOnly", DlssNrInputStyleFacesOnly),
        NR_PROFILE_FIELD("LutFile", DlssNrLutFile),
        NR_PROFILE_FIELD("LutStrength", DlssNrLutStrength),
        NR_PROFILE_FIELD("WhitePointSource", DlssNrWhitePointSource),
        NR_PROFILE_FIELD("GameExposureScale", DlssNrGameExposureScale),
        NR_PROFILE_FIELD("WhitePointTrim", DlssNrWhitePointTrim),
        NR_PROFILE_FIELD("WhitePointScale", DlssNrWhitePointScale),
        NR_PROFILE_FIELD("AutoExposureTrim", DlssNrAutoExposureTrim),
        NR_PROFILE_FIELD("AutoExposureShadowProtection", DlssNrAutoExposureShadowProtection),
        NR_PROFILE_FIELD("AutoExposureMeter", DlssNrAutoExposureMeter),
        NR_PROFILE_FIELD("AutoExposureMeterLowPercent", DlssNrAutoExposureMeterLowPercent),
        NR_PROFILE_FIELD("AutoExposureMeterHighPercent", DlssNrAutoExposureMeterHighPercent),
        NR_PROFILE_FIELD("AutoExposureAdaptBrighterSeconds", DlssNrAutoExposureAdaptBrighterSeconds),
        NR_PROFILE_FIELD("AutoExposureAdaptDarkerSeconds", DlssNrAutoExposureAdaptDarkerSeconds),
        NR_PROFILE_FIELD("AutoExposureFollowGame", DlssNrAutoExposureFollowGame),
        NR_PROFILE_FIELD("GameExposureTrimAnchors", DlssNrGameExposureTrimAnchors),
        NR_PROFILE_FIELD("AutoExposureTrimAnchors", DlssNrAutoExposureTrimAnchors),
        NR_PROFILE_FIELD("ScanTrim", DlssNrScanTrim),
        NR_PROFILE_FIELD("ScanAnchorValue", DlssNrScanAnchorValue),
        NR_PROFILE_FIELD("ScanAnchorWhitePoint", DlssNrScanAnchorWhitePoint),
        NR_PROFILE_FIELD("ScanAnchors", DlssNrScanAnchors),
        NR_PROFILE_FIELD("ScanInverted", DlssNrScanInverted),
        NR_PROFILE_FIELD("ScanMeter", DlssNrScanMeter),
        NR_PROFILE_FIELD("ColourEncoding", DlssNrColourEncoding),
        NR_PROFILE_FIELD("NativeDepthFinder", DlssNrNativeDepthFinder),
        NR_PROFILE_FIELD("NativeDepthVkCopyUsage", DlssNrNativeDepthVkCopyUsage),
        NR_PROFILE_FIELD("NativeDepthResolveInList", DlssNrNativeDepthResolveInList),
        NR_PROFILE_FIELD("NativeDxvkVulkan", DlssNrNativeDxvkVulkan),
        NR_PROFILE_FIELD("NativeDepthWarmupFrames", DlssNrNativeDepthWarmupFrames),
        NR_PROFILE_FIELD("NativeMotionProfile", DlssNrNativeMotionProfile),
        NR_PROFILE_FIELD("NativeMotionProfileCustom", DlssNrNativeMotionProfileCustom),
        NR_PROFILE_FIELD("NativeMotion", DlssNrNativeMotion),
        NR_PROFILE_FIELD("NativeInput", DlssNrNativeInput),
        NR_PROFILE_FIELD("NativeUpscaler", DlssNrNativeUpscaler),
        NR_PROFILE_FIELD("NativeUpscalerPass", DlssNrNativeUpscalerPass),
        NR_PROFILE_FIELD("NativeFrameGenerationOnly", DlssNrNativeFrameGenerationOnly),
        NR_PROFILE_FIELD("NativeLowLatencyBoost", DlssNrNativeLowLatencyBoost),
        NR_PROFILE_FIELD("NativeLowLatencyDxvkGameThread", DlssNrNativeLowLatencyDxvkGameThread),
        NR_PROFILE_FIELD("SpatialCompression", DlssNrSpatialCompression),
        NR_PROFILE_FIELD("SpatialCenterX", DlssNrSpatialCenterX),
        NR_PROFILE_FIELD("SpatialCenterY", DlssNrSpatialCenterY),
        NR_PROFILE_FIELD("SpatialWorkX", DlssNrSpatialWorkX),
        NR_PROFILE_FIELD("SpatialWorkY", DlssNrSpatialWorkY),
        NR_PROFILE_FIELD("SpatialOffsetX", DlssNrSpatialOffsetX),
        NR_PROFILE_FIELD("SpatialOffsetY", DlssNrSpatialOffsetY),
        NR_PROFILE_FIELD("SpatialShiftX", DlssNrSpatialShiftX),
        NR_PROFILE_FIELD("SpatialShiftY", DlssNrSpatialShiftY),
    };
#undef NR_PROFILE_FIELD

    // Only a setting whose value changes is cleared and read again: the render thread reads these while the menu
    // applies them, and a setting left alone is never seen empty (or as a string being replaced) for a moment.
    const DlssNrProfiles::Section current = DlssNrSnapshot();
    const auto valueIn = [](const DlssNrProfiles::Section& s, const std::string& key) -> std::string
    {
        const auto it = s.find(key);
        return it == s.end() ? std::string("auto") : it->second;
    };
    const auto changes = [&](const std::string& key)
    { return !DlssNrProfiles::EqualNoCase(valueIn(current, key), valueIn(section, key)); };

    CSimpleIniA source;
    size_t defaulted = 0;

    for (const auto& [key, clear] : fields)
    {
        const std::string name(key);
        const std::string value = valueIn(section, name);

        // Every value goes to the reader, changed or not (some reads reset a setting whose key is missing, e.g.
        // ScalingDownscaler); one that did not change still has its value, so set_from_config leaves it as it is
        if (DlssNrProfiles::EqualNoCase(value, "auto"))
            ++defaulted;
        else
            source.SetValue("DlssNr", name.c_str(), value.c_str());

        if (changes(name))
            clear(*this);
    }

    // Pass 4..30: a pass whose keys change is cleared and read again as a whole
    for (unsigned int i = 0; i < 27; ++i)
    {
        static constexpr const char* suffixes[] = { "Style",     "Intensity",     "LocalStructure",
                                                    "LocalTone", "SkinStructure", "AutoMask" };
        bool passChanges = false;
        for (const char* suffix : suffixes)
        {
            const std::string name = std::format("Pass{}{}", i + 4, suffix);
            passChanges = passChanges || changes(name);
            if (const std::string value = valueIn(section, name); !DlssNrProfiles::EqualNoCase(value, "auto"))
                source.SetValue("DlssNr", name.c_str(), value.c_str());
        }

        if (!passChanges)
            continue;

        auto& pass = DlssNrExtraPasses[i];
        pass.style.reset();
        pass.intensity.reset();
        pass.structure.reset();
        pass.tone.reset();
        pass.skin.reset();
        pass.autoMask.reset();
    }

    // readSource goes back to the ini however ReadDlssNr leaves (it is one global: profiles are applied, like the ini
    // is loaded, on one thread at a time -- the menu's)
    struct ReadFrom
    {
        explicit ReadFrom(CSimpleIniA& source) { readSource = &source; }
        ~ReadFrom() { readSource = &ini; }
    } readFrom(source);

    ReadDlssNr();
    return defaulted;
}

bool Config::SaveIni()
{
    // Upscalers
    {
        auto SaveUpscaler = [&](const char* key, auto& upscalerSetting)
        {
            std::string value = upscalerSetting.value_for_config()
                                    .transform(UpscalerToCode) // Turn enum into string
                                    .value_or("auto");

            ini.SetValue("Upscalers", key, value.c_str());
        };

        SaveUpscaler("Dx11Upscaler", Instance()->Dx11Upscaler);
        SaveUpscaler("Dx12Upscaler", Instance()->Dx12Upscaler);
        SaveUpscaler("VulkanUpscaler", Instance()->VulkanUpscaler);
    }

    // Frame Generation
    {
        ini.SetValue("FrameGen", "Enabled", GetBoolValue(Instance()->FGEnabled.value_for_config()).c_str());
        ini.SetValue("FrameGen", "External", GetBoolValue(Instance()->ExternalFrameGeneration.value_for_config_or(false)).c_str());
        ini.SetValue("MFGUnlock", "XeFG", MfgUnlock::ModeText(Instance()->MfgUnlockXeFG.value_for_config()));
        ini.SetValue("MFGUnlock", "RTX40", MfgUnlock::ModeText(Instance()->MfgUnlockRTX40.value_for_config()));
        ini.SetValue("MFGUnlock", "RTX2030", MfgUnlock::ModeText(Instance()->MfgUnlockRTX2030.value_for_config()));

        // The old [DLSSG] unlock keys were replaced by [MFGUnlock]; drop them so the startup note stops after a save
        for (const char* key : { "AdaMfgUnlock", "AdaBlackwellKernels", "AdaTemporalFix", "AdaFlipMeteringPatch",
                                 "AmpereMfgUnlock", "AmpereMfgMaxFrames", "AmpereMfgKernelImage",
                                 "AmpereMfgHardwareBilinear" })
            ini.Delete("DLSSG", key);
        ini.SetValue("FrameGen", "DebugView", GetBoolValue(Instance()->FGDebugView.value_for_config()).c_str());
        std::string FGInputString = "auto";
        if (auto FGInputHeld = Instance()->FGInput.value_for_config(); FGInputHeld.has_value())
        {
            if (FGInputHeld.value() == FGInput::NoFG)
                FGInputString = "NoFG";
            else if (FGInputHeld.value() == FGInput::Upscaler)
                FGInputString = "Upscaler";
            else if (FGInputHeld.value() == FGInput::NvngxFG)
                FGInputString = "NvngxFG";
            else if (FGInputHeld.value() == FGInput::DLSSG)
                FGInputString = "DLSSG";
            else if (FGInputHeld.value() == FGInput::FSRFG)
                FGInputString = "FSRFG";
            else if (FGInputHeld.value() == FGInput::FSRFG30)
                FGInputString = "FSRFG30";
        }
        ini.SetValue("FrameGen", "FGInput", FGInputString.c_str());

        std::string FGOutputString = "auto";
        if (auto FGOutputHeld = Instance()->FGOutput.value_for_config(); FGOutputHeld.has_value())
        {
            if (FGOutputHeld.value() == FGOutput::NoFG)
                FGOutputString = "NoFG";
            else if (FGOutputHeld.value() == FGOutput::FSRFG)
                FGOutputString = "FSRFG";
            else if (FGOutputHeld.value() == FGOutput::XeFG)
                FGOutputString = "XeFG";
            else if (FGOutputHeld.value() == FGOutput::DLSSG)
                FGOutputString = "DLSSG";
        }
        ini.SetValue("FrameGen", "FGOutput", FGOutputString.c_str());

        ini.SetValue("FrameGen", "StreamlineIgnoreOTA",
                     GetBoolValue(Instance()->FGStreamlineIgnoreOTA.value_for_config()).c_str());

        std::string FGNvngxReplacementString = "auto";
        if (auto FGNvngxReplacementHeld = Instance()->FGNvngxReplacement.value_for_config();
            FGNvngxReplacementHeld.has_value())
        {
            if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::None)
                FGNvngxReplacementString = "None";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Nukems)
                FGNvngxReplacementString = "Nukems";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Arturs)
                FGNvngxReplacementString = "Arturs";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::FFX)
                FGNvngxReplacementString = "FFX";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Combo)
                FGNvngxReplacementString = "Combo";
        }
        ini.SetValue("FrameGen", "FGNvngxReplacement", FGNvngxReplacementString.c_str());

        std::optional<int> ftInput;
        if (Instance()->FTInput.has_value())
            ftInput = (int) Instance()->FTInput.value();

        ini.SetValue("FrameGen", "FTInput", GetIntValue(ftInput).c_str());
        ini.SetValue("FrameGen", "DrawUIOverFG", GetBoolValue(Instance()->FGDrawUIOverFG.value_for_config()).c_str());
        ini.SetValue("FrameGen", "UIPremultipliedAlpha",
                     GetBoolValue(Instance()->FGUIPremultipliedAlpha.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DisableHudless",
                     GetBoolValue(Instance()->FGDisableHudless.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DisableUI", GetBoolValue(Instance()->FGDisableUI.value_for_config()).c_str());
        ini.SetValue("FrameGen", "SkipReset", GetBoolValue(Instance()->FGSkipReset.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectLeft", GetIntValue(Instance()->FGRectLeft.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectTop", GetIntValue(Instance()->FGRectTop.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectWidth", GetIntValue(Instance()->FGRectWidth.value_for_config()).c_str());
        ini.SetValue("FrameGen", "RectHeight", GetIntValue(Instance()->FGRectHeight.value_for_config()).c_str());
        ini.SetValue("FrameGen", "AllowedFrameAhead",
                     GetIntValue(Instance()->FGAllowedFrameAhead.value_for_config()).c_str());
        ini.SetValue("FrameGen", "DepthValidNow", GetBoolValue(Instance()->FGDepthValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "VelocityValidNow",
                     GetBoolValue(Instance()->FGVelocityValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "HudlessValidNow",
                     GetBoolValue(Instance()->FGHudlessValidNow.value_for_config()).c_str());
        ini.SetValue("FrameGen", "OnlyAcceptFirstHudless",
                     GetBoolValue(Instance()->FGOnlyAcceptFirstHudless.value_for_config()).c_str());
        ini.SetValue("FrameGen", "PreserveSwapChain",
                     GetBoolValue(Instance()->FGPreserveSwapChain.value_for_config()).c_str());
        ini.SetValue("FrameGen", "SkipResizeBuffers",
                     GetBoolValue(Instance()->FGSkipResizeBuffers.value_for_config()).c_str());
        ini.SetValue("FrameGen", "ModifyBufferState",
                     GetBoolValue(Instance()->FGModifyBufferState.value_for_config()).c_str());
        ini.SetValue("FrameGen", "ModifySCIndex", GetBoolValue(Instance()->FGModifySCIndex.value_for_config()).c_str());
        ini.SetValue("FrameGen", "HudCutoff", GetFloatValue(Instance()->FGHudCutoff.value_for_config()).c_str());
    }

    // FSR FG output
    {
        ini.SetValue("FSRFG", "DebugTearLines", GetBoolValue(Instance()->FGDebugTearLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DebugResetLines",
                     GetBoolValue(Instance()->FGDebugResetLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DebugPacingLines",
                     GetBoolValue(Instance()->FGDebugPacingLines.value_for_config()).c_str());
        ini.SetValue("FSRFG", "AllowAsync", GetBoolValue(Instance()->FGAsync.value_for_config()).c_str());
        ini.SetValue("FSRFG", "UseMutexForSwapchain",
                     GetBoolValue(Instance()->FGUseMutexForSwapchain.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FramePacingTuning",
                     GetBoolValue(Instance()->FGFramePacingTuning.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTSafetyMarginInMs",
                     GetFloatValue(Instance()->FGFPTSafetyMarginInMs.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTVarianceFactor",
                     GetFloatValue(Instance()->FGFPTVarianceFactor.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTHybridSpin",
                     GetBoolValue(Instance()->FGFPTAllowHybridSpin.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTHybridSpinTime",
                     GetIntValue(Instance()->FGFPTHybridSpinTime.value_for_config()).c_str());
        ini.SetValue("FSRFG", "FPTWaitForSingleObjectOnFence",
                     GetBoolValue(Instance()->FGFPTAllowWaitForSingleObjectOnFence.value_for_config()).c_str());
        ini.SetValue("FSRFG", "EnableWatermark",
                     GetBoolValue(Instance()->FSRFGEnableWatermark.value_for_config()).c_str());
        ini.SetValue("FSRFG", "DisableFlipMetering",
                     GetBoolValue(Instance()->FSRFGDisableFlipMetering.value_for_config()).c_str());
    }

    // XeFG output
    {
        ini.SetValue("XeFG", "MaxInterpolations",
                     GetIntValue(Instance()->FGXeFGMaxInterpolations.value_for_config()).c_str());
        ini.SetValue("XeFG", "InterpolationCount",
                     GetIntValue(Instance()->FGXeFGInterpolationCount.value_for_config()).c_str());
        ini.SetValue("XeFG", "DynamicMFG", GetBoolValue(Instance()->FGXeFGDynamicMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "FramerateTargetDMFG",
                     GetFloatValue(Instance()->FGXeFGFramerateTargetDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "RoundingDMFG", GetIntValue(Instance()->FGXeFGRoundingDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "DynamicAllow1X",
                     GetBoolValue(Instance()->FGXeFGDynamicAllow1X.value_for_config()).c_str());
        ini.SetValue("XeFG", "TargetAsCapDMFG",
                     GetBoolValue(Instance()->FGXeFGTargetAsCapDMFG.value_for_config()).c_str());
        ini.SetValue("XeFG", "IgnoreInitChecks",
                     GetBoolValue(Instance()->FGXeFGIgnoreInitChecks.value_for_config()).c_str());
        ini.SetValue("XeFG", "UIComposition", GetBoolValue(Instance()->FGXeFGUIComposition.value_for_config()).c_str());
        ini.SetValue("XeFG", "DepthInverted", GetBoolValue(Instance()->FGXeFGDepthInverted.value_for_config()).c_str());
        ini.SetValue("XeFG", "JitteredMV", GetBoolValue(Instance()->FGXeFGJitteredMV.value_for_config()).c_str());
        ini.SetValue("XeFG", "HighResMV", GetBoolValue(Instance()->FGXeFGHighResMV.value_for_config()).c_str());
        ini.SetValue("XeFG", "DebugView", GetBoolValue(Instance()->FGXeFGDebugView.value_for_config()).c_str());
        ini.SetValue("XeFG", "ForceBorderless",
                     GetBoolValue(Instance()->FGXeFGForceBorderless.value_for_config()).c_str());

        // Nothing held (never chosen): auto, which reads as xell, so the key is in the file for the player to find
        if (const auto held = Instance()->FGXeFGLowLatencyOutput.value_for_config(); held.has_value())
            ini.SetValue("XeFG", "LowLatencyOutput", native::lowlatency::XefgLatencyIniText(held.value()));
        else
            ini.SetValue("XeFG", "LowLatencyOutput", "auto");
    }

    {
        ini.SetValue("DLSSG", "InterpolationCount",
                     GetIntValue(Instance()->FGDLSSGInterpolationCount.value_for_config()).c_str());
        ini.SetValue("DLSSG", "UseGamesReflexMarkers",
                     GetBoolValue(Instance()->FGDLSSGUseGamesReflexMarkers.value_for_config()).c_str());
        ini.SetValue("DLSSG", "OverrideInterpolationCount",
                     GetIntValue(Instance()->FGDLSSGOverrideInterpolationCount.value_for_config()).c_str());
        ini.SetValue("DLSSG", "FramerateTargetDMFG",
                     GetFloatValue(Instance()->FGDLSSGFramerateTargetDMFG.value_for_config()).c_str());
        ini.SetValue("DLSSG", "OverrideForceDMFG",
                     GetBoolValue(Instance()->FGDLSSGOverrideForceDMFG.value_for_config()).c_str());
        ini.SetValue("DLSSG", "ForceDMFG", GetBoolValue(Instance()->FGDLSSGForceDMFG.value_for_config()).c_str());
    }

    // OptiFG
    {
        ini.SetValue("OptiFG", "DisableHUDFix", GetBoolValue(Instance()->FGDisableHUDFix.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFix", GetBoolValue(Instance()->FGHUDFix.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDLimit", GetIntValue(Instance()->FGHUDLimit.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixExtended", GetBoolValue(Instance()->FGHUDFixExtended.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixUITexture",
                     GetBoolValue(Instance()->FGHUDFixUITexture.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixUIUnblend",
                     GetBoolValue(Instance()->FGHUDFixUIUnblend.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixImmediate",
                     GetBoolValue(Instance()->FGImmediateCapture.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixPersistentBindings",
                     GetBoolValue(Instance()->FGHudfixPersistentBindings.value_for_config()).c_str());
        ini.SetValue("OptiFG", "UseShards", GetBoolValue(Instance()->FGUseShards.value_for_config()).c_str());
        ini.SetValue("OptiFG", "AlwaysTrackHeaps",
                     GetBoolValue(Instance()->FGAlwaysTrackHeaps.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceBlocking",
                     GetBoolValue(Instance()->FGResourceBlocking.value_for_config()).c_str());
        ini.SetValue("OptiFG", "MakeDepthCopy", GetBoolValue(Instance()->FGMakeDepthCopy.value_for_config()).c_str());
        ini.SetValue("OptiFG", "MakeMVCopy", GetBoolValue(Instance()->FGMakeMVCopy.value_for_config()).c_str());

        ini.SetValue("OptiFG", "HudfixDisableRTV",
                     GetBoolValue(Instance()->FGHudfixDisableRTV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSRV",
                     GetBoolValue(Instance()->FGHudfixDisableSRV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableUAV",
                     GetBoolValue(Instance()->FGHudfixDisableUAV.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableOM",
                     GetBoolValue(Instance()->FGHudfixDisableOM.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDispatch",
                     GetBoolValue(Instance()->FGHudfixDisableDispatch.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDI",
                     GetBoolValue(Instance()->FGHudfixDisableDI.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableDII",
                     GetBoolValue(Instance()->FGHudfixDisableDII.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSCR",
                     GetBoolValue(Instance()->FGHudfixDisableSCR.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HudfixDisableSGR",
                     GetBoolValue(Instance()->FGHudfixDisableSGR.value_for_config()).c_str());

        ini.SetValue("OptiFG", "EnableDepthScale",
                     GetBoolValue(Instance()->FGEnableDepthScale.value_for_config()).c_str());
        ini.SetValue("OptiFG", "DepthScaleMax", GetFloatValue(Instance()->FGDepthScaleMax.value_for_config()).c_str());

        ini.SetValue("OptiFG", "HUDFixDontUseSwapchainBuffers",
                     GetBoolValue(Instance()->FGDontUseSwapchainBuffers.value_for_config()).c_str());
        ini.SetValue("OptiFG", "HUDFixRelaxedResolutionCheck",
                     GetBoolValue(Instance()->FGRelaxedResolutionCheck.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceFlip", GetBoolValue(Instance()->FGResourceFlip.value_for_config()).c_str());
        ini.SetValue("OptiFG", "ResourceFlipOffset",
                     GetBoolValue(Instance()->FGResourceFlipOffset.value_for_config()).c_str());

        ini.SetValue("OptiFG", "AlwaysCaptureFSRFGSwapchain",
                     GetBoolValue(Instance()->FGAlwaysCaptureFSRFGSwapchain.value_for_config()).c_str());
    }

    // FSR FG Inputs
    {
        ini.SetValue("FSRFGInputs", "SkipConfigForHudless",
                     GetBoolValue(Instance()->FSRFGSkipConfigForHudless.value_for_config()).c_str());
        ini.SetValue("FSRFGInputs", "SkipDispatchForHudless",
                     GetBoolValue(Instance()->FSRFGSkipDispatchForHudless.value_for_config()).c_str());
    }

    // Framerate
    {
        ini.SetValue("Framerate", "FramerateLimit",
                     GetFloatValue(Instance()->FramerateLimit.value_for_config()).c_str());

        // The new key, and the old [DlssNr] one beside it for a build from before the Input choice
        auto input = native::lowlatency::Input::Auto;

        if (auto held = Instance()->FramerateLowLatencyInput.value_for_config(); held.has_value())
            input = held.value();

        ini.SetValue("Framerate", "LowLatencyInput", native::lowlatency::InputIniText(input));
        ini.SetValue("DlssNr", "NativeLowLatency", native::lowlatency::LegacySettingText(input));
        ini.SetValue("Framerate", "FrameCapOnly",
                     GetBoolValue(Instance()->FramerateFrameCapOnly.value_for_config()).c_str());
    }

    // Output Scaling
    {
        ini.SetValue("OutputScaling", "Enabled",
                     GetBoolValue(Instance()->OutputScalingEnabled.value_for_config()).c_str());
        ini.SetValue("OutputScaling", "Multiplier",
                     GetFloatValue(Instance()->OutputScalingMultiplier.value_for_config()).c_str());
        ini.SetValue("OutputScaling", "Downscaler", GetIntValue(Instance()->OutputScalingDownscaler).c_str());
    }

    // FSR common
    {
        ini.SetValue("FSR", "VerticalFov", GetFloatValue(Instance()->FsrVerticalFov.value_for_config()).c_str());
        ini.SetValue("FSR", "HorizontalFov", GetFloatValue(Instance()->FsrHorizontalFov.value_for_config()).c_str());
        ini.SetValue("FSR", "CameraNear", GetFloatValue(Instance()->FsrCameraNear.value_for_config()).c_str());
        ini.SetValue("FSR", "CameraFar", GetFloatValue(Instance()->FsrCameraFar.value_for_config()).c_str());
        ini.SetValue("FSR", "UseFsrInputValues",
                     GetBoolValue(Instance()->FsrUseFsrInputValues.value_for_config()).c_str());
    }

    // FSR
    {
        ini.SetValue("FSR", "VelocityFactor", GetFloatValue(Instance()->FsrVelocity.value_for_config()).c_str());
        ini.SetValue("FSR", "ReactiveScale", GetFloatValue(Instance()->FsrReactiveScale.value_for_config()).c_str());
        ini.SetValue("FSR", "ShadingScale", GetFloatValue(Instance()->FsrShadingScale.value_for_config()).c_str());
        ini.SetValue("FSR", "AccAddPerFrame", GetFloatValue(Instance()->FsrAccAddPerFrame.value_for_config()).c_str());
        ini.SetValue("FSR", "MinDisOccAcc", GetFloatValue(Instance()->FsrMinDisOccAcc.value_for_config()).c_str());
        ini.SetValue("FSR", "DebugView", GetBoolValue(Instance()->FsrDebugView.value_for_config()).c_str());
        ini.SetValue("FSR", "UpscalerIndex", GetIntValue(Instance()->FfxUpscalerIndex.value_for_config()).c_str());
        ini.SetValue("FSR", "FGIndex", GetIntValue(Instance()->FfxFGIndex.value_for_config()).c_str());
        ini.SetValue("FSR", "UseReactiveMaskForTransparency",
                     GetBoolValue(Instance()->FsrUseMaskForTransparency.value_for_config()).c_str());
        ini.SetValue("FSR", "DlssReactiveMaskBias",
                     GetFloatValue(Instance()->DlssReactiveMaskBias.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4GpuSupport", GetIntValue(Instance()->Fsr4GpuSupport.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4Preset", GetIntValue(Instance()->Fsr4Preset.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4Model", GetIntValue(Instance()->Fsr4Model.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4EnableWatermark",
                     GetBoolValue(Instance()->Fsr4EnableWatermark.value_for_config()).c_str());
        ini.SetValue("FSR", "Fsr4DoNotLoadAmdxc64",
                     GetBoolValue(Instance()->Fsr4DoNotLoadAmdxc64.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearColorSpace",
                     GetBoolValue(Instance()->FsrNonLinearColorSpace.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearPQ", GetBoolValue(Instance()->FsrNonLinearPQ.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrNonLinearSRGB", GetBoolValue(Instance()->FsrNonLinearSRGB.value_for_config()).c_str());
        ini.SetValue("FSR", "FsrAgilitySDKUpgrade",
                     GetBoolValue(Instance()->FsrAgilitySDKUpgrade.value_for_config()).c_str());
    }

    // XeSS
    {
        ini.SetValue("XeSS", "BuildPipelines", GetBoolValue(Instance()->BuildPipelines.value_for_config()).c_str());
        ini.SetValue("XeSS", "CreateHeaps", GetBoolValue(Instance()->CreateHeaps.value_for_config()).c_str());
        ini.SetValue("XeSS", "NetworkModel", GetIntValue(Instance()->NetworkModel.value_for_config()).c_str());
    }

    // DLSS
    {
        ini.SetValue("DLSS", "Enabled", GetBoolValue(Instance()->DLSSEnabled.value_for_config()).c_str());

        WriteDlssNr(*this, ini);
        ini.SetValue("DLSS", "RenderPresetOverride",
                     GetBoolValue(Instance()->RenderPresetOverride.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetForAll",
                     GetIntValue(Instance()->RenderPresetForAll.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetDLAA", GetIntValue(Instance()->RenderPresetDLAA.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetUltraQuality",
                     GetIntValue(Instance()->RenderPresetUltraQuality.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetQuality",
                     GetIntValue(Instance()->RenderPresetQuality.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetBalanced",
                     GetIntValue(Instance()->RenderPresetBalanced.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetPerformance",
                     GetIntValue(Instance()->RenderPresetPerformance.value_for_config()).c_str());
        ini.SetValue("DLSS", "RenderPresetUltraPerformance",
                     GetIntValue(Instance()->RenderPresetUltraPerformance.value_for_config()).c_str());
        ini.SetValue("DLSS", "UseGenericAppIdWithDlss",
                     GetBoolValue(Instance()->UseGenericAppIdWithDlss.value_for_config()).c_str());
    }

    // DLSSD
    {
        ini.SetValue("DLSSD", "RenderPresetOverride",
                     GetBoolValue(Instance()->DLSSDRenderPresetOverride.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetForAll",
                     GetIntValue(Instance()->DLSSDRenderPresetForAll.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetDLAA",
                     GetIntValue(Instance()->DLSSDRenderPresetDLAA.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetUltraQuality",
                     GetIntValue(Instance()->DLSSDRenderPresetUltraQuality.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetQuality",
                     GetIntValue(Instance()->DLSSDRenderPresetQuality.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetBalanced",
                     GetIntValue(Instance()->DLSSDRenderPresetBalanced.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetPerformance",
                     GetIntValue(Instance()->DLSSDRenderPresetPerformance.value_for_config()).c_str());
        ini.SetValue("DLSSD", "RenderPresetUltraPerformance",
                     GetIntValue(Instance()->DLSSDRenderPresetUltraPerformance.value_for_config()).c_str());
    }

    // NvngxFG
    {
        ini.SetValue("NvngxFG", "MakeDepthCopy",
                     GetBoolValue(Instance()->NvngxFGMakeDepthCopy.value_for_config()).c_str());
        ini.SetValue("NvngxFG", "DispatchFlags",
                     GetIntValue(Instance()->NvngxFGDispatchFlags.value_for_config(), true).c_str());
        ini.SetValue("NvngxFG", "ShowDebug", GetBoolValue(Instance()->NvngxFGShowDebug.value_for_config()).c_str());
        ini.SetValue("NvngxFG", "DisableHudless",
                     GetBoolValue(Instance()->NvngxFGDisableHudless.value_for_config()).c_str());
    }

    // Sharpness
    {
        std::string shader = SharpnessShader.value_for_config()
                                 .transform(SharpnessShaderToCode) // Turn enum into string
                                 .value_or("auto");

        ini.SetValue("Sharpness", "Shader", shader.c_str());

        ini.SetValue("Sharpness", "OverrideSharpness",
                     GetBoolValue(Instance()->OverrideSharpness.value_for_config()).c_str());
        ini.SetValue("Sharpness", "Sharpness", GetFloatValue(Instance()->Sharpness.value_for_config()).c_str());
    }

    // CAS
    {
        ini.SetValue("CAS", "Enabled", GetBoolValue(Instance()->RcasEnabled.value_for_config()).c_str());

        ini.SetValue("CAS", "MotionSharpnessEnabled",
                     GetBoolValue(Instance()->MotionSharpnessEnabled.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionSharpness", GetFloatValue(Instance()->MotionSharpness.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionThreshold", GetFloatValue(Instance()->MotionThreshold.value_for_config()).c_str());
        ini.SetValue("CAS", "MotionScaleLimit", GetFloatValue(Instance()->MotionScaleLimit.value_for_config()).c_str());

        ini.SetValue("CAS", "ContrastEnabled", GetBoolValue(Instance()->ContrastEnabled.value_for_config()).c_str());
        ini.SetValue("CAS", "Contrast", GetFloatValue(Instance()->Contrast.value_for_config()).c_str());

        ini.SetValue("CAS", "DADepthScale", GetFloatValue(Instance()->DADepthScale.value_for_config()).c_str());
        ini.SetValue("CAS", "DADepthBias", GetFloatValue(Instance()->DADepthBias.value_for_config()).c_str());
        ini.SetValue("CAS", "DAClampOutput", GetBoolValue(Instance()->DAClampOutput.value_for_config()).c_str());

        ini.SetValue("CAS", "SharpenerDebug",
                     GetBoolValue(Instance()->MotionSharpnessDebug.value_for_config()).c_str());
    }

    // Magnifier
    {
        ini.SetValue("Magnifier", "Enabled", GetBoolValue(Instance()->MagnifierEnabled.value_for_config()).c_str());

        ini.SetValue("Magnifier", "Size", GetFloatValue(Instance()->MagnifierSize.value_for_config()).c_str());
        ini.SetValue("Magnifier", "ZoomFactor",
                     GetIntValue(Instance()->MagnifierZoomFactor.value_for_config()).c_str());

        ini.SetValue("Magnifier", "BorderSize",
                     GetFloatValue(Instance()->MagnifierBorderSize.value_for_config()).c_str());
        ini.SetValue("Magnifier", "CursorOffsetX",
                     GetFloatValue(Instance()->MagnifierCursorOffsetX.value_for_config()).c_str());
        ini.SetValue("Magnifier", "CursorOffsetY",
                     GetFloatValue(Instance()->MagnifierCursorOffsetY.value_for_config()).c_str());

        ini.SetValue("Magnifier", "StaticPosX",
                     GetFloatValue(Instance()->MagnifierStaticPosX.value_for_config()).c_str());
        ini.SetValue("Magnifier", "StaticPosY",
                     GetFloatValue(Instance()->MagnifierStaticPosY.value_for_config()).c_str());
    }

    // Menu
    {
        ini.SetValue("Menu", "Scale", GetFloatValue(Instance()->MenuScale).c_str());
        ini.SetValue("Menu", "MenuWidth", GetFloatValue(Instance()->MenuWidth.value_for_config()).c_str());
        ini.SetValue("Menu", "MenuHeight", GetFloatValue(Instance()->MenuHeight.value_for_config()).c_str());
        ini.SetValue("Menu", "MenuPage", Instance()->MenuPage.value_for_config_or("auto").c_str());
        ini.SetValue("Menu", "OverlayMenu", GetBoolValue(Instance()->OverlayMenu.value_for_config()).c_str());

        auto setting = Instance()->ShortcutKey.value_for_config();
        ini.SetValue("Menu", "ShortcutKey",
                     GetIntValue(Instance()->ShortcutKey.value_for_config(), setting > 0).c_str());

        ini.SetValue("Menu", "ExtendedLimits", GetBoolValue(Instance()->ExtendedLimits.value_for_config()).c_str());
        ini.SetValue("Menu", "ShowFps", GetBoolValue(Instance()->ShowFps.value_for_config()).c_str());
        ini.SetValue("Menu", "F5LowHint", GetBoolValue(Instance()->F5LowHint.value_for_config()).c_str());
        ini.SetValue("Menu", "UseHQFont", GetBoolValue(Instance()->UseHQFont.value_for_config()).c_str());
        ini.SetValue("Menu", "DisableSplash", GetBoolValue(Instance()->DisableSplash.value_for_config()).c_str());

        setting = Instance()->FGShortcutKey.value_for_config();
        ini.SetValue("Menu", "FGShortcutKey",
                     GetIntValue(Instance()->FGShortcutKey.value_for_config(), setting > 0).c_str());

        setting = Instance()->FpsShortcutKey.value_for_config();
        ini.SetValue("Menu", "FpsShortcutKey",
                     GetIntValue(Instance()->FpsShortcutKey.value_for_config(), setting > 0).c_str());

        setting = Instance()->FpsCycleShortcutKey.value_for_config();
        ini.SetValue("Menu", "FpsCycleShortcutKey",
                     GetIntValue(Instance()->FpsCycleShortcutKey.value_for_config(), setting > 0).c_str());

        ini.SetValue("Menu", "FpsOverlayPos", GetIntValue(Instance()->FpsOverlayPosition.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayType", GetIntValue(Instance()->FpsOverlayType.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayHorizontal",
                     GetBoolValue(Instance()->FpsOverlayHorizontal.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayShowRatio",
                     GetBoolValue(Instance()->FpsOverlayShowRatio.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsOverlayAlpha", GetFloatValue(Instance()->FpsOverlayAlpha.value_for_config()).c_str());
        ini.SetValue("Menu", "FpsScale", GetFloatValue(Instance()->FpsScale.value_for_config()).c_str());
        ini.SetValue("Menu", "FontSize", GetFloatValue(Instance()->FontSize.value_for_config()).c_str());
        ini.SetValue("Menu", "TTFFontPath",
                     wstring_to_string(Instance()->TTFFontPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Menu", "LightTheme", GetBoolValue(Instance()->LightTheme.value_for_config()).c_str());
        ini.SetValue("Menu", "ModernTheme", GetBoolValue(Instance()->ModernTheme.value_for_config()).c_str());
        ini.SetValue("Menu", "OverlaysUseTheme", GetBoolValue(Instance()->OverlaysUseTheme.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorR", GetFloatValue(Instance()->MenuAccentColorR.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorG", GetFloatValue(Instance()->MenuAccentColorG.value_for_config()).c_str());
        ini.SetValue("Menu", "AccentColorB", GetFloatValue(Instance()->MenuAccentColorB.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorR", GetFloatValue(Instance()->MenuBGColorR.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorG", GetFloatValue(Instance()->MenuBGColorG.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorB", GetFloatValue(Instance()->MenuBGColorB.value_for_config()).c_str());
        ini.SetValue("Menu", "BGColorA", GetFloatValue(Instance()->MenuBGColorA.value_for_config()).c_str());
    }

    // Hooks
    {
        ini.SetValue("Hooks", "HookOriginalNvngxOnly",
                     GetBoolValue(Instance()->HookOriginalNvngxOnly.value_for_config()).c_str());
        ini.SetValue("Hooks", "EarlyHooking", GetBoolValue(Instance()->EarlyHooking.value_for_config()).c_str());
        ini.SetValue("Hooks", "D3D11FeatureLevelElevation",
                     GetBoolValue(Instance()->D3D11FeatureLevelElevation.value_for_config()).c_str());
        ini.SetValue("Hooks", "UseNtdllHooks", GetBoolValue(Instance()->UseNtdllHooks.value_for_config()).c_str());
    }

    // InitFlags
    {
        ini.SetValue("InitFlags", "AutoExposure", GetBoolValue(Instance()->AutoExposure.value_for_config()).c_str());
        ini.SetValue("InitFlags", "HDR", GetBoolValue(Instance()->HDR.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DepthInverted", GetBoolValue(Instance()->DepthInverted.value_for_config()).c_str());
        ini.SetValue("InitFlags", "JitterCancellation",
                     GetBoolValue(Instance()->JitterCancellation.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DisplayResolution",
                     GetBoolValue(Instance()->DisplayResolution.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DynamicResolution",
                     GetBoolValue(Instance()->DynamicResolution.value_for_config()).c_str());
        ini.SetValue("InitFlags", "DisableReactiveMask",
                     GetBoolValue(Instance()->DisableReactiveMask.value_for_config()).c_str());
    }

    // Upscale Ratio Override
    {
        ini.SetValue("UpscaleRatio", "UpscaleRatioOverrideEnabled",
                     GetBoolValue(Instance()->UpscaleRatioOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("UpscaleRatio", "UpscaleRatioOverrideValue",
                     GetFloatValue(Instance()->UpscaleRatioOverrideValue.value_for_config()).c_str());
    }

    // Quality Overrides
    {
        ini.SetValue("QualityOverrides", "QualityRatioOverrideEnabled",
                     GetBoolValue(Instance()->QualityRatioOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioDLAA",
                     GetFloatValue(Instance()->QualityRatio_DLAA.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioUltraQuality",
                     GetFloatValue(Instance()->QualityRatio_UltraQuality.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioQuality",
                     GetFloatValue(Instance()->QualityRatio_Quality.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioBalanced",
                     GetFloatValue(Instance()->QualityRatio_Balanced.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioPerformance",
                     GetFloatValue(Instance()->QualityRatio_Performance.value_for_config()).c_str());
        ini.SetValue("QualityOverrides", "QualityRatioUltraPerformance",
                     GetFloatValue(Instance()->QualityRatio_UltraPerformance.value_for_config()).c_str());
    }

    // Anisotropy
    {
        ini.SetValue("Anisotropy", "AnisotropyOverride",
                     GetIntValue(Instance()->AnisotropyOverride.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "ModifyComparison",
                     GetBoolValue(Instance()->AnisotropyModifyComp.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "ModifyMinMax",
                     GetBoolValue(Instance()->AnisotropyModifyMinMax.value_for_config()).c_str());
        ini.SetValue("Anisotropy", "SkipPointFilter",
                     GetBoolValue(Instance()->AnisotropySkipPointFilter.value_for_config()).c_str());
    }

    // Mipmap
    {
        ini.SetValue("Mipmap", "MipmapBiasOverride",
                     GetFloatValue(Instance()->MipmapBiasOverride.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasOverrideAll",
                     GetBoolValue(Instance()->MipmapBiasOverrideAll.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasFixedOverride",
                     GetBoolValue(Instance()->MipmapBiasFixedOverride.value_for_config()).c_str());
        ini.SetValue("Mipmap", "MipmapBiasScaleOverride",
                     GetBoolValue(Instance()->MipmapBiasScaleOverride.value_for_config()).c_str());
    }

    // Process Filter
    {
        ini.SetValue("ProcessFilter", "TargetProcessName",
                     wstring_to_string(Instance()->TargetProcess.value_for_config_or(L"auto")).c_str());
        ini.SetValue("ProcessFilter", "ProcessExclusionList",
                     wstring_to_string(Instance()->ProcessExclusionList.value_for_config_or(L"auto")).c_str());
    }

    // Hotfixes
    {
        ini.SetValue("Hotfix", "CreateD3D12DeviceForLuma",
                     GetBoolValue(Instance()->CreateD3D12DeviceForLuma.value_for_config()).c_str());
        ini.SetValue("Hotfix", "CheckForUpdate", GetBoolValue(Instance()->CheckForUpdate.value_for_config()).c_str());
        ini.SetValue("Hotfix", "SimulateWaitableObject",
                     GetBoolValue(Instance()->SimulateWaitableObject.value_for_config()).c_str());
        ini.SetValue("Hotfix", "DisableOverlays", GetBoolValue(Instance()->DisableOverlays.value_for_config()).c_str());
        ini.SetValue("Hotfix", "REFrameworkCompat",
                     GetBoolValue(Instance()->REFrameworkCompat.value_for_config()).c_str());

        ini.SetValue("Hotfix", "RoundInternalResolution",
                     GetIntValue(Instance()->RoundInternalResolution.value_for_config()).c_str());

        ini.SetValue("Hotfix", "RestoreComputeSignature",
                     GetBoolValue(Instance()->RestoreComputeSignature.value_for_config()).c_str());
        ini.SetValue("Hotfix", "RestoreGraphicSignature",
                     GetBoolValue(Instance()->RestoreGraphicSignature.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ExtendedStateRestore",
                     GetBoolValue(Instance()->ExtendedStateRestore.value_for_config()).c_str());
        ini.SetValue("Hotfix", "SkipFirstFrames", GetIntValue(Instance()->SkipFirstFrames.value_for_config()).c_str());

        ini.SetValue("Hotfix", "UsePrecompiledShaders",
                     GetBoolValue(Instance()->UsePrecompiledShaders.value_for_config()).c_str());
        ini.SetValue("Hotfix", "PreferDedicatedGpu",
                     GetBoolValue(Instance()->PreferDedicatedGpu.value_for_config()).c_str());
        ini.SetValue("Hotfix", "PreferFirstDedicatedGpu",
                     GetBoolValue(Instance()->PreferFirstDedicatedGpu.value_for_config()).c_str());

        ini.SetValue("Hotfix", "ColorResourceBarrier",
                     GetIntValue(Instance()->ColorResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "MotionVectorResourceBarrier",
                     GetIntValue(Instance()->MVResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "DepthResourceBarrier",
                     GetIntValue(Instance()->DepthResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ColorMaskResourceBarrier",
                     GetIntValue(Instance()->MaskResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "ExposureResourceBarrier",
                     GetIntValue(Instance()->ExposureResourceBarrier.value_for_config()).c_str());
        ini.SetValue("Hotfix", "OutputResourceBarrier",
                     GetIntValue(Instance()->OutputResourceBarrier.value_for_config()).c_str());
    }

    // Dx11 with Dx12
    {
        ini.SetValue("Dx11withDx12", "DontUseNTShared",
                     GetBoolValue(Instance()->DontUseNTShared.value_for_config()).c_str());
    }

    // Logging
    {
        ini.SetValue("Log", "LogToFile", GetBoolValue(Instance()->LogToFile.value_for_config()).c_str());
        ini.SetValue("Log", "LogLevel", GetIntValue(Instance()->LogLevel.value_for_config()).c_str());
        ini.SetValue("Log", "LogToConsole", GetBoolValue(Instance()->LogToConsole.value_for_config()).c_str());
        ini.SetValue("Log", "LogToDebug", GetBoolValue(Instance()->LogToDebug.value_for_config()).c_str());
        ini.SetValue("Log", "LogToNGX", GetBoolValue(Instance()->LogToNGX.value_for_config()).c_str());
        ini.SetValue("Log", "D3D12Dred", GetBoolValue(Instance()->D3D12Dred.value_for_config()).c_str());
        ini.SetValue("Log", "CrashDump", GetBoolValue(Instance()->CrashDump.value_for_config()).c_str());
        ini.SetValue("Log", "OpenConsole", GetBoolValue(Instance()->OpenConsole.value_for_config()).c_str());
        ini.SetValue("Log", "SingleFile", GetBoolValue(Instance()->LogSingleFile.value_for_config()).c_str());
        ini.SetValue("Log", "LogFileName",
                     wstring_to_string(Instance()->LogFileName.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Log", "LogAsync", GetBoolValue(Instance()->LogAsync.value_for_config()).c_str());
        ini.SetValue("Log", "LogAsyncThreads", GetIntValue(Instance()->LogAsyncThreads.value_for_config()).c_str());
    }

    // NvApi
    {
        ini.SetValue("NvApi", "DisableFlipMetering",
                     GetBoolValue(Instance()->DisableFlipMetering.value_for_config()).c_str());
    }

    // DRS
    {
        ini.SetValue("DRS", "DrsMinOverrideEnabled",
                     GetBoolValue(Instance()->DrsMinOverrideEnabled.value_for_config()).c_str());
        ini.SetValue("DRS", "DrsMaxOverrideEnabled",
                     GetBoolValue(Instance()->DrsMaxOverrideEnabled.value_for_config()).c_str());
    }

    // Spoofing
    {
        ini.SetValue("Spoofing", "Dxgi", GetBoolValue(Instance()->DxgiSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiFactoryWrapping",
                     GetBoolValue(Instance()->DxgiFactoryWrapping.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiBlacklist", Instance()->DxgiBlacklist.value_for_config_or("auto").c_str());
        ini.SetValue("Spoofing", "Vulkan", GetBoolValue(Instance()->VulkanSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "VulkanExtensionSpoofing",
                     GetBoolValue(Instance()->VulkanExtensionSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "VulkanVRAM", GetIntValue(Instance()->VulkanVRAM.value_for_config()).c_str());
        ini.SetValue("Spoofing", "DxgiVRAM", GetIntValue(Instance()->DxgiVRAM.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofedGPUName",
                     wstring_to_string(Instance()->SpoofedGPUName.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Spoofing", "StreamlineSpoofing",
                     GetBoolValue(Instance()->StreamlineSpoofing.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofHAGS", GetBoolValue(Instance()->SpoofHAGS.value_for_config()).c_str());
        ini.SetValue("Spoofing", "D3DFeatureLevel",
                     GetBoolValue(Instance()->SpoofFeatureLevel.value_for_config()).c_str());
        ini.SetValue("Spoofing", "UEIntelAtomics",
                     GetBoolValue(Instance()->UESpoofIntelAtomics64.value_for_config()).c_str());
        ini.SetValue("Spoofing", "SpoofedVendorId",
                     GetIntValue(Instance()->SpoofedVendorId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "SpoofedDeviceId",
                     GetIntValue(Instance()->SpoofedDeviceId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "TargetVendorId",
                     GetIntValue(Instance()->TargetVendorId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "TargetDeviceId",
                     GetIntValue(Instance()->TargetDeviceId.value_for_config(), true).c_str());
        ini.SetValue("Spoofing", "Registry", GetBoolValue(Instance()->SpoofRegistry.value_for_config()).c_str());
        ini.SetValue("Spoofing", "RegistryDriver",
                     wstring_to_string(Instance()->SpoofedDriver.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Spoofing", "User32", GetBoolValue(Instance()->SpoofUser32.value_for_config()).c_str());
    }

    // Plugins
    {

        ini.SetValue("Plugins", "Path", wstring_to_string(Instance()->PluginPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Plugins", "LoadSpecialK", GetBoolValue(Instance()->LoadSpecialK.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadReShade", GetBoolValue(Instance()->LoadReShade.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadCustomAmdxc64OnRdna2",
                     GetBoolValue(Instance()->LoadCustomAmdxc64OnRdna2.value_for_config()).c_str());
        ini.SetValue("Plugins", "LoadAsiPlugins", GetBoolValue(Instance()->LoadAsiPlugins.value_for_config()).c_str());
        ini.SetValue("Plugins", "LateAsiPluginsDelay",
                     GetIntValue(Instance()->LateAsiPluginsDelay.value_for_config()).c_str());
        ini.SetValue("Plugins", "DisabledAsi",
                     wstring_to_string(Instance()->DisabledAsi.value_for_config_or(L"auto")).c_str());
    }

    // fakenvapi
    {
        ini.SetValue("fakenvapi", "UseFakenvapi", GetBoolValue(Instance()->UseFakenvapi.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceXeLL", GetBoolValue(Instance()->ForceXeLL.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceLatencyFlex",
                     GetBoolValue(Instance()->FN_ForceLatencyFlex.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "LatencyFlexMode",
                     GetIntValue(Instance()->FN_LatencyFlexMode.value_for_config()).c_str());
        ini.SetValue("fakenvapi", "ForceReflex", GetIntValue(Instance()->FN_ForceReflex.value_for_config()).c_str());
    }

    // inputs
    {
        ini.SetValue("Inputs", "EnableDlssInputs",
                     GetBoolValue(Instance()->EnableDlssInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableXeSSInputs",
                     GetBoolValue(Instance()->EnableXeSSInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2Inputs", GetBoolValue(Instance()->UseFsr2Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2Dx11Inputs",
                     GetBoolValue(Instance()->UseFsr2Dx11Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr2VulkanInputs",
                     GetBoolValue(Instance()->UseFsr2VulkanInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "Fsr2Pattern", GetBoolValue(Instance()->Fsr2Pattern.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFsr3Inputs", GetBoolValue(Instance()->UseFsr3Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "Fsr3Pattern", GetBoolValue(Instance()->Fsr3Pattern.value_for_config()).c_str());
        ini.SetValue("Inputs", "UseFfxInputs", GetBoolValue(Instance()->UseFfxInputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableHotSwapping",
                     GetBoolValue(Instance()->EnableHotSwapping.value_for_config()).c_str());

        ini.SetValue("Inputs", "EnableFsr2Inputs",
                     GetBoolValue(Instance()->EnableFsr2Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableFsr3Inputs",
                     GetBoolValue(Instance()->EnableFsr3Inputs.value_for_config()).c_str());
        ini.SetValue("Inputs", "EnableFfxInputs", GetBoolValue(Instance()->EnableFfxInputs.value_for_config()).c_str());
    }

    // HDR (Convert is the live A/B toggle and is not written; ForceHDR is read into Enabled and not written back)
    {
        ini.SetValue("HDR", "Enabled", GetBoolValue(Instance()->HdrOutputEnabled.value_for_config()).c_str());
        ini.SetValue("HDR", "UseHDR10", GetBoolValue(Instance()->UseHDR10.value_for_config()).c_str());
        ini.SetValue("HDR", "PeakNits", GetFloatValue(Instance()->HdrOutputPeakNits.value_for_config()).c_str());
        ini.SetValue("HDR", "GameNits", GetFloatValue(Instance()->HdrOutputGameNits.value_for_config()).c_str());
        ini.SetValue("HDR", "Decode", GetIntValue(Instance()->HdrOutputDecode.value_for_config()).c_str());
        {
            auto compare = Instance()->HdrCompareKey.value_for_config();
            ini.SetValue("HDR", "CompareKey", GetIntValue(compare, compare > 0).c_str());
        }
        ini.Delete("HDR", "ForceHDR", true);
        ini.Delete("HDR", "SkipColorSpace", true);
    }

    // V-Sync
    {
        ini.SetValue("V-Sync", "OverrideVsync", GetBoolValue(Instance()->OverrideVsync.value_for_config()).c_str());
        ini.SetValue("V-Sync", "ForceVsync", GetBoolValue(Instance()->ForceVsync.value_for_config()).c_str());
        ini.SetValue("V-Sync", "SyncInterval", GetIntValue(Instance()->VsyncInterval.value_for_config()).c_str());

        if (Instance()->VsyncInterval.has_value())
        {
            if (Instance()->VsyncInterval.value() < 0 || Instance()->VsyncInterval.value() > 3)
                Instance()->VsyncInterval.reset();
        }
    }

    // Libraries
    {
        ini.SetValue("Libraries", "OptiDllPath",
                     wstring_to_string(Instance()->MainDllPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "NvngxPath",
                     wstring_to_string(Instance()->NvngxPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvngxFeaturePath",
                     wstring_to_string(Instance()->DLSSFeaturePath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvngxDlssPath",
                     wstring_to_string(Instance()->NVNGX_DLSS_Library.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "NvapiPath",
                     wstring_to_string(Instance()->NvapiDllPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "FfxDx12Path",
                     wstring_to_string(Instance()->FfxDx12Path.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12SRPath",
                     wstring_to_string(Instance()->FfxDx12SRPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12FGPath",
                     wstring_to_string(Instance()->FfxDx12FGPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12RRPath",
                     wstring_to_string(Instance()->FfxDx12RRPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxDx12RCPath",
                     wstring_to_string(Instance()->FfxDx12RCPath.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "FfxVkPath",
                     wstring_to_string(Instance()->FfxVkPath.value_for_config_or(L"auto")).c_str());

        ini.SetValue("Libraries", "XeSSPath",
                     wstring_to_string(Instance()->XeSSLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeFGPath",
                     wstring_to_string(Instance()->XeFGLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeLLPath",
                     wstring_to_string(Instance()->XeLLLibrary.value_for_config_or(L"auto")).c_str());
        ini.SetValue("Libraries", "XeSSDx11Path",
                     wstring_to_string(Instance()->XeSSDx11Library.value_for_config_or(L"auto")).c_str());
    }

    // Old configs, just delete them
    {
        ini.Delete("FSR", "Fsr4ForceEnableInt8");
        ini.Delete("FSR", "Fsr4ForceModel");
        ini.Delete("Nukems", "MakeDepthCopy", true);
    }

    auto pathWStr = absoluteFileName.wstring();

    LOG_INFO("Trying to save ini to: {0}", wstring_to_string(pathWStr));

    if (ini.SaveFile(absoluteFileName.wstring().c_str()) < 0)
        return false;

    savedDlssNr = DlssNrSnapshot();
    return true;
}

bool Config::SaveXeFG()
{
    ini.SetValue("XeFG", "DepthInverted", GetBoolValue(Instance()->FGXeFGDepthInverted.value_for_config()).c_str());
    ini.SetValue("XeFG", "JitteredMV", GetBoolValue(Instance()->FGXeFGJitteredMV.value_for_config()).c_str());
    ini.SetValue("XeFG", "HighResMV", GetBoolValue(Instance()->FGXeFGHighResMV.value_for_config()).c_str());

    auto pathWStr = absoluteFileName.wstring();
    LOG_INFO("Trying to save ini to: {0}", wstring_to_string(pathWStr));

    return ini.SaveFile(absoluteFileName.wstring().c_str()) >= 0;
}

void Config::CheckUpscalerFiles()
{
    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"nvngx.dll");

    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"_nvngx.dll");

    if (!State::Instance().nvngxExists)
    {
        State::Instance().nvngxExists = GetModuleHandle(L"nvngx.dll") != nullptr;

        if (!State::Instance().nvngxExists)
            State::Instance().nvngxExists = GetModuleHandle(L"_nvngx.dll") != nullptr;

        if (State::Instance().nvngxExists)
            LOG_INFO("nvngx.dll found in memory");
        else
            LOG_WARN("nvngx.dll not found!");
    }
    else
    {
        LOG_INFO("nvngx.dll found in game folder");
    }

    if (auto nvngxReplacement = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlss.dll");
        nvngxReplacement.has_value())
    {
        State::Instance().nvngxReplacement = nvngxReplacement.value().wstring();
    }

    State::Instance().libxessExists = std::filesystem::exists(Util::ExePath().parent_path() / L"libxess.dll");
    if (!State::Instance().libxessExists)
    {
        State::Instance().libxessExists = GetModuleHandle(L"libxess.dll") != nullptr;

        if (State::Instance().libxessExists)
            LOG_INFO("libxess.dll found in memory");
        else
            LOG_WARN("libxess.dll not found!");
    }
    else
    {
        LOG_INFO("libxess.dll found in game folder");
    }
}

bool Config::FGCanFollowGame()
{
    const auto& state = State::Instance();
    return FGFollowGame::CanFollow(state.activeFgInput == FGInput::DLSSG, state.streamlineVersion.major);
}

bool Config::FGFollowsGame() const { return FGFollowGame::Follows(FGEnabled.has_value(), FGCanFollowGame()); }

bool Config::IsFGEnabled() const
{
    const bool follows = FGFollowsGame();
    auto& state = State::Instance();

    if (follows)
    {
        const auto format = state.currentSwapchainDesc.BufferDesc.Format;
        const bool floatSwapchain = format >= DXGI_FORMAT_R32G32B32A32_TYPELESS && format <= DXGI_FORMAT_R16G16B16A16_SINT;

        if (FGFollowGame::FollowBlocked(state.activeFgOutput == FGOutput::XeFG, FGXeFGIgnoreInitChecks.value_or_default(),
                                        state.realExclusiveFullscreen, floatSwapchain))
            return false;
    }

    return FGFollowGame::Enabled(follows, state.dlssgInputGameRequest.load(), FGEnabled.value_or_default());
}

int Config::FGInterpolationCount(int menuCount, int outputMax) const
{
    return FGFollowGame::Count(FGFollowsGame(), State::Instance().dlssgInputGameRequest.load(), menuCount, outputMax);
}

std::vector<std::string> Config::GetConfigLog() { return _log; }

std::optional<std::string> Config::readString(std::string section, std::string key, bool lowercase)
{
    std::string value = readSource->GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    if (readSource == &ini)
        _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? lower : value;
}

std::optional<std::wstring> Config::readWString(std::string section, std::string key, bool lowercase)
{
    std::string value = readSource->GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    if (readSource == &ini)
        _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? string_to_wstring(lower) : string_to_wstring(value);
}

std::optional<float> Config::readFloat(std::string section, std::string key)
{
    auto value = readString(section, key);

    try
    {
        float result;

        if (value.has_value() && isFloat(value.value(), result))
            return result;

        return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<int> Config::readInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<uint32_t> Config::readUInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<bool> Config::readBool(std::string section, std::string key)
{
    auto value = readString(section, key, true);
    if (value == "true")
        return true;
    else if (value == "false")
        return false;

    return std::nullopt;
}

// Only use for unsigned enums that have Enum::Count as the last entry
template <typename Enum> std::optional<Enum> Config::readEnum(std::string section, std::string key)
{
    static_assert(std::is_enum_v<Enum>, "Enum type required");

    auto value = readUInt(section, key);
    if (!value.has_value())
        return std::nullopt;

    using Underlying = std::underlying_type_t<Enum>;

    if (*value < static_cast<Underlying>(Enum::Count))
        return static_cast<Enum>(*value);

    return std::nullopt;
}

Config* Config::Instance()
{
    if (!_config)
        _config = new Config();

    return _config;
}
