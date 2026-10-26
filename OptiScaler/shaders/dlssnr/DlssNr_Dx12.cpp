#include "pch.h"
#include <dlssnr/PassProfiles.h>
#include <dlssnr/DlssNr_SuperiorScaling.h>
#include <dlssnr/DlssNr_SuperiorGains.h>
#include <dlssnr/DlssNr_GpuLifetime.h>
#include <output/SdrToHdr_ProxyDx12.h>
#include <native/DriverInitGate.h>

#include <set>
#include <wrl/client.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNrNative.h>
#include <dlssnr/DlssNrVitReuse.h>
#include <dlssnr/ResidualFg.h>
#include <dlssnr/DlssNrDetailReuse.h>
#include <dlssnr/DlssNrDetailReuseHost.h>
#include <dlssnr/DlssNrEditCache.h>
#include <dlssnr/DlssNrSceneCut.h>
#include <motion/SceneCut_Dx12.h>
#include <DirectXMath.h>


#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include "DlssNr_Dx12.h"
#include "DlssNr_SurfaceFormats.h"
#include "DlssNr_ActiveColor.h"
#include "DlssNr_Guides.h"
#include "DlssNr_SeamClock.h"
#include "DlssNr_GameScale.h"
#include "DlssNr_TrimAnchors.h"
#include "DlssNr_AutoTrimDefault.h"
#include "DlssNr_ColourEncoding.h"
#include <dlssnr/DlssNr_ColourEncodingStatus.h>
#include <dlssnr/DlssNr_LutPack.h>
#include <dlssnr/DlssNr_LutStatus.h>
#include "DlssNr_FollowGame.h"
#include "DlssNr_ExposureCalibrate.h"
#include "DlssNr_ExposureCalibrate_Run.h"
#include "DlssNr_ProxyCurve.h"
#include "DlssNr_ExposureAdapt.h"
#include "DlssNr_ExposureFoundBlend.h"
#include "DlssNr_ExposureMeter.h"
#include "DlssNr_FinishedReady.h"
#include <dlssnr/DlssNr_StreamlinePicture.h>
#include <dlssnr/DlssNr_GameDefaults.h>

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>
#include "DlssNr_WatchedLists.h"
#include "DlssNr_GpuTime.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cstring>
#include "precompile/DlssNr_Shader.h"
#include "precompile/dlssnr_finished_color_Shader.h"
#include "precompile/dlssnr_detail_stats_Shader.h"
#include "precompile/dlssnr_detail_reuse_Shader.h"
#include "precompile/dlssnr_edit_cache_Shader.h"
#include "precompile/dlssnr_spatial_Shader.h"
#include "precompile/dlssnr_spatial_guides_Shader.h"
#include "superior/precompile/host_surface_guides_Shader.h"
#include "superior/precompile/host_uniform_surface_guides_Shader.h"
#include "superior/precompile/host_joint_guides_Shader.h"
#include "superior/precompile/host_joint_minified_guides_Shader.h"
#include "superior/precompile/host_uniform_joint_guides_Shader.h"
#include "superior/precompile/host_uniform_joint_minified_guides_Shader.h"
#include "superior/precompile/host_joint_guides_quantized_Shader.h"
#include "superior/precompile/host_joint_minified_guides_quantized_Shader.h"
#include "superior/precompile/host_uniform_joint_guides_quantized_Shader.h"
#include "superior/precompile/host_uniform_joint_minified_guides_quantized_Shader.h"
#include "precompile/dlssnr_exposure_adapt_Shader.h"
#include "precompile/dlssnr_lut_Shader.h"
#include "../output_scaling/OS_Dx12.h"
#include "../sgsr1/SGSR1_Dx12.h"

namespace
{
// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00003: return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004: return "FAIL_FeatureNotFound";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00006: return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007: return "FAIL_NotInitialized";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD0000F: return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    case 0xBAD00012: return "FAIL_NotImplemented";
    default: return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control =
        (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})",
             result, NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright =
        result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*) (ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*,
                                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                      unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                      unsigned int, unsigned int, unsigned int, unsigned int, int, int, float,
                                      int, float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetExtras = void(__cdecl*) (void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                        unsigned int, unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.

using DlssNr::Profiles::NrPassTuning;
using DlssNr::Profiles::PassPreset;
using DlssNr::Profiles::PassStyle;
using DlssNr::Profiles::PassTuning;

struct NrState
{
    unsigned long long successfulDispatches = 0;
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;
    const char* (*lastModelError)() = nullptr;
    std::string modelError;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    // The loaded nvngx.dll_dlssnr.dll is the vendor-neutral port (it exports dlssnr_backend_id): it runs the
    // model itself on any GPU and never reads the capability block, so no NVIDIA NGX core is needed.
    bool isPort = false;
    void* feature = nullptr;
    bool superiorMainOnlyGains = false; // actual selected NR runtime, cached at feature creation
    bool featurePendingSubmission = false;
    unsigned long long featureCreateEpoch = 0;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to. Extra features are created on a
    // build-only invocation and first evaluated on a later command list.
    void* passFeature[DlssNr::MaxPassCount] = {};
    bool passNeedsReset[DlssNr::MaxPassCount] = {};
    bool passCreateFailed[DlssNr::MaxPassCount] = {};
    bool passPendingSubmission[DlssNr::MaxPassCount] = {};
    unsigned long long passCreateEpoch[DlssNr::MaxPassCount] = {};

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The second half of the model-output ping-pong. The base proxy stays immutable: pass 0 writes
    // output (A), pass 1 writes this (B), and pass 2 writes A again. Only the final answer is composed.
    ID3D12Resource* passScratch = nullptr;
    bool passScratchFailed = false;

    // A pass's raw answer, saturated back into the proxy's valid range, before it becomes the next
    // pass's input. Two of these ping-pong (mirroring output/passScratch above), not one: the clamp
    // step reads the previous boundary's clamped proxy as gModel (to rescale the edit against, see the
    // interpass clamp's own comment) while writing the new one as gTarget -- with only one buffer,
    // the second boundary in a 3-pass chain would bind the same resource as both, an SRV/UAV alias on
    // the same dispatch (found in review; the original always-on clamp never read a second resource,
    // so this collision didn't exist before CubeScaleResidual needed the proxy too).
    ID3D12Resource* passClampScratch = nullptr;
    ID3D12Resource* passClampScratch2 = nullptr;
    bool passClampScratchFailed = false;
    bool passClampScratch2Failed = false;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // Compact origin-zero pre-SR image, only needed when Color has allocation padding. All codec,
    // hold and capture paths then see the real raster. UAV at rest, retired with the scratch set.
    ID3D12Resource* activeColor = nullptr;

    // LUT-apply epic (dlssnr-lut-apply), Story 2: the LUT pass's graded copy of
    // `target`, same size/format, only allocated while a LUT is actually loaded. UAV at rest, like
    // activeColor -- the graded result is copied back onto `target` itself (DlssNr_Dx12::DispatchLut's
    // caller), so every later stage reads the same resource it always did with no extra indirection.
    ID3D12Resource* lutScratch = nullptr;
    bool lutScratchFailed = false;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    //
    // Reduced up-leg (working scale < 1): outputNative doubles as the SGSR1-enlarged answer here too
    // -- the two cases are mutually exclusive per frame (workScale is a single scalar) and both
    // dispatches fully overwrite the buffer, so sharing it needs no extra lifecycle handling.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;

    // Reduced up-leg. DlssNrReducedUpscaleMethod == 1 enlarges the answer with SGSR1's
    // edge-directed upscale; the model's own work-resolution source still correctly reads via
    // the resolve's own implicit bilinear tap (dlssnr.hlsl:931-932) -- that is not the old
    // colorCopy-vs-modelInput bug ("the low res image got combined with the final image"), which
    // was comparing against the wrong buffer entirely, not merely a softer-filtered one.
    //
    // Built for one Dispatch() call per frame, like superUp/superDown: a single
    // non-double-buffered _constantBuffer and a 2-slot FrameDescriptorHeap meant to alternate
    // *across frames*. No filter choice (one fixed shader), so it isn't tied to nrScaler and is
    // built once.
    SGSR1_Dx12* sgsr1UpAnswer = nullptr;
    Scaler nrScaler = Scaler::Count;

    // Experimental input styles (DlssNr_InputStyles.h): the model's picture restyled (DlssNrMode_Watercolor,
    // DlssNrMode_StructureHint or DlssNrMode_SmoothCanvas), which then stands in for the model input and the resolve's proxy. Created when first
    // used, parked with the scratch set; its state is tracked because the resolve reads it.
    ID3D12Resource* styled = nullptr;
    D3D12_RESOURCE_STATES styledState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The second texture the styles alternate with when more than one is on
    ID3D12Resource* styledB = nullptr;
    D3D12_RESOURCE_STATES styledBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    // Where NR runs without a Tune moving it (EvaluateInternal): before SR, for the run's wait (CalibrationSituation).
    bool beforeSrPlacement = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;


    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The working scale actually used last frame -- manual, or RR + Auto's derived ratio. Read back by
    // CurrentModelResolutionPercent() so the menu can show the live value instead of the stale manual
    // one while Auto is in effect.
    float appliedWorkScale = 1.0f;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // Automatic exposure (white point source 3): the 64x64 meter's tile means reduced on the GPU to a
    // 1x1 exposure texture the encode and resolve read the same frame. Its value also rides home on
    // the meter's readback ring, but only for the menu and for capturing Trim anchors; the picture
    // never waits on it. `autoExposureReadable` is whether the texture is currently in the
    // shader-resource state rather than the UAV state it is created in.
    ID3D12Resource* autoExposure = nullptr;
    bool autoExposureReadable = false;
    float autoExposureValue = 0.0f;

    // Auto's found exposure (DlssNr_ExposureScan.h FoundExposure): the base white point it gives this frame, 0 when
    // none is in force. Read back a few frames behind like the meter's value, so while it holds the white point is the
    // host's (the shader is not asked to recompute it from the meter's live texture). Eased in and out
    // (DlssNr_ExposureFoundBlend.h).
    float foundBaseWhitePoint = 0.0f;
    DlssNrExposureFoundBlend::Blend foundBlend;

    // Eye adaptation (DlssNr_ExposureAdapt.h): the meter's own reading lands in autoExposureRaw (kept in the UAV state
    // between evaluations) and a one-texel pass eases autoExposure toward it, so autoExposure above is the eased value
    // and everything downstream reads that. autoExposureRawValue is the reading's readback, for the log.
    ID3D12Resource* autoExposureRaw = nullptr;
    float autoExposureRawValue = 0.0f;
    bool autoExposureAdapting = false;
    bool autoExposureRawFailed = false;
    DlssNrExposureAdapt::Adapter autoExposureAdapter;
    float autoExposurePreExposure = 1.0f;
    unsigned long long autoExposureFrames = 0;

    // The game's exposure, read back beside Automatic's from the same frame: what the follow-game calibration learns
    // from, and the host's white point while following (DlssNr_FollowGame.h). `followingGame` is this frame's decision.
    float autoPairGameExposure = 0.0f;
    float autoPairPreExposure = 1.0f;
    bool followingGame = false;

    // Which white point source last fed the readback ring. A source change invalidates the ring, so
    // one source's numbers are never read as another's.
    uint32_t exposureReadbackSource = 0;

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    //
    // What the slot holds: 0 nothing believable, 1 the game's exposure, 2 the automatic exposure. The
    // pre-exposure it was measured against travels with it for the same reason.
    uint32_t meterExposureKind[4] = {};
    float meterExposurePreExposure[4] = {};
    bool meterPairHasGame[4] = {}; // the slot also carries the game's exposure in texel 1
    bool meterHasRaw[4] = {};      // ... and Automatic's reading before eye adaptation in texel 2
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // Frame statistics diagnostic (ini [DlssNr] FrameStats). One sample is queued every 120 frames and
    // read eight frames later, so a single readback pair is enough: the grid of tile luminances, and the
    // game's exposure texture couriered into tile 0 of the same meter. diagQueuedAt is the frame the
    // sample was queued on, 0 when none is pending. The rest describe that frame.
    ID3D12Resource* diagGridReadback = nullptr;
    ID3D12Resource* diagExposureReadback = nullptr;
    // The proxy itself (what the encode wrote and the model is shown), metered the same way after the encode.
    ID3D12Resource* diagProxyReadback = nullptr;
    bool diagProxyQueued = false;
    bool diagPassthrough = false;
    unsigned long long diagQueuedAt = 0;
    DXGI_FORMAT diagFormat = DXGI_FORMAT_UNKNOWN;
    unsigned int diagWidth = 0;
    unsigned int diagHeight = 0;
    float diagPreExposure = 1.0f;
    bool diagExposureSupplied = false;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool beforeUpscale = false;
    bool rayReconstruction = false;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values each live feature was created with. Preset and style may differ per layer; the
    // remaining strengths are intentionally shared by the stack.
    unsigned int builtPreset[DlssNr::MaxPassCount] = {};
    float builtIntensity = 0.0f;
    NrPassTuning builtPassTuning[DlssNr::MaxPassCount] {};
    unsigned int builtStyle[DlssNr::MaxPassCount] = {};
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";

    // The game's last output format, to say once that it swaps RGBA8 and BGRA8 (the surfaces and model are kept)
    DXGI_FORMAT lastGameFormat = DXGI_FORMAT_UNKNOWN;
    bool saidFormatSwap = false;
};

NrState g_nr;

// DlssNr_SurfaceFormats.h takes no Windows headers and spells these formats as numbers
static_assert(DXGI_FORMAT_R8G8B8A8_TYPELESS == 27 && DXGI_FORMAT_R8G8B8A8_UNORM == 28 &&
              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB == 29 && DXGI_FORMAT_B8G8R8A8_UNORM == 87 &&
              DXGI_FORMAT_B8G8R8A8_TYPELESS == 90 && DXGI_FORMAT_B8G8R8A8_UNORM_SRGB == 91);

// Automatic's own base white point (PreExposure / its metered exposure, never the followed game's), from the CPU
// readbacks a few frames behind the shader's live value. 0 when there is no reading yet.
float AutoOwnBaseWhitePoint()
{
    return g_nr.autoExposureValue > 1e-8f ? g_nr.autoExposurePreExposure / g_nr.autoExposureValue : 0.0f;
}

// Automatic exposure's base white point, the value its Trim multiplies: the game's base white point times the
// learned calibration while following the game (DlssNr_FollowGame.h), Auto's found exposure while the scan has one,
// else Automatic's own.
float AutoBaseWhitePoint()
{
    if (g_nr.followingGame && g_nr.autoPairGameExposure > 1e-8f)
        return g_nr.autoPairPreExposure / g_nr.autoPairGameExposure * DlssNrFollowGame::Instance().Scale();

    if (g_nr.foundBaseWhitePoint > 0.0f)
        return g_nr.foundBaseWhitePoint;

    return AutoOwnBaseWhitePoint();
}

// While following the game: how far the followed base sits from Automatic's own, in EV; 0 otherwise.
float FollowDisagreementEv()
{
    return g_nr.followingGame ? DlssNrExposureCalibrate::BaseDisagreementEv(AutoBaseWhitePoint(), AutoOwnBaseWhitePoint())
                              : 0.0f;
}
std::unique_ptr<DlssNr_Dx12> g_compose;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<DlssNrGpuTime> g_gpuTime;

// A second timer, around the model's evaluate and nothing else.
//
// The first one brackets the whole pass, which is the number the menu shows and the right one for
// "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
// full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
// encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
// resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
// work to NGX overhead.
//
// Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
// we can actually do something about.
std::unique_ptr<DlssNrGpuTime> g_ngxTime;
std::optional<double> g_lastNgxTime;

// A third, around the exposure scan's copies (DlssNr::ExposureScan::Tick), for what searching costs -- it runs by
// itself on Auto, so its price is paid by everyone on Auto until it finds or gives up. Averaged over the log interval.
std::unique_ptr<DlssNrGpuTime> g_scanTime;
double g_scanTimeSum = 0.0, g_scanTimeMax = 0.0;
unsigned g_scanTimeSamples = 0;
std::optional<double> g_lastGpuTime;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;
// The capture running is an edit cache dump (RequestCacheDump): it also copies the game's motion vectors and depth
// and notes each frame's guide facts, for the offline replay of carrying an edit from frame to frame
// (DLSS-TOOLS editcache). Written to dlssnr-cachedump beside OptiScaler.
bool g_captureIsDump = false;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

#include "DlssNr_ExposureCalibrate_Dx12.inl"

// Logical frame identity for deferred pairing; feature readiness keeps the raw submission counter.
DlssNrSeamClock g_nrSeamClock;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
void CheckCaptureTrigger()
{
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }

    const auto dumpTrigger = Util::DllPath().remove_filename() / "dlssnr-cachedump.trigger";

    if (std::filesystem::exists(dumpTrigger, ec))
    {
        std::filesystem::remove(dumpTrigger, ec);
        DlssNr::RequestCacheDump(16);
        LOG_INFO("DLSS-NR edit cache dump requested by trigger file");
    }
}

// Depth formats copied into an edit cache dump: one plane, read back as is. Depth-stencil layouts with a stencil plane
// (R24_UNORM_X8, R32_FLOAT_X8X24) are left out rather than risk a copy the device rejects; the dump then says so and
// the replay validates by colour alone.
bool DumpableDepth(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R16_UNORM ||
           f == DXGI_FORMAT_D16_UNORM;
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

const char* SelectedModelFile()
{
    return "nvngx_dlssnr.dll";
}

std::optional<std::filesystem::path> FindSelectedModel()
{
    auto path = Util::FindFilePath(g_dllDir, SelectedModelFile());
    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), SelectedModelFile());
    return path;
}

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr && g_nr.evaluate != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    // Only the vendor-neutral port exports this; the NGX forwarder does not.
    g_nr.isPort = GetProcAddress(g_nr.forwarder, "dlssnr_backend_id") != nullptr;

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate_v2");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");
    g_nr.lastModelError = (const char*(*)()) GetProcAddress(g_nr.forwarder, "dlssnr_call_error");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "Update nvngx.dll_dlssnr.dll from the complete release (NR v2 exports required)";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {} ({})", path.string(),
             g_nr.isPort ? "vendor-neutral port, no NGX core needed" : "NVIDIA NGX");
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf",         "Balanced",    "MaxQuality",
                                    "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    // nullptr -> release through the DLSS-NR forwarder's own dlssnr_call_release (g_nr.release), as
    // every feature here used to. A non-null override is for a feature of a different NGX type,
    // released through a different API entirely.
    void (*featureRelease)(void*) = nullptr;
    // Retired with the feature it belongs to, never before it. NGX reads a parameter block for as
    // long as the evaluate that was handed it is still in flight, and with frame generation that is
    // several frames after the CPU moved on -- destroying it on the spot while the feature itself sat
    // out the usual 32-frame delay is the same use-after-free this whole retirement list exists to
    // prevent. DlssNr_DeferredSr.inl's ~Generation tears both down together for the same reason.
    NVSDK_NGX_Parameter* parameters = nullptr;
    int framesLeft = 32;
};

std::vector<NrRetired> g_nrRetired;

void ParkNrFeature(void*& feature, void (*releaseFn)(void*) = nullptr,
                  NVSDK_NGX_Parameter** parameters = nullptr)
{
    if (feature == nullptr && (parameters == nullptr || *parameters == nullptr))
        return;

    NrRetired r;
    r.feature = feature;
    r.featureRelease = releaseFn;
    feature = nullptr;

    if (parameters != nullptr)
    {
        r.parameters = *parameters;
        *parameters = nullptr;
    }

    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void ReleaseNrFeature(const NrRetired& r)
{
    if (r.feature != nullptr)
    {
        if (r.featureRelease != nullptr)
            r.featureRelease(r.feature);
        else if (g_nr.release != nullptr)
            g_nr.release(r.feature);
    }

    // After the feature, never before -- same order as DlssNr_DeferredSr.inl's ~Generation.
    if (r.parameters != nullptr && NVNGXProxy::D3D12_DestroyParameters())
        NVNGXProxy::D3D12_DestroyParameters()(r.parameters);
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (--g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        ReleaseNrFeature(g_nrRetired[i]);

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

// modelFormat is the model surfaces' format: the game's (canonical), or RGBA16F while Compress screen edges packs the
// picture. colourFormat is the game's colour format made canonical (DlssNr_SurfaceFormats.h: BGRA8 -> RGBA8), which
// colorCopy, colorSmall and outputNative are made in. hdrCopy, activeColor and lutScratch go with a real change too,
// and check the exact game format themselves at their creation, so an RGBA8/BGRA8 swap re-makes only those. Both are
// checked: with compression on the model format does not move when the game's does (DlssNr_SurfaceFormats.h).
void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT modelFormat, DXGI_FORMAT colourFormat)
{
    const DXGI_FORMAT haveModel = g_nr.output != nullptr ? g_nr.output->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT haveColour = g_nr.colorCopy != nullptr ? g_nr.colorCopy->GetDesc().Format : DXGI_FORMAT_UNKNOWN;

    if (!DlssNr::SurfacesStale((int) haveModel, (int) modelFormat, (int) haveColour, (int) colourFormat))
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: model format {} -> {}, colour format {} -> {} (inject point or Compress "
             "screen edges changed)",
             (int) haveModel, (int) modelFormat, (int) haveColour, (int) colourFormat);

    ForgetCalibration();

    ParkNrFeature(g_nr.feature);
    g_nr.featurePendingSubmission = false;

    // The extras go with it: they were built for this raster and this tuning too.
    for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
    {
        ParkNrFeature(g_nr.passFeature[i]);
        g_nr.passNeedsReset[i] = false;
        g_nr.passCreateFailed[i] = false;
        g_nr.passPendingSubmission[i] = false;
    }

    for (ID3D12Resource** r :
         { &g_nr.output, &g_nr.passScratch, &g_nr.passClampScratch, &g_nr.passClampScratch2, &g_nr.colorCopy,
           &g_nr.hdrCopy, &g_nr.colorSmall, &g_nr.outputNative, &g_nr.activeColor, &g_nr.lutScratch })
        ParkNrResource(*r);

    g_nr.passScratchFailed = false;
    g_nr.passClampScratchFailed = false;
    g_nr.passClampScratch2Failed = false;
    g_nr.lutScratchFailed = false;

    g_nr.reset = true;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
// texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                         bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureKind[slot] = exposureBound ? 1u : 0u;
    g_nr.meterPairHasGame[slot] = false;
    g_nr.meterExposurePreExposure[slot] = g_nr.gamePreExposure;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Queues the automatic exposure's 1x1 value for readback, into the same ring the game's exposure uses.
// Only the menu and anchor capture read it; the picture uses the texture itself.
//
// With `withGameExposure`, the courier has just put the game's exposure in the meter's tile (0,0), and it rides in texel 1
// of the same slot: a pair from one frame, for the follow-game calibration.
//
// With `raw` (in the copy-source state), Automatic's reading before eye adaptation rides in texel 2, for the log.
void CopyAutoExposureToReadback(ID3D12GraphicsCommandList* cmdList, float preExposure, bool withGameExposure,
                                ID3D12Resource* raw)
{
    if (g_nr.autoExposure == nullptr)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    g_nr.meterExposureKind[slot] = 2u;
    g_nr.meterExposurePreExposure[slot] = std::isfinite(preExposure) && preExposure > 1e-6f ? preExposure : 1.0f;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.autoExposure;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = 1;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.autoExposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.autoExposure, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    g_nr.meterPairHasGame[slot] = false;
    g_nr.meterHasRaw[slot] = raw != nullptr;

    if (raw != nullptr)
    {
        D3D12_TEXTURE_COPY_LOCATION rawSrc = src;
        rawSrc.pResource = raw;
        D3D12_TEXTURE_COPY_LOCATION rawDst = dst;
        rawDst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
        cmdList->CopyTextureRegion(&rawDst, 2, 0, 0, &rawSrc, nullptr);
    }

    if (withGameExposure && g_nr.meter != nullptr)
    {
        D3D12_TEXTURE_COPY_LOCATION meterSrc {};
        meterSrc.pResource = g_nr.meter;
        meterSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        meterSrc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION pairDst = dst;
        pairDst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;

        const D3D12_BOX tile0 { 0, 0, 0, 1, 1, 1 };

        Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyTextureRegion(&pairDst, 1, 0, 0, &meterSrc, &tile0);
        Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        g_nr.meterPairHasGame[slot] = true;
    }

    g_nr.meterFrames++;
    g_nr.autoExposureFrames++;
}

// Copies the meter's whole grid into `buffer`, leaving the meter as it found it (a UAV). Unlike
// CopyMeterToReadback this is not part of the exposure ring: it advances nothing and nothing reads
// it but the frame statistics diagnostic below.
void CopyMeterGridTo(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* buffer)
{
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

const char* DiagFormatName(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        default: return "other";
    }
}

float DiagSrgbEncode(float linear)
{
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Reads the sample queued by the FrameStats block in Dispatch and writes one log line.
//
// The grid is the meter's own: the mean luminance of each of 64x64 screen tiles of the frame NR was
// handed, before this pass wrote anything. Divided by the pre-exposure that is scene units. A tile is
// an average of a few hundred pixels, so a sun disc is diluted and "max" here is the brightest tile,
// not the brightest pixel; what it does show reliably is whether the frame lives near 1 (display
// scaled) or far above it (scene referred).
//
// "proxy sRGB" is the brightness the model would see at that luminance under the white point in force,
// before the soft knee: sRGB(value / white point). Compare it with what the same part of the picture
// looks like on screen.
void ReportFrameStats(float whitePoint, uint32_t source)
{
    ID3D12Resource* gridBuffer = g_nr.diagGridReadback;
    ID3D12Resource* exposureBuffer = g_nr.diagExposureReadback;
    g_nr.diagQueuedAt = 0;

    if (gridBuffer == nullptr || exposureBuffer == nullptr)
        return;

    void* gridMapped = nullptr;
    void* exposureMapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };
    std::string proxyText = "not measured";

    if (g_nr.diagProxyQueued && g_nr.diagProxyReadback != nullptr)
    {
        void* proxyMapped = nullptr;

        if (SUCCEEDED(g_nr.diagProxyReadback->Map(0, &range, &proxyMapped)) && proxyMapped != nullptr)
        {
            std::vector<float> encoded;
            const float* p = (const float*) proxyMapped;

            for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
            {
                if (std::isfinite(p[i]) && p[i] >= 0.0f)
                    encoded.push_back(p[i]);
            }

            D3D12_RANGE none { 0, 0 };
            g_nr.diagProxyReadback->Unmap(0, &none);

            if (encoded.size() >= 64)
            {
                std::sort(encoded.begin(), encoded.end());
                double total = 0.0;
                size_t bright = 0;

                for (float v : encoded)
                {
                    total += v;
                    bright += v > 0.9f;
                }

                const auto at = [&](float q) { return encoded[(size_t) ((float) (encoded.size() - 1) * q)]; };
                proxyText = std::format("tile luma p05 {:.2f} p25 {:.2f} p50 {:.2f} p75 {:.2f} p95 {:.2f} max {:.2f}, mean {:.2f}, tiles above 0.9: {:.0f}%",
                                        at(0.05f), at(0.25f), at(0.5f), at(0.75f), at(0.95f), encoded.back(),
                                        (float) (total / (double) encoded.size()),
                                        100.0f * (float) bright / (float) encoded.size());
            }
        }
    }

    if (FAILED(gridBuffer->Map(0, &range, &gridMapped)) || gridMapped == nullptr)
        return;

    if (FAILED(exposureBuffer->Map(0, &range, &exposureMapped)) || exposureMapped == nullptr)
    {
        D3D12_RANGE nothingWritten { 0, 0 };
        gridBuffer->Unmap(0, &nothingWritten);
        return;
    }

    const float preExposure = std::isfinite(g_nr.diagPreExposure) && g_nr.diagPreExposure > 1e-6f
                                  ? g_nr.diagPreExposure
                                  : 1.0f;
    const float* grid = (const float*) gridMapped;
    const float gameExposure = ((const float*) exposureMapped)[0];

    std::vector<float> scene;
    scene.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(grid[i]) && grid[i] >= 0.0f)
            scene.push_back(grid[i] / preExposure);
    }

    const bool haveExposure = g_nr.diagExposureSupplied && std::isfinite(gameExposure) && gameExposure > 0.0f;
    D3D12_RANGE nothingWritten { 0, 0 };
    gridBuffer->Unmap(0, &nothingWritten);
    exposureBuffer->Unmap(0, &nothingWritten);

    if (scene.size() < 64)
        return;

    std::sort(scene.begin(), scene.end());

    const auto percentile = [&](float p) { return scene[(size_t) ((float) (scene.size() - 1) * p)]; };

    double sum = 0.0;
    double logSum = 0.0;
    size_t above1 = 0, above10 = 0, above100 = 0;

    for (float v : scene)
    {
        sum += v;
        logSum += std::log(std::max(v, 1e-8f));
        above1 += v > 1.0f;
        above10 += v > 10.0f;
        above100 += v > 100.0f;
    }

    const float count = (float) scene.size();
    const float mean = (float) (sum / count);
    const float logAverage = (float) std::exp(logSum / count);

    // The Automatic meter's black-tile rule (dlssnr.hlsl, gMode 13): 12 stops below the mean of the tiles at or
    // below 16x the plain mean (the sun and lamps left out of the reference), or the plain mean when none are.
    double coreSum = 0.0;
    size_t coreCount = 0;

    for (float v : scene)
        if (v <= mean * 16.0f)
            coreSum += v, ++coreCount;

    const float blackLevel = (coreCount > 0 ? (float) (coreSum / (double) coreCount) : mean) * std::exp2(-12.0f);
    const float blackShare =
        100.0f * (float) std::count_if(scene.begin(), scene.end(), [&](float v) { return v <= blackLevel; }) / count;
    const float p50 = percentile(0.50f);
    const float p95 = percentile(0.95f);
    const float safeWhite = std::max(whitePoint, 1e-6f);

    std::string exposureText = "none supplied";

    if (haveExposure)
        exposureText = std::format("{:.5g} (white point it would give: {:.4g})", gameExposure, preExposure / gameExposure);

    std::string autoText = "n/a";

    if (source == 3 && g_nr.autoExposureValue > 1e-8f)
        autoText = std::format("{:.5g} (white point it gives: {:.4g}){}{}", g_nr.autoExposureValue,
                               g_nr.autoExposurePreExposure / g_nr.autoExposureValue,
                               g_nr.autoExposureAdapting
                                   ? std::format(", eye adaptation {:.1f} s brighter / {:.1f} s darker, meter reading {:.5g}",
                                                 DlssNrExposureAdapt::Seconds(
                                                     Config::Instance()->DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(),
                                                     DlssNrExposureAdapt::kDefaultBrighterSeconds),
                                                 DlssNrExposureAdapt::Seconds(
                                                     Config::Instance()->DlssNrAutoExposureAdaptDarkerSeconds.value_or_default()),
                                                 g_nr.autoExposureRawValue)
                                   : std::string(),
                               g_nr.followingGame
                                   ? std::format(", following the game's exposure (calibration {:+.2f} EV)",
                                                 DlssNrFollowGame::Instance().OffsetEv())
                               : DlssNrFollowGame::Instance().Locked()
                                   ? std::format(", not following (calibration {:+.2f} EV)",
                                                 DlssNrFollowGame::Instance().OffsetEv())
                                   : std::string());

    LOG_INFO("DLSS-NR frame stats: {} ({}) {}x{}, source {}, pre-exposure {:.5g}, game exposure {}, "
             "auto exposure {}, white point {:.4g}; tile luma in scene units: min {:.3g} p05 {:.3g} p25 {:.3g} "
             "p50 {:.3g} p75 {:.3g} p95 {:.3g} p99 {:.3g} max {:.3g}, mean {:.3g}, log-average {:.3g}; tiles "
             "above 1: {:.0f}%, above 10: {:.0f}%, above 100: {:.0f}%, black (left out of Automatic): {:.0f}%; proxy sRGB at p50 {:.2f}, at "
             "log-average {:.2f}, at mean {:.2f}, at p95 {:.2f}; MEASURED PROXY (sRGB-encoded, what the model is shown{}): {}",
             DiagFormatName(g_nr.diagFormat), (int) g_nr.diagFormat, g_nr.diagWidth, g_nr.diagHeight, source,
             preExposure, exposureText, autoText, whitePoint, scene.front(), percentile(0.05f),
             percentile(0.25f), p50, percentile(0.75f), p95, percentile(0.99f), scene.back(), mean,
             logAverage, 100.0f * (float) above1 / count, 100.0f * (float) above10 / count,
             100.0f * (float) above100 / count, blackShare, DiagSrgbEncode(p50 / safeWhite),
             DiagSrgbEncode(logAverage / safeWhite), DiagSrgbEncode(mean / safeWhite),
             DiagSrgbEncode(p95 / safeWhite), g_nr.diagPassthrough ? ", passthrough: frame handed over untouched" : "", proxyText);
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough  ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, 3 * sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureKind[slot] == 1u && std::isfinite(src[0]) && src[0] > 0.0f)
    {
        g_nr.gameExposure = src[0];
    }
    else if (g_nr.meterExposureKind[slot] == 2u && std::isfinite(src[0]) && src[0] > 0.0f)
    {
        g_nr.autoExposureValue = src[0];
        g_nr.autoExposurePreExposure = g_nr.meterExposurePreExposure[slot];
        g_nr.autoExposureRawValue = g_nr.meterHasRaw[slot] && std::isfinite(src[2]) ? src[2] : src[0];

        DlssNr::ReportAutoExposureDefaults();

        // The game's exposure from the same frame, when it supplied one. Learned against only while following the
        // game's exposure (DlssNr_GameDefaults.h).
        if (g_nr.meterPairHasGame[slot] && std::isfinite(src[1]) && src[1] > 0.0f)
        {
            g_nr.autoPairGameExposure = src[1];
            g_nr.autoPairPreExposure = g_nr.meterExposurePreExposure[slot];

            // Against the meter's own reading of that frame, not eye adaptation's eased value: the pair is one frame.
            const float autoReading = g_nr.meterHasRaw[slot] && std::isfinite(src[2]) && src[2] > 0.0f
                                          ? src[2]
                                          : g_nr.autoExposureValue;

            if (DlssNr::FollowGameOn(*Config::Instance()) &&
                DlssNrFollowGame::Instance().Feed(g_nr.autoExposurePreExposure / autoReading,
                                                  g_nr.autoPairPreExposure / g_nr.autoPairGameExposure))
                LOG_INFO("DLSS-NR automatic exposure: calibrated against the game's own exposure: {:+.2f} EV "
                         "(Automatic's base white point is {:.3g}x the game's); follows the game's exposure from here "
                         "while AutoExposureFollowGame is on",
                         DlssNrFollowGame::Instance().OffsetEv(), DlssNrFollowGame::Instance().Scale());
            else if (DlssNr::FollowGameOn(*Config::Instance()))
                DlssNr::SayFollowTrack(DlssNrFollowGame::Instance().Track(
                    g_nr.autoExposurePreExposure / autoReading, g_nr.autoPairPreExposure / g_nr.autoPairGameExposure,
                    GetTickCount64(), DlssNrExposureCalibrate::HoldsFollow(DlssNrExposureCalibrate::TheRun(), GetTickCount64())));
        }
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;
    g_nr.autoExposureValue = 0.0f;
    g_nr.autoExposurePreExposure = 1.0f;
    g_nr.autoExposureRawValue = 0.0f;
    g_nr.autoExposureAdapter.Invalidate();
    g_nr.autoPairGameExposure = 0.0f;
    g_nr.autoPairPreExposure = 1.0f;

    for (uint32_t& kind : g_nr.meterExposureKind)
        kind = 0u;

    for (bool& pair : g_nr.meterPairHasGame)
        pair = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
}

// Turns what the meter saw into the divisor the encode uses, or falls back to the slider. Nothing here
// eases: the game's exposure is taken as it comes, and Automatic's easing is eye adaptation's, on the
// GPU (DlssNr_ExposureAdapt.h), with a snap on the game's own cut.
float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const float slider = cfg.DlssNrWhitePointScale.value_or_default();

    // A frame the game already tone mapped is display-referred: white is at 1 by definition and there
    // is nothing to measure. The encode passes such a frame through with no white point at all
    // (dlssnr.hlsl's passthrough); the slider's value only scales the debug views there (DebugScale).
    if (!isHdrBuffer)
        return slider;

    // The game's own exposure, where it supplies one.
    //
    // Exposure is the step that makes a cave and a field comparable: the renderer works in arbitrary
    // scene-referred units and multiplies by this before tone mapping, which is precisely why one
    // fixed paper white cannot serve both. FSR spells the relationship out -- frame / preExposure *
    // exposure -- so undoing it gives the divisor this pass wants, and paper white becomes a constant
    // on top rather than a value chasing the scene.
    //
    // Unlike anything measured off the frame this cannot be moved by what the pass writes, which is
    // what killed the statistical meter. It is the game's number, decided upstream.
    //
    // Held across the frames where the texture is absent -- GTA V dropped it three times in one
    // session -- because falling back to a default on those frames is a flicker, not a fallback.
    // The scan's anchor, where the game supplies no exposure of its own.
    //
    // Only ratios are used, so the units of the buffer never have to be known -- which is the whole
    // reason this is anchored rather than absolute. The anchor is the user's own white point at the
    // moment they pressed the button; everything after that is the scan moving it.
    //
    // Deliberately below the exposure texture in priority and mutually exclusive with it in the
    // menu. A game that hands over a real exposure has no business being driven by a buffer found by
    // its shape, and two sources fighting over one number is the class of bug worth making
    // unreachable rather than merely unlikely.
    if (DlssNr::WhitePointSourceEffective(cfg) == 2)
    {
        // Multi-point: one or more calibration points the user placed, interpolated in log space by
        // the current scan value. One point is the original ratio law; more fit the buffer's actual
        // relationship so the white point holds across the whole range, not only near one anchor.
        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
            DlssNr::ExposureScan::BestValue(), cfg.DlssNrScanInverted.value_or_default(),
            cfg.DlssNrScanTrim.value_or_default());

        if (w > 0.0f)
            return w;
    }

    if (DlssNr::WhitePointSourceEffective(cfg) == 1 && g_nr.gameExposure > 1e-6f)
    {
        // Its own setting, not the manual divisor. See Config: they are different quantities with
        // different units and different sensible ranges, and sharing one value meant adjusting the
        // trim destroyed the divisor somebody had found by hand.
        //
        // Still bounded at the point of use rather than only in the menu that draws it.
        //
        // Bounding it at the slider would have been cosmetic: someone who found 64 by hand on the
        // manual path and then switched the exposure source on keeps that 64 in their ini, and the
        // composition would go on reading it until they happened to touch the control. The picture
        // would be wrong for a reason the menu was no longer showing.
        //
        // Their value is left in the config untouched, so switching back to manual restores the
        // number they arrived at. It is only what this path consumes that is limited.
        //
        // The Trim is the slider, or interpolated from the Trim anchors at this base white point when
        // there are any. See DlssNr_TrimAnchors.h.
        //
        // What the Trim multiplies follows the scale (DlssNr_GameScale.h), but the points stay keyed by the game's
        // exposure, the scene's brightness, in both scales.
        const float baseWhitePoint = DlssNrGameScale::WhiteBase(DlssNr::GameExposureScaleEffective(cfg),
                                                                g_nr.gamePreExposure, g_nr.gameExposure);
        const float anchorKey = DlssNrGameScale::AnchorKey(g_nr.gamePreExposure, g_nr.gameExposure);
        const auto anchors = DlssNrTrim::Parse(cfg.DlssNrGameExposureTrimAnchors.value_or_default());
        const float trim = DlssNrTrim::TrimForKey(anchorKey, cfg.DlssNrWhitePointTrim.value_or_default(), anchors,
                                                  cfg.DlssNrGameExposureTrimPreview.value_or_default());

        return std::clamp(baseWhitePoint * trim, 0.01f, 4096.0f);
    }

    // Automatic exposure. The shader recomputes this from the live 1x1 texture every frame; this is the
    // value it falls back to, and what the menu shows, from the readback three frames behind.
    //
    // Measured off the frame the encode is about to read -- the upscaler's fresh output -- and never
    // off anything this pass has written. That is the difference from the statistical meter removed
    // below, which read its own output and chased it.
    // A found exposure in force counts on its own: Automatic's reading starts over on a change of source, and the
    // white point must not fall to the slider for the frames that takes (DlssNr_ExposureFoundBlend.h).
    if (DlssNr::WhitePointSourceEffective(cfg) == 3 && (g_nr.autoExposureValue > 1e-8f || g_nr.foundBaseWhitePoint > 0.0f))
    {
        // Following the game (DlssNr_FollowGame.h): the game's base white point times the learned calibration, which
        // is where Automatic's own would sit. The shader does the same from the game's live texture.
        const float baseWhitePoint = AutoBaseWhitePoint();
        const auto anchors = DlssNrTrim::Parse(cfg.DlssNrAutoExposureTrimAnchors.value_or_default());
        const float trim = DlssNrTrim::TrimForKey(baseWhitePoint, DlssNr::AutoTrimEffective(cfg),
                                                  anchors, cfg.DlssNrAutoExposureTrimPreview.value_or_default());

        return std::clamp(baseWhitePoint * trim, 0.01f, 4096.0f);
    }

    // Otherwise the slider, and only the slider.
    //
    // Measuring white from the frame was tried and removed. It could not be made to work because the
    // pass writes the frame it measures: in Enshrouded one session walked the divisor from 0.010 to
    // 97.910, and toggling NR at a fixed spot read 41.31 off and 0.46 on. Two attempts to damp it --
    // a relative lit threshold, then a rate limit with a cut snap -- both treated a coupled system as
    // a noisy one and neither held. A constant cannot do that, which is the whole argument for it,
    // and is what RenoDX has always done.
    return slider;
}

// The exposure fields of an encode or resolve dispatch, for the source in force. The shader uses them
// when it recomputes the white point from a live exposure texture, and ignores them otherwise.
void FillExposureConstants(DlssNrConstants& params, const Config& cfg, uint32_t source, float preExposure)
{
    const bool automatic = source == 3;
    const auto anchors = DlssNrTrim::Parse(automatic ? cfg.DlssNrAutoExposureTrimAnchors.value_or_default()
                                                     : cfg.DlssNrGameExposureTrimAnchors.value_or_default());

    params.PreExposure = preExposure;
    DlssNrTrim::FillConstants(params,
                              automatic ? DlssNr::AutoTrimEffective(cfg)
                                        : cfg.DlssNrWhitePointTrim.value_or_default(),
                              anchors,
                              automatic ? cfg.DlssNrAutoExposureTrimPreview.value_or_default()
                                        : cfg.DlssNrGameExposureTrimPreview.value_or_default(),
                              cfg.DlssNrAutoExposureShadowProtection.value_or_default());
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                              unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

float SuperiorDepthPrecision(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return 2.f / 65535.f;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return 2.f / 16777215.f;
    default:
        return 0.f;
    }
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* source, ID3D12Resource** clone)
{
    if (source == nullptr || !IsTypeless(source->GetDesc().Format))
        return source;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}",
                  (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

// The frame's colour encoding, from [DlssNr] ColourEncoding (DlssNr_ColourEncoding.h), for the evaluate and deferred-SR
// paths; Finished Picture decides from the screen instead. format is the colour authority's (the output's), the same
// buffer Auto has always judged. report is false where another path owns the menu line.
void ApplyColourEncoding(DlssNrFrameInfo& frame, uint32_t setting, bool gameSaysHdr, DXGI_FORMAT format, bool report)
{
    const auto choice = DlssNrColourEncoding::Resolve(setting, gameSaysHdr, FormatCanHoldLinearHdr(format));
    frame.ColourIsLinearHdr = choice.LinearHdr();
    frame.InputEncoding = DlssNrColourEncoding::ShaderConversion(choice.encoding);
    frame.ColourEncoding = (uint32_t) choice.encoding;
    frame.ColourEncodingForced = !choice.automatic;
    // Forced PQ is display light: its white is the game's HDR paper white (the decode puts 203 nits at 1.0), not the
    // game's exposure (which describes the scene) or Automatic's meter.
    if (choice.encoding == DlssNrColourEncoding::Encoding::Pq)
        frame.WhitePointOverride = DlssNrExposureState::PinnedWhitePoint(
            Config::Instance()->DlssNrDisplayPaperWhiteNits.value_or_default(),
            DlssNrExposureState::kNitsPerPqDecodedUnit);
    if (report)
        DlssNr::ReportColourEncoding(choice, DiagFormatName(format), "", false);
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The network pools its input 2x2 and runs its 8x8 attention windows on the result, so its grid is 16
// input pixels per window. A size that is not a multiple of 16 leaves a ragged last window that is
// zero-padded (the model copes, it is just wasted work at the border), and, more usefully here, every
// distinct size is a different feature: Auto's continuous render:output ratio moved the size by a pixel
// or two under dynamic resolution and each move rebuilt the model. Rounding to 16 lets the size hold.
//
// Only a size we are already resampling to is rounded. A native-size pass (WorkingScale 1.0, or a scale
// that rounds back to native) is left alone: rounding 1080 to 1088 would turn a 1:1 pass into a
// resample of the frame, which is worse than a ragged border window.
//
// The rule lives in DlssNr_Spatial.h, which Compress screen edges sizes its packed picture by too.
unsigned int AlignWorkSize(unsigned int size, unsigned int native)
{
    return DlssNr::Spatial::AlignedExtent(size, native);
}

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer,
                   uiWidth, uiHeight, bbWidth, bbHeight);
}


bool TuningMatchesFeature(const Config& cfg, unsigned int requestedPasses)
{
    for (unsigned int pass = 0; pass < requestedPasses; ++pass)
    {
        // A profile cannot be stale until its feature exists. This lets a user prepare pass 2 or 3
        // while running fewer layers without needlessly rebuilding pass 1.
        if (pass > 0 && g_nr.passFeature[pass] == nullptr)
            continue;

        if (g_nr.builtPassTuning[pass] != PassTuning(cfg, pass) ||
            g_nr.builtPreset[pass] != PassPreset(cfg, pass) ||
            g_nr.builtStyle[pass] != PassStyle(cfg, pass))
            return false;
    }

    return true;
}

void RecordBuiltPrimaryTuning(const Config& cfg)
{
    g_nr.builtPassTuning[0] = PassTuning(cfg, 0);
    g_nr.builtPreset[0] = PassPreset(cfg, 0);
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle[0] = PassStyle(cfg, 0);
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::recursive_mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

#include "DlssNr_DetailReuse.inl"
#include "DlssNr_EditCache.inl"
#include "DlssNr_SceneCut.inl"
#include "DlssNr_Spatial.inl"

} // namespace

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

#include "superior/Pipeline.inl"

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Six inputs (the sixth is read by the detail reuse pass only), two outputs, one constant buffer, and a clamped
    // linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                               ID3D12Resource* InMotion, ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                               ID3D12Resource* OutKeep, ID3D12Resource* InLow, ID3D12PipelineState* overridePipeline)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
        InLow != nullptr ? InLow : InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(overridePipeline ? overridePipeline : _pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has. Automatic exposure's meter is the one exception: its
    // shader spends a whole 8x8 thread group on each tile, so it dispatches one group per tile.
    const bool parallelExposureMeter =
        InConstants.Mode == DlssNrMode_Meter && InConstants.MeterCopiesExposure == 0;
    const UINT dispatchWidth =
        parallelExposureMeter ? InConstants.Width : (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight =
        parallelExposureMeter ? InConstants.Height : (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

#include "superior/Shader.inl"
#include "superior/PrivateGuides.inl"

DlssNr_Dx12::~DlssNr_Dx12()
{
    for (auto& buffer : _superiorConstantBuffers)
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    if (_superiorRawLow)
        _superiorRawLow->Release();
    if (_superiorWarpCache)
        _superiorWarpCache->Release();
    for (auto* pipeline : _superiorNormalResolve)
        if (pipeline)
            pipeline->Release();
    for (auto* pipeline : _superiorResolve)
        if (pipeline)
            pipeline->Release();
    for (auto* pipeline : _superiorPipeline)
        if (pipeline)
            pipeline->Release();
    if (_finishedColorPipelineState)
        _finishedColorPipelineState->Release();
    if (_detailStatsPipelineState)
        _detailStatsPipelineState->Release();
    if (_exposureAdaptPipelineState)
        _exposureAdaptPipelineState->Release();
    if (_detailReusePipelineState)
        _detailReusePipelineState->Release();
    if (_editCachePipelineState)
        _editCachePipelineState->Release();
    if (_spatialPipelineState)
        _spatialPipelineState->Release();
    if (_spatialGuidesPipelineState)
        _spatialGuidesPipelineState->Release();
    if (_privateGuidesHeap)
        _privateGuidesHeap->Release();
    for (auto* pipeline : _privateGuidesPipeline)
        if (pipeline)
            pipeline->Release();
    if (_privateGuidesRoot)
        _privateGuidesRoot->Release();
    for (auto* pipeline : _superiorSpatialGuides)
        if (pipeline)
            pipeline->Release();
    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }

    if (_lutPipelineState)
        _lutPipelineState->Release();
    if (_lutRootSignature)
        _lutRootSignature->Release();
    if (_lutTexture)
        _lutTexture->Release();
    for (auto& buffer : _lutConstantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }
}

bool DlssNr_Dx12::LutPipelineReady()
{
    if (_lutPipelineState != nullptr || _lutPipelineFailed || !_init)
        return _lutPipelineState != nullptr;

    // Its own root signature: t0 input, t1 the LUT as a 3D texture, t2 the encode's live exposure (dlssnr_lut.hlsl
    // recomputes the white point from it as the encode does), u0 output, b0 constants, plus a static
    // linear-clamp sampler at s0 (the same filter/address choice as this class's own sampler, just not that
    // root signature: this is the first pass here to sample a 3D texture, and folding it into the shared
    // table would mean touching every other Dispatch* call site's stand-in array for a slot only this
    // shader reads).
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_DESCRIPTOR_RANGE1 ranges[3] = {
        CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0), // t0, t1, t2
        CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0), // u0
        CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0), // b0
    };

    CD3DX12_ROOT_PARAMETER1 rootParameter {};
    rootParameter.InitAsDescriptorTable(_countof(ranges), ranges);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc {};
    rootSigDesc.Init_1_1(1, &rootParameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
    if (SUCCEEDED(hr))
        hr = _device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&_lutRootSignature));

    if (FAILED(hr) || _lutRootSignature == nullptr)
    {
        _lutPipelineFailed = true;
        LOG_WARN("DLSS-NR: the LUT pass's root signature could not be built; LutFile is ignored");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = _lutRootSignature;
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(dlssnr_lut_cso, sizeof(dlssnr_lut_cso));
    hr = _device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_lutPipelineState));

    if (FAILED(hr) || _lutPipelineState == nullptr)
    {
        _lutPipelineFailed = true;
        LOG_WARN("DLSS-NR: the LUT pass could not be built; LutFile is ignored");
        return false;
    }

    for (uint32_t i = 0; i < kLutHeapCount; ++i)
    {
        if (!_lutHeaps[i].Initialize(_device, 3, 1, 1)) // 3 SRV (t0, t1, t2), 1 UAV (u0), 1 CBV (b0)
        {
            _lutPipelineFailed = true;
            LOG_WARN("DLSS-NR: the LUT pass's descriptor heaps could not be built; LutFile is ignored");
            return false;
        }

        const D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrLutConstants));
        const auto cbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(_device->CreateCommittedResource(&cbHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&_lutConstantBuffers[i]))))
        {
            _lutPipelineFailed = true;
            LOG_WARN("DLSS-NR: the LUT pass's constant buffers could not be built; LutFile is ignored");
            return false;
        }
    }

    return true;
}

void DlssNr_Dx12::ReleaseLutTexture()
{
    if (_lutTexture != nullptr)
        ParkNrResource(_lutTexture);
    _lutTextureSize = 0;
    _lutTextureSourcePath.clear();
}

bool DlssNr_Dx12::EnsureLutTexture(ID3D12GraphicsCommandList* InCmdList)
{
    if (!_lutState.Loaded() || InCmdList == nullptr || _device == nullptr)
        return false;

    const int size = _lutState.lut.size;

    // Keyed on the loaded path, not the lattice size: two different .cube files sharing a size (17/33/65
    // are near-universal) must not read as "nothing changed" just because neither resized the texture --
    // that was the bug (Review Pass, 2026-10-04): switching between two same-size LUTs silently kept
    // sampling whichever uploaded first.
    if (_lutTexture != nullptr && _lutTextureSize == size && _lutTextureSourcePath == _lutState.loadedPath)
        return true; // already uploaded, and it is still this exact file

    if (_lutTexture != nullptr)
        ParkNrResource(_lutTexture);
    _lutTextureSize = 0;
    _lutTextureSourcePath.clear();

    D3D12_HEAP_PROPERTIES heapProps {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    texDesc.Width = (UINT64) size;
    texDesc.Height = (UINT) size;
    texDesc.DepthOrArraySize = (UINT16) size;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = _device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_lutTexture));
    if (FAILED(hr) || _lutTexture == nullptr)
    {
        LOG_ERROR("DLSS-NR: the LUT texture could not be allocated ({0}x{0}x{0})", size);
        return false;
    }

    // Pack the parsed lattice (red-fastest, matching .cube's own order, which is already x-fastest-then-y-
    // then-z -- exactly a 3D texture's own row-major layout) into half4, honouring the destination's row
    // pitch -- GetCopyableFootprints is the only correct source for it, rather than assuming size*8 bytes is
    // already 256-byte aligned.
    UINT64 totalBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    _device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeapProps {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);

    ID3D12Resource* uploadBuffer = nullptr;
    hr = _device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr) || uploadBuffer == nullptr)
    {
        LOG_ERROR("DLSS-NR: the LUT texture's upload buffer could not be allocated");
        ParkNrResource(_lutTexture);
        return false;
    }

    uint8_t* mapped = nullptr;
    const CD3DX12_RANGE readRange(0, 0);
    if (FAILED(uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
    {
        LOG_ERROR("DLSS-NR: the LUT texture's upload buffer could not be mapped");
        uploadBuffer->Release();
        ParkNrResource(_lutTexture);
        return false;
    }

    DlssNrLutPack::PackHalf4(_lutState.lut.rgb.data(), size, mapped, (size_t) footprint.Footprint.RowPitch,
                             (size_t) footprint.Footprint.RowPitch * (size_t) size);

    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = _lutTexture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = uploadBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    InCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(InCmdList, _lutTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The copy above is only recorded, not yet executed -- the upload buffer must outlive it, which
    // ParkNrResource's deferred release (rather than an immediate one here) already guarantees for every
    // other scratch resource in this file.
    ParkNrResource(uploadBuffer);

    _lutTextureSize = size;
    _lutTextureSourcePath = _lutState.loadedPath;
    return true;
}

bool DlssNr_Dx12::DispatchLut(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InSource, ID3D12Resource* OutTarget,
                              unsigned int Width, unsigned int Height, float Strength, uint32_t InputEncoding,
                              bool ColourIsLinearHdr, const DlssNrConstants& White, ID3D12Resource* ExposureTex,
                              const std::string& LutPath)
{
    DlssNr_LutEnsureParsed(&_lutState, LutPath);

    if (!_lutState.Loaded())
        return false;

    if (InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr || Width == 0 ||
        Height == 0)
        return false;

    if (!LutPipelineReady())
        return false;

    if (!EnsureLutTexture(InCmdList))
        return false;

    const uint32_t slot = _lutHeapIndex;
    _lutHeapIndex = (_lutHeapIndex + 1) % kLutHeapCount;

    FrameDescriptorHeap& heap = _lutHeaps[slot];

    CreateShaderResourceView(_device, InSource, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, _lutTexture, heap.GetSrvCPU(1));
    // The source stands in when there is no live exposure; the constants then say not to read it.
    CreateShaderResourceView(_device, ExposureTex != nullptr ? ExposureTex : InSource, heap.GetSrvCPU(2));
    CreateUnorderedAccessView(_device, OutTarget, heap.GetUavCPU(0), 0);

    DlssNrLutConstants constants {};
    constants.Width = Width;
    constants.Height = Height;
    constants.Strength = Strength;
    constants.DomainMinR = _lutState.lut.domainMin[0];
    constants.DomainMinG = _lutState.lut.domainMin[1];
    constants.DomainMinB = _lutState.lut.domainMin[2];
    constants.DomainMaxR = _lutState.lut.domainMax[0];
    constants.DomainMaxG = _lutState.lut.domainMax[1];
    constants.DomainMaxB = _lutState.lut.domainMax[2];
    constants.LutSize = (uint32_t) _lutState.lut.size;
    constants.InputEncoding = InputEncoding;
    constants.ColourIsLinearHdr = ColourIsLinearHdr ? 1u : 0u;
    CopyLiveWhitePoint(constants, White);
    if (ExposureTex == nullptr)
        constants.UseGameExposure = constants.UseExposureWhitePoint = 0u;

    if (!CreateConstantsBuffer(_device, _lutConstantBuffers[slot], constants, heap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create the LUT pass's constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_lutRootSignature);
    InCmdList->SetPipelineState(_lutPipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);

    return true;
}

bool DlssNr_Dx12::DispatchDetailStats(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                      ID3D12Resource* InOutput, ID3D12Resource* InPrevOutput, ID3D12Resource* InInput,
                                      ID3D12Resource* InPrevInput, ID3D12Resource* InProxy, ID3D12Resource* OutGrid)
{
    if (!_detailStatsPipelineState && _init)
        CreateComputePipeline(_device, &_detailStatsPipelineState, dlssnr_detail_stats_cso,
                              sizeof(dlssnr_detail_stats_cso), nullptr);

    if (!_init || _detailStatsPipelineState == nullptr || InCmdList == nullptr || _device == nullptr ||
        InOutput == nullptr || InPrevOutput == nullptr || InInput == nullptr || InPrevInput == nullptr ||
        InProxy == nullptr || OutGrid == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    ID3D12Resource* const srvs[kSrvCount] = { InOutput, InPrevOutput, InInput, InPrevInput, InProxy, InOutput };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutGrid, OutGrid };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_detailStatsPipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // One 8x8 group per tile of the 64x64 grid, whatever the frame size.
    InCmdList->Dispatch(64, 64, 1);

    return true;
}

bool DlssNr_Dx12::ExposureAdaptReady()
{
    if (!_exposureAdaptPipelineState && !_exposureAdaptPipelineFailed && _init)
    {
        CreateComputePipeline(_device, &_exposureAdaptPipelineState, dlssnr_exposure_adapt_cso,
                              sizeof(dlssnr_exposure_adapt_cso), nullptr);

        if (_exposureAdaptPipelineState == nullptr)
        {
            _exposureAdaptPipelineFailed = true;
            LOG_WARN("DLSS-NR: the eye adaptation pass could not be built; Automatic follows every frame at once");
        }
    }

    return _init && _exposureAdaptPipelineState != nullptr;
}

bool DlssNr_Dx12::DispatchExposureAdapt(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                        ID3D12Resource* InReading, ID3D12Resource* OutEased)
{
    if (!ExposureAdaptReady() || InCmdList == nullptr || _device == nullptr || InReading == nullptr ||
        OutEased == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // The shader reads t0 and writes u0; the rest of the table gets the same resources so nothing is left unbound.
    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, InReading, currentHeap.GetSrvCPU(i));

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, OutEased, currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_exposureAdaptPipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    InCmdList->Dispatch(1, 1, 1);

    return true;
}

bool DlssNr_Dx12::DetailReuseReady()
{
    if (!_detailReusePipelineState && !_detailReusePipelineFailed && _init)
    {
        CreateComputePipeline(_device, &_detailReusePipelineState, dlssnr_detail_reuse_cso,
                              sizeof(dlssnr_detail_reuse_cso), nullptr);

        if (_detailReusePipelineState == nullptr)
        {
            _detailReusePipelineFailed = true;
            LOG_WARN("DLSS-NR: the detail reuse pass could not be built; every frame runs the model");
        }
    }

    return _init && _detailReusePipelineState != nullptr;
}

bool DlssNr_Dx12::DispatchDetailReuse(ID3D12GraphicsCommandList* InCmdList,
                                      const DlssNrDetailReuseConstants& InConstants, unsigned int Width,
                                      unsigned int Height, ID3D12Resource* In0, ID3D12Resource* In1,
                                      ID3D12Resource* In2, ID3D12Resource* In3, ID3D12Resource* In4,
                                      ID3D12Resource* OutTarget, ID3D12Resource* OutSecond,
                                      ID3D12Resource* InHistoryDistrust)
{
    if (!DetailReuseReady() || InCmdList == nullptr || _device == nullptr || In0 == nullptr ||
        OutTarget == nullptr || Width == 0 || Height == 0)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    ID3D12Resource* const srvs[kSrvCount] = {
        In0,
        In1 != nullptr ? In1 : In0,
        In2 != nullptr ? In2 : In0,
        In3 != nullptr ? In3 : In0,
        In4 != nullptr ? In4 : In0,
        InHistoryDistrust != nullptr ? InHistoryDistrust : In0,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutTarget, OutSecond != nullptr ? OutSecond : OutTarget };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_detailReusePipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    InCmdList->Dispatch((Width + _numThreadsX - 1) / _numThreadsX, (Height + _numThreadsY - 1) / _numThreadsY, 1);

    return true;
}

bool DlssNr_Dx12::EditCacheReady()
{
    if (!_editCachePipelineState && !_editCachePipelineFailed && _init)
    {
        CreateComputePipeline(_device, &_editCachePipelineState, dlssnr_edit_cache_cso, sizeof(dlssnr_edit_cache_cso),
                              nullptr);

        if (_editCachePipelineState == nullptr)
        {
            _editCachePipelineFailed = true;
            LOG_WARN("DLSS-NR: the edit cache pass could not be built; every frame runs the model");
        }
    }

    return _init && _editCachePipelineState != nullptr;
}

bool DlssNr_Dx12::DispatchEditCache(ID3D12GraphicsCommandList* InCmdList, const DlssNrEditCacheConstants& InConstants,
                                    unsigned int Width, unsigned int Height, ID3D12Resource* const (&In)[kSrvCount],
                                    ID3D12Resource* OutTarget, ID3D12Resource* OutSecond)
{
    if (!EditCacheReady() || InCmdList == nullptr || _device == nullptr || In[0] == nullptr || OutTarget == nullptr ||
        Width == 0 || Height == 0)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, In[i] != nullptr ? In[i] : In[0], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutTarget, OutSecond != nullptr ? OutSecond : OutTarget };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_editCachePipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    InCmdList->Dispatch((Width + _numThreadsX - 1) / _numThreadsX, (Height + _numThreadsY - 1) / _numThreadsY, 1);

    return true;
}

bool DlssNr_Dx12::SpatialReady()
{
    if (!_spatialPipelineFailed && _init && (!_spatialPipelineState || !_spatialGuidesPipelineState))
    {
        if (!_spatialPipelineState)
            CreateComputePipeline(_device, &_spatialPipelineState, dlssnr_spatial_cso, sizeof(dlssnr_spatial_cso),
                                  nullptr);
        if (!_spatialGuidesPipelineState)
            CreateComputePipeline(_device, &_spatialGuidesPipelineState, dlssnr_spatial_guides_cso,
                                  sizeof(dlssnr_spatial_guides_cso), nullptr);

        if (!_spatialPipelineState || !_spatialGuidesPipelineState)
        {
            _spatialPipelineFailed = true;
            LOG_WARN("DLSS-NR: the Compress screen edges passes could not be built; NR runs on the whole picture");
        }
    }

    return _init && _spatialPipelineState != nullptr && _spatialGuidesPipelineState != nullptr;
}

bool DlssNr_Dx12::DispatchSpatial(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Spatial::Constants& InConstants,
                                  ID3D12Resource* In0, ID3D12Resource* In1, ID3D12Resource* In2, ID3D12Resource* Out0,
                                  ID3D12Resource* Out1, uint32_t motionMethod)
{
    const bool uniform = InConstants.mode == 104u;
    if ((!uniform && !SpatialReady()) || !_init || InCmdList == nullptr || _device == nullptr || Out0 == nullptr ||
        InConstants.width == 0 || InConstants.height == 0)
        return false;

    const bool guides = InConstants.mode == 101 || uniform;
    auto constants = InConstants;
    constants.unused = g_nr.guideDepthInverted ? 1.f : 0.f;
    if (guides && motionMethod == 2u && In1 && !constants.warp.reserved)
        DlssNr::Spatial::SetDepthPrecision(constants, SuperiorDepthPrecision(In1->GetDesc().Format));
    const bool joint = guides && motionMethod == 2u && constants.padding[0] > .5f;
    const auto minified =
        joint ? DlssNr::Spatial::PrepareJointMinification(constants) : DlssNr::Spatial::MinifiedMotionDispatch {};
    ID3D12PipelineState* correction = nullptr;
    auto* pipeline = guides ? _spatialGuidesPipelineState : _spatialPipelineState;
    if (guides && motionMethod != 0u)
    {
        const unsigned primary =
            joint ? (uniform ? 4u : 2u) + (constants.warp.reserved ? 4u : 0u) : (uniform ? 1u : 0u);
        for (unsigned pass = 0; pass < (minified.pixels ? 2u : 1u); ++pass)
        {
            const unsigned index = pass ? primary + 1u : primary;
            if (!_superiorSpatialGuidesAttempted[index])
            {
                _superiorSpatialGuidesAttempted[index] = true;
                const void* blobs[] { host_surface_guides_cso,
                                      host_uniform_surface_guides_cso,
                                      host_joint_guides_cso,
                                      host_joint_minified_guides_cso,
                                      host_uniform_joint_guides_cso,
                                      host_uniform_joint_minified_guides_cso,
                                      host_joint_guides_quantized_cso,
                                      host_joint_minified_guides_quantized_cso,
                                      host_uniform_joint_guides_quantized_cso,
                                      host_uniform_joint_minified_guides_quantized_cso };
                const size_t sizes[] { sizeof(host_surface_guides_cso),
                                       sizeof(host_uniform_surface_guides_cso),
                                       sizeof(host_joint_guides_cso),
                                       sizeof(host_joint_minified_guides_cso),
                                       sizeof(host_uniform_joint_guides_cso),
                                       sizeof(host_uniform_joint_minified_guides_cso),
                                       sizeof(host_joint_guides_quantized_cso),
                                       sizeof(host_joint_minified_guides_quantized_cso),
                                       sizeof(host_uniform_joint_guides_quantized_cso),
                                       sizeof(host_uniform_joint_minified_guides_quantized_cso) };
                CreateComputePipeline(_device, &_superiorSpatialGuides[index], blobs[index], sizes[index], nullptr);
            }
            if (!_superiorSpatialGuides[index])
                return false;
            if (pass)
                correction = _superiorSpatialGuides[index];
        }
        pipeline = _superiorSpatialGuides[primary];
    }
    if (!pipeline)
        return false;
    ID3D12Resource* const first = In0 != nullptr ? In0 : In1;

    if (first == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // The shader reads t0..t2 only. The rest of the table gets t0 as a stand-in so no descriptor in it is unbound.
    ID3D12Resource* const srvs[kSrvCount] = {
        first,
        In1 != nullptr ? In1 : first,
        In2 != nullptr ? In2 : first,
        first,
        first,
        first,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { Out0, Out1 != nullptr ? Out1 : Out0 };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(pipeline);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    InCmdList->Dispatch((InConstants.width + _numThreadsX - 1) / _numThreadsX,
                        (InConstants.height + _numThreadsY - 1) / _numThreadsY, 1);
    if (correction)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = joint ? nullptr : Out1 != nullptr ? Out1 : Out0;
        InCmdList->ResourceBarrier(1, &barrier);
        InCmdList->SetPipelineState(correction);
        InCmdList->Dispatch(minified.groupsX, minified.groupsY, 1);
    }

    return true;
}

bool DlssNr_Dx12::DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList,
                                       const DlssNrConstants& InConstants, ID3D12Resource* InSource,
                                       ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                                       ID3D12Resource* InMotion, ID3D12Resource* OutTarget)
{
    if (!_finishedColorPipelineState && _init)
        CreateComputePipeline(_device, &_finishedColorPipelineState, dlssnr_finished_color_cso,
                              sizeof(dlssnr_finished_color_cso), nullptr);
    auto* pipeline = _finishedColorPipelineState;
    if (!_init || pipeline == nullptr || InCmdList == nullptr || _device == nullptr ||
        InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Same table shape as DispatchPass: the finished-colour shader reads t0..t3 + u0, and t4/t5/u1 get
    // the source as a stand-in so no descriptor in the table is left unbound.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InSource,
        InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutTarget, OutTarget };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(pipeline);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}



void DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                           ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
                           const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::recursive_mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();
    // Superior Scaling (superior/Pipeline.inl) runs only while selected and not given up on this session; otherwise the
    // ordinary path runs, with Matched residual in its place. Selecting it again retries.
    const bool superiorSelected = DlssNr::SuperiorScaling::Enabled(cfg);
    auto& superiorState = SuperiorRun::GetState();
    if (superiorState.wasSelected != superiorSelected)
    {
        superiorState.wasSelected = superiorSelected;
        superiorState.unavailable.clear();
    }
    const bool superior = superiorSelected && superiorState.unavailable.empty();
    if (superiorState.wasActive != superior)
    {
        superiorState.wasActive = superior;
        g_nr.reset = true;
        SuperiorRun::Drop();
    }

    // Depth is the one input the model can run without: NVIDIA's own retail DLL treats it as an optional
    // refinement of motion-vector dilation at object edges (it is null in every known capture, including
    // NVIDIA's own native integration), while colour, motion and output are unconditional. A frame with no
    // depth loses that edge refinement and depth-based reuse/trust discrimination, not NR itself.
    if (g_nr.failed || cmdList == nullptr || colour == nullptr || motion == nullptr || output == nullptr)
    {
        ReportSkipOnce(g_nr.failed ? "it already failed this session" : "a resource was missing");
        return;
    }

    ID3D12Resource* target = output;

    // Feature creation records GPU work too, and may return before the first evaluate.
    // Guard the entire dispatch, not just the colour passes at the bottom. Otherwise
    // creation/resize during an RE Engine loading screen captures NR's bindings as
    // the game's state, or returns with those bindings still active.
    const bool restoreRequired = cfg.RestoreComputeSignature.value_or_default() ||
                                 cfg.RestoreGraphicSignature.value_or_default();
    if (restoreRequired && !frame.IndependentCommands && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");
        return;
    }
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
    // readable. Track every transition so both paths return the resource exactly as their caller gave
    // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
    const D3D12_RESOURCE_STATES outputArrival =
        frame.FinishedPicture ? (D3D12_RESOURCE_STATES) frame.OutputArrivalState : frame.BeforeUpscale
            ? (!frame.PrivateColorCopy && Config::Instance()->ColorResourceBarrier.has_value()
                   ? (D3D12_RESOURCE_STATES) Config::Instance()->ColorResourceBarrier.value()
                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            : Config::Instance()->OutputResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES targetState = outputArrival;
    const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
    {
        Barrier(cmdList, target, targetState, to);
        targetState = to;
    };

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    // Our surfaces' colour format: the game's, with BGRA8 made RGBA8 (DlssNr_SurfaceFormats.h). A game that swaps the
    // two keeps one set of surfaces, one model feature and its history. Only the surfaces copied straight to or from
    // `target` use desc.Format and check it themselves (activeColor, lutScratch, heldColor, hdrCopy without a UAV).
    const auto colourFormat = (DXGI_FORMAT) DlssNr::CanonicalColourFormat((int) desc.Format);
    {
        auto& lastGameFormat = g_nr.lastGameFormat;

        if (!g_nr.saidFormatSwap && lastGameFormat != DXGI_FORMAT_UNKNOWN && lastGameFormat != desc.Format &&
            DlssNr::SameColourFamily((int) lastGameFormat, (int) desc.Format))
        {
            g_nr.saidFormatSwap = true;
            LOG_INFO("DLSS-NR: the game's output changes between format {} and {} (RGBA8/BGRA8); the model and its "
                     "history are kept",
                     (int) lastGameFormat, (int) desc.Format);
        }

        lastGameFormat = desc.Format;
    }
    const auto active = frame.BeforeUpscale
        ? DlssNr::PreSrColorExtent(desc, frame.RenderSubrectWidth, frame.RenderSubrectHeight)
        : std::optional<DlssNr::ColorExtent> { DlssNr::ColorExtent { (unsigned int) desc.Width, desc.Height } };
    if (!active)
    {
        ReportSkipOnce("the pre-SR active colour size is invalid");
        device->Release();
        return;
    }
    const auto width = active->width;
    const auto height = active->height;
    const bool cropColor = frame.BeforeUpscale && (width != desc.Width || height != desc.Height);
    const bool targetSupportsUav =
        cropColor || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    // No depth this frame resolves to an empty depth region.
    const auto guideDesc = depth != nullptr ? depth->GetDesc() : D3D12_RESOURCE_DESC {};
    const auto motionDesc = motion->GetDesc();
    const auto guides = DlssNr::ResolveGuideRegions(
        { depth != nullptr ? (unsigned int) guideDesc.Width : 0u, depth != nullptr ? guideDesc.Height : 0u },
        { (unsigned int) motionDesc.Width, motionDesc.Height }, { frame.RenderSubrectWidth, frame.RenderSubrectHeight },
        { frame.OutputWidth, frame.OutputHeight }, frame.MotionVectorsLowResolution, frame.DepthSubrectBaseX,
        frame.DepthSubrectBaseY, frame.MotionSubrectBaseX, frame.MotionSubrectBaseY);
    if (!guides.motion.valid())
    {
        ReportSkipOnce("the motion-vector subrect is empty");
        device->Release();
        return;
    }
    // Depth with an empty subrect is no depth.
    if (!guides.depth.valid())
        depth = nullptr;
    const auto guideWidth = guides.depth.width, guideHeight = guides.depth.height;
    const auto motionWidth = guides.motion.width, motionHeight = guides.motion.height;
    const auto depthBaseX = guides.depth.x, depthBaseY = guides.depth.y;
    const auto motionBaseX = guides.motion.x, motionBaseY = guides.motion.y;

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    g_nr.guideDepthInverted = frame.DepthInverted;

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        g_nr.reset = true;

        static unsigned long long resets = 0;
        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    // A scene cut the game did not flag, found a few evaluates ago (DlssNr_SceneCut.inl; SceneCut=2 only).
    if (SceneCut::BeginFrame(cfg, frame.Reset))
        g_nr.reset = true;

    // Logged whenever it changes, not once per session.
    //
    // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
    // the output size is what the model is built for -- so a once-only line goes stale the moment the
    // player moves the quality slider, and every later line in the log is then read against numbers
    // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
    // and the log claimed 1920x1080 guides throughout.
    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
        DXGI_FORMAT depthFormat;
        DXGI_FORMAT motionFormat;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow { true,
                                  g_nr.guideDepthInverted,
                                  g_nr.guideMvScaleX,
                                  g_nr.guideMvScaleY,
                                  guideWidth,
                                  guideHeight,
                                  width,
                                  (unsigned int) height,
                                  depth != nullptr ? guideDesc.Format : DXGI_FORMAT_UNKNOWN,
                                  motionDesc.Format };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH ||
        loggedGuides.depthFormat != guidesNow.depthFormat || loggedGuides.motionFormat != guidesNow.motionFormat)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {} (format {}), motion vector scale {} x {} (format {}), guides {}x{} for a "
                 "{}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", (int) guidesNow.depthFormat,
                 g_nr.guideMvScaleX, g_nr.guideMvScaleY, (int) guidesNow.motionFormat, guideWidth, guideHeight,
                 width, height);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    // The port needs no capability block, so it also runs where the NGX core cannot start (AMD, Intel).
    if (!EnsureForwarder() || (!g_nr.isPort && !EnsureCapabilityParams(device)))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
    // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
    // working size so the model denoises a super-native input, which the resolve then samples back down.
    // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
    //
    // Auto: derived from the render:output ratio the upscaler itself already reconstructed detail at,
    // rather than the manual slider. This only makes sense once NR sees the upscaler's own output --
    // pre-SR, NR is looking at the render-resolution buffer directly and there is no ratio to derive
    // from -- so it is scoped to post-SR placement (RunBeforeSr off, or forced off by Ray
    // Reconstruction, which always runs NR after RR+SR). Width and height are averaged rather than
    // taking width alone, so an asymmetric render subrect (padding, non-uniform dynamic-resolution
    // scaling) still lands on a sane single scale for this model, which -- like the manual slider --
    // only ever works at one uniform percentage, not a separate X/Y one.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    if (!frame.BeforeUpscale && cfg.DlssNrModelResolutionAuto.value_or_default() &&
        frame.RenderSubrectWidth > 0 && frame.OutputWidth > 0 &&
        frame.RenderSubrectHeight > 0 && frame.OutputHeight > 0)
        workScale = 0.5f * ((float) frame.RenderSubrectWidth / (float) frame.OutputWidth +
                            (float) frame.RenderSubrectHeight / (float) frame.OutputHeight);
    if (!std::isfinite(workScale))
        workScale = 1.0f;
    workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
    // Superior keeps its exact raster (below), which would follow every pixel of a moving Auto scale and rebuild the
    // NR feature and its private DLSS each time; it moves only when the ordinary 16-pixel cell does.
    if (superior && !frame.BeforeUpscale && cfg.DlssNrModelResolutionAuto.value_or_default())
    {
        static DlssNr::SuperiorScaling::ScaleHold superiorScaleHold;
        workScale = superiorScaleHold.Hold(width, height,
                                           AlignWorkSize((unsigned int) (width * workScale + 0.5f), width),
                                           AlignWorkSize((unsigned int) (height * workScale + 0.5f), height), workScale);
    }
    g_nr.appliedWorkScale = workScale;
    const auto workWidth = superior ? DlssNr::SuperiorScaling::WorkingExtent(width, workScale)
                                    : AlignWorkSize((unsigned int) (width * workScale + 0.5f), width);
    const auto workHeight = superior ? DlssNr::SuperiorScaling::WorkingExtent(height, workScale)
                                     : AlignWorkSize((unsigned int) (height * workScale + 0.5f), height);
    const bool reduced = workWidth != width || workHeight != height;
    const unsigned int configuredPasses =
        std::clamp(cfg.DlssNrPasses.value_or_default(),
                   1u, cfg.DlssNrUnlockPasses.value_or_default() ? DlssNr::MaxPassCount
                                                               : DlssNr::DefaultMaxPassCount);
    const bool proxyBackend = cfg.DlssNrUseProxy.value_or_default();
    const unsigned int requestedPasses = proxyBackend ? 1u : configuredPasses;

    if (proxyBackend && configuredPasses > 1)
    {
        static bool warnedProxyPasses = false;
        if (!warnedProxyPasses)
        {
            warnedProxyPasses = true;
            LOG_WARN("DLSS-NR: the driver-proxy backend supports one pass; Passes={} is using 1",
                     configuredPasses);
        }
    }

    // Compress screen edges (DlssNr_Spatial.inl): whether the model works on a packed picture this frame, and at what
    // size. Everything about the model below (its features, its surfaces, the guides it is handed) follows the packed
    // size; the encode, the resolve and the frame stay at the ordinary ones.
    uint64_t spatialSignature = EdgeCompression::SignatureOf(
        colourFormat, depth != nullptr ? guideDesc.Format : DXGI_FORMAT_UNKNOWN,
        depth != nullptr ? (unsigned int) guideDesc.Width : 0u, depth != nullptr ? guideDesc.Height : 0u,
        motionDesc.Format, (unsigned int) motionDesc.Width, motionDesc.Height);

    // Superior supplies the current guide subrect/units on every evaluate. A
    // changing source allocation or render subrect does not change NR's colour history grid.
    if (superior)
        spatialSignature = DlssNr::Spatial::SuperiorSignature(
            colourFormat, depth ? guideDesc.Format : DXGI_FORMAT_UNKNOWN, motionDesc.Format);

    bool spatialReset = false;
    bool spatial =
        EdgeCompression::Begin(cfg, width, height, workScale, !superior, proxyBackend, spatialSignature, spatialReset);

    if (spatialReset)
        g_nr.reset = true;

    bool superiorPacked =
        superior && spatial &&
        DlssNr::SuperiorScaling::NeedsTemporal(width, height, workWidth, workHeight, true,
                                               EdgeCompression::Layout().modelW, EdgeCompression::Layout().modelH);
    // Exposure calibration alone still needs the original unpacked proxy. Normal Superior rendering bypasses it.
    const bool spatialNeedsUnpack = !superiorPacked || CalibrationWanted() || CalibrationActive();
    const DlssNr::GuideExtent renderGuideSize { frame.RenderSubrectWidth, frame.RenderSubrectHeight };
    const bool renderGuides = DlssNr::Spatial::UseRenderGuides(superior && workScale == 1.f, true, spatial, width,
                                                               height, workWidth, workHeight, renderGuideSize);
    const auto spatialGuideLayout = renderGuides
                                        ? DlssNr::Spatial::RenderGuideLayout(EdgeCompression::Layout(), renderGuideSize)
                                        : EdgeCompression::Layout();
    if (spatial && (!SpatialReady() ||
                    !EdgeCompression::Prepare(device, EdgeCompression::Layout(), workScale > 1.0f, spatialNeedsUnpack,
                                              { spatialGuideLayout.modelW, spatialGuideLayout.modelH }, renderGuides)))
    {
        EdgeCompression::TurnOff(DlssNr::Spatial::Status::TurnedOffResources);
        g_nr.reset = true;
        spatial = false;
        superiorPacked = false;
    }

    if (!spatial)
        EdgeCompression::Release();

    const unsigned int modelWidth = spatial ? EdgeCompression::Layout().modelW : workWidth;
    const unsigned int modelHeight = spatial ? EdgeCompression::Layout().modelH : workHeight;
    const DXGI_FORMAT modelFormat = spatial ? EdgeCompression::kFormat : colourFormat;

    // The Replace curves take the model's answer as the picture and never read the proxy, so the unpack's loss at the
    // edges would land on screen with nothing to cancel it. The unpack then adds it back, measured against the picture the
    // uncompressed path would have shown the model (mode 103), which is built below the way that path builds it.
    const bool replaceCorrection =
        spatial && !superiorPacked && DlssNrProxyCurve::IsReplace(cfg.DlssNrReversibleMode.value_or_default());

    ReleaseSurfacesIfFormatChanged(modelFormat, colourFormat);

    const bool resolutionChanged = g_nr.width != width || g_nr.height != height ||
                                   g_nr.workWidth != modelWidth || g_nr.workHeight != modelHeight;
    const bool placementChanged = g_nr.feature != nullptr &&
        (g_nr.beforeUpscale != frame.BeforeUpscale ||
         g_nr.rayReconstruction != frame.RayReconstruction);

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = !TuningMatchesFeature(cfg, requestedPasses);

    if (g_nr.feature != nullptr && (resolutionChanged || tuningChanged || placementChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(g_nr.feature);
        g_nr.featurePendingSubmission = false;

        for (unsigned int i = 1; i < DlssNr::MaxPassCount; ++i)
        {
            ParkNrFeature(g_nr.passFeature[i]);
            g_nr.passNeedsReset[i] = false;
            g_nr.passCreateFailed[i] = false;
            g_nr.passPendingSubmission[i] = false;
        }

        // Resolution and seam changes invalidate the scratch state. Tuning does not, and throwing
        // resources away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged || placementChanged)
        {
            if (placementChanged)
                ForgetCalibration();

            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.passScratch);
            ParkNrResource(g_nr.passClampScratch);
            ParkNrResource(g_nr.passClampScratch2);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.outputNative);
            ParkNrResource(g_nr.activeColor);
            ParkNrResource(g_nr.lutScratch);
            ParkNrResource(g_nr.styled);
            g_nr.styledState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ParkNrResource(g_nr.styledB);
            g_nr.styledBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            g_nr.passScratchFailed = false;
            g_nr.passClampScratchFailed = false;
            g_nr.passClampScratch2Failed = false;
            g_nr.lutScratchFailed = false;
        }
    }

    // The kept copy holds the frame the pass works on, which the resolve reads back as its original when the colour
    // takes a UAV. While the Colour encoding override converts (gamma 2.2, PQ) that frame is no longer in the game's
    // encoding -- PQ decoded to linear runs past 1.0 -- so it needs a float copy. Without a UAV the copy is the
    // resolve's target instead, copied back over the colour, so it has to keep the colour's exact format (and the
    // resolve reads the game's own texture, OriginalIsGameColour).
    const DXGI_FORMAT keepFormat = !targetSupportsUav ? desc.Format
                                   : DlssNrColourEncoding::ShaderConverts(frame.InputEncoding)
                                       ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                       : colourFormat;

    if (g_nr.hdrCopy != nullptr && g_nr.hdrCopy->GetDesc().Format != keepFormat)
        ParkNrResource(g_nr.hdrCopy);

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, modelFormat, modelWidth, modelHeight);
        g_nr.colorCopy = CreateScratch(device, colourFormat, width, height);
        g_nr.workWidth = modelWidth;
        g_nr.workHeight = modelHeight;
    }

    if (g_nr.hdrCopy == nullptr)
        g_nr.hdrCopy = CreateScratch(device, keepFormat, width, height);

    // The crop is a CopyTextureRegion from the game's colour: exact format
    if (g_nr.activeColor != nullptr && g_nr.activeColor->GetDesc().Format != desc.Format)
        ParkNrResource(g_nr.activeColor);

    if (cropColor && g_nr.activeColor == nullptr)
        g_nr.activeColor = CreateScratch(device, desc.Format, width, height);
    if (cropColor && g_nr.activeColor == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the pre-SR active colour staging texture could not be allocated";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    if (requestedPasses == 1)
    {
        // Reclaim the extra raster and clear its failure latch. Raising the count later gets one fresh
        // allocation attempt; holding a failing allocation at two must not retry it every frame.
        ParkNrResource(g_nr.passScratch);
        g_nr.passScratchFailed = false;
        ParkNrResource(g_nr.passClampScratch);
        g_nr.passClampScratchFailed = false;
        ParkNrResource(g_nr.passClampScratch2);
        g_nr.passClampScratch2Failed = false;
    }
    else if (g_nr.passScratch == nullptr && !g_nr.passScratchFailed)
    {
        g_nr.passScratch = CreateScratch(device, modelFormat, modelWidth, modelHeight);
        g_nr.passScratchFailed = g_nr.passScratch == nullptr;

        if (g_nr.passScratchFailed)
            LOG_ERROR("DLSS-NR: could not allocate the model-output ping-pong; extra passes are disabled");
    }

    if (requestedPasses > 1 && g_nr.passClampScratch == nullptr && !g_nr.passClampScratchFailed)
    {
        g_nr.passClampScratch = CreateScratch(device, modelFormat, modelWidth, modelHeight);
        g_nr.passClampScratchFailed = g_nr.passClampScratch == nullptr;

        if (g_nr.passClampScratchFailed)
            LOG_ERROR("DLSS-NR: could not allocate the interpass clamp target; extra passes are disabled");
    }

    // A second clamp target, ping-ponging with the one above exactly like output/passScratch ping-pong
    // model answers: the clamp step reads the previous boundary's clamped proxy as well as writing the
    // next one, so with only one buffer the second boundary would alias its own read and write. Only
    // the second boundary (Passes=3, not Passes=2) ever reaches it -- gated on > 2, not > 1 like
    // passClampScratch, so a Passes=2 configuration doesn't permanently carry a full working-resolution
    // texture it structurally can never use. A failure here doesn't disable multipass outright, only
    // caps the chain one pass short at that second boundary (the pass loop's own null-target check).
    if (requestedPasses > 2 && g_nr.passClampScratch2 == nullptr && !g_nr.passClampScratch2Failed)
    {
        g_nr.passClampScratch2 = CreateScratch(device, modelFormat, modelWidth, modelHeight);
        g_nr.passClampScratch2Failed = g_nr.passClampScratch2 == nullptr;

        if (g_nr.passClampScratch2Failed)
            LOG_ERROR("DLSS-NR: could not allocate the second interpass clamp target; "
                      "the pass chain will stop one pass short of the third");
    }
    else if (requestedPasses <= 2 && g_nr.passClampScratch2 != nullptr)
    {
        ParkNrResource(g_nr.passClampScratch2);
        g_nr.passClampScratch2Failed = false;
    }

    // Packed, the model input comes from the colour pack instead, straight from the full-size proxy.
    // Replace keeps the ordinary reduced picture (below or above 100%) as the reference for the unpack.
    //
    // colorSmall is made at the ordinary size, which can move while the packed size (all resolutionChanged sees under
    // compression) stays, so its own size is checked: the downsample would write past a smaller one and the unpack would
    // read zeros there.
    if (g_nr.colorSmall != nullptr)
    {
        const D3D12_RESOURCE_DESC smallDesc = g_nr.colorSmall->GetDesc();

        if (DlssNr::SizeStale((unsigned) smallDesc.Width, smallDesc.Height, workWidth, workHeight))
            ParkNrResource(g_nr.colorSmall);
    }

    if (spatial && !replaceCorrection)
        ParkNrResource(g_nr.colorSmall);
    else if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, colourFormat, workWidth, workHeight);

    // The up/down-leg target is native (the answer is brought back to frame size before the
    // resolve) -- shared by both the supersampling down-leg (> 1) and the reduced up-leg (< 1),
    // mutually exclusive per frame.
    // Packed and supersampling, the pair is averaged into Compress screen edges' own native pair instead.
    if (workScale != 1.0f && !(spatial && workScale > 1.0f) && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, colourFormat, width, height);

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        // The frame statistics diagnostic's own pair, so it never shares a buffer with the exposure ring.
        for (ID3D12Resource** rb : { &g_nr.diagGridReadback, &g_nr.diagExposureReadback, &g_nr.diagProxyReadback })
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(rb))))
                *rb = nullptr;
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.autoExposure == nullptr)
    {
        g_nr.autoExposure = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, 1, 1);
        g_nr.autoExposureReadable = false;

        if (g_nr.autoExposure != nullptr)
            LOG_INFO("DLSS-NR: GPU automatic exposure is available");
        else
            LOG_WARN("DLSS-NR: could not allocate the automatic exposure texture");

        // A new texture holds nothing to ease from.
        g_nr.autoExposureAdapter.Invalidate();
    }

    // Tried once per device: a failure is not retried (and logged) every frame.
    if (g_nr.autoExposureRaw == nullptr && !g_nr.autoExposureRawFailed)
    {
        g_nr.autoExposureRaw = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, 1, 1);
        g_nr.autoExposureRawFailed = g_nr.autoExposureRaw == nullptr;

        if (g_nr.autoExposureRawFailed)
            LOG_WARN("DLSS-NR: could not allocate the eye adaptation texture; Automatic follows every frame at once");
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr && g_nr.hdrCopy != nullptr)
    {
        auto snippet = FindSelectedModel();

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return;
        }

        version_t nrVersion {};
        if (Util::GetFileVersion(snippet->wstring(), &nrVersion))
            g_nr.superiorMainOnlyGains = DlssNr::SuperiorScaling::MainOnlyRuntime(nrVersion.major, nrVersion.minor);
        else
        {
            g_nr.superiorMainOnlyGains = false;
            // Only Superior reads it; nothing in the log for users who never select it.
            if (superior)
                LOG_WARN("Superior Scaling: NR runtime version unavailable; using 310.9 preset semantics for "
                         "frequency gains.");
        }

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        const auto tuning = PassTuning(cfg, 0);
        // Not at the same moment as a game's vkCreateDevice (native/DriverInitGate.h)
        native::DriverInitScope driverInit;
        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, modelWidth, modelHeight,
                        (int) PassPreset(cfg, 0),
                        tuning.intensity, (int) PassStyle(cfg, 0),
                        tuning.structure, tuning.tone, tuning.skin,
                        tuning.autoMask ? 1 : 0,
                        // UI correction at the model's own default: with no UI layer fed to it there
                        // is nothing for it to correct.
                        1);

        if (g_nr.feature == nullptr)
        {
            g_nr.featurePendingSubmission = false;
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            if (g_nr.lastModelError && *g_nr.lastModelError())
            {
                g_nr.modelError = g_nr.lastModelError();
                g_nr.reason = g_nr.modelError.c_str();
            }
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.beforeUpscale = frame.BeforeUpscale;
        g_nr.rayReconstruction = frame.RayReconstruction;
        g_nr.reset = true;
        g_nr.featurePendingSubmission = true;
        g_nr.featureCreateEpoch = frame.SubmissionEpoch;
        RecordBuiltPrimaryTuning(cfg);
        LOG_INFO("DLSS-NR model feature created from {}", snippet->string());
        LOG_INFO("DLSS-NR running {}: target {}x{}, model input {}x{} (main network {}x{}), guides {}x{} "
                 "(preset {}, intensity {}, style {}, build epoch {})",
                 frame.RayReconstruction ? (frame.BeforeUpscale ? "before RR+SR" : "after RR+SR") :
                     (frame.BeforeUpscale ? "before SR" : "after SR"),
                 width, height, modelWidth, modelHeight, (modelWidth + 1) / 2, (modelHeight + 1) / 2,
                 guideWidth, guideHeight, g_nr.builtPreset[0], g_nr.builtIntensity, g_nr.builtStyle[0],
                 frame.SubmissionEpoch);

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        device->Release();
        return;
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    // A later function call is not proof that the command list containing CreateFeature was
    // submitted: some engines record more than one upscale on the same list. Native DX12 supplies
    // the wrapped Present count and the bridges supply their post-Execute frame counter, so an epoch
    // change is the first point at which evaluating the feature is safe.
    if (g_nr.featurePendingSubmission)
    {
        if (frame.SubmissionEpoch == g_nr.featureCreateEpoch)
        {
            device->Release();
            return;
        }

        g_nr.featurePendingSubmission = false;
        LOG_INFO("DLSS-NR: primary feature ready after submitted epoch {}", g_nr.featureCreateEpoch);
    }

    // Park no-longer-requested feature histories immediately (their actual release remains deferred),
    // and clear their failure latch so a later 1 -> N change is a deliberate retry.
    for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
    {
        if (pass >= requestedPasses)
        {
            ParkNrFeature(g_nr.passFeature[pass]);
            g_nr.passNeedsReset[pass] = false;
            g_nr.passCreateFailed[pass] = false;
            g_nr.passPendingSubmission[pass] = false;
        }
    }

    // Do not create another feature, and do not evaluate any feature, while a requested layer still
    // belongs to the current submission epoch. This keeps multiple upscaler evaluations recorded on
    // one command list from recreating the historical create/evaluate GPU hang.
    for (unsigned int pass = 1; pass < requestedPasses; ++pass)
    {
        if (!g_nr.passPendingSubmission[pass])
            continue;

        if (frame.SubmissionEpoch == g_nr.passCreateEpoch[pass])
        {
            device->Release();
            return;
        }

        g_nr.passPendingSubmission[pass] = false;
        LOG_INFO("DLSS-NR: feature for pass {} ready after submitted epoch {}", pass + 1,
                 g_nr.passCreateEpoch[pass]);
    }

    // Build at most one missing extra feature on this invocation and evaluate nothing afterwards.
    // NGX feature creation records work on the supplied command list; evaluating that feature before
    // the list has been submitted is the creation-frame GPU hang that caused the old multi-pass path
    // to be removed. A new feature therefore gets an entire build-only frame and starts next time.
    // Also gated on the first clamp scratch buffer: without it there is nowhere to land an
    // intermediate pass's raw answer before handing it to the next pass, so no extra pass may become
    // active. Not gated on passClampScratch2 here -- a 2-pass chain (one boundary) never touches it;
    // if it failed to allocate, the pass loop itself degrades gracefully at the second boundary
    // instead of disabling multipass outright (see the clamp dispatch's own null check).
    if (g_nr.passScratch != nullptr && g_nr.passClampScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] != nullptr)
                continue;

            if (g_nr.passCreateFailed[pass])
                break;

            auto snippet = FindSelectedModel();

            if (!snippet.has_value())
            {
                g_nr.passCreateFailed[pass] = true;
                LOG_ERROR("DLSS-NR: pass {} feature not built because {} disappeared",
                          pass + 1, SelectedModelFile());
            }
            else
            {
                SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
                const auto tuning = PassTuning(cfg, pass);
                native::DriverInitScope driverInit;
                g_nr.passFeature[pass] = g_nr.create(
                    snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                    device, cmdList, g_nr.capabilityParams, modelWidth, modelHeight,
                    (int) PassPreset(cfg, pass), tuning.intensity,
                    (int) PassStyle(cfg, pass),
                    tuning.structure, tuning.tone, tuning.skin,
                    tuning.autoMask ? 1 : 0, 1);

                if (g_nr.passFeature[pass] != nullptr)
                {
                    g_nr.builtPreset[pass] = PassPreset(cfg, pass);
                    g_nr.builtPassTuning[pass] = tuning;
                    g_nr.builtStyle[pass] = PassStyle(cfg, pass);
                    g_nr.passNeedsReset[pass] = true;
                    g_nr.passPendingSubmission[pass] = true;
                    g_nr.passCreateEpoch[pass] = frame.SubmissionEpoch;
                    LOG_INFO("DLSS-NR: feature for pass {} built with preset {}, style {} at epoch {}; "
                             "waiting for submission",
                             pass + 1, g_nr.builtPreset[pass], g_nr.builtStyle[pass],
                             frame.SubmissionEpoch);
                }
                else
                {
                    g_nr.passPendingSubmission[pass] = false;
                    g_nr.passCreateFailed[pass] = true;
                    LOG_ERROR("DLSS-NR: feature for pass {} failed to build; using {} ready pass(es)",
                              pass + 1, pass);
                }
            }

            device->Release();
            return;
        }
    }

    if (superior)
    {
        ResTrack_Dx12::HookLateNrQueue(device);
        const bool temporal = DlssNr::SuperiorScaling::NeedsTemporal(width, height, workWidth, workHeight, spatial,
                                                                     modelWidth, modelHeight);
        // Wait: the private DLSS was just created, its first evaluate waits for a later submission (one frame, like
        // the NR feature's own creation). Unavailable: given up for this session (logged once); this frame is skipped
        // and from the next one the ordinary path runs with Matched residual.
        if (SuperiorRun::Begin(*this, cmdList, device, colourFormat, width, height,
                               superiorPacked ? modelWidth : workWidth, superiorPacked ? modelHeight : workHeight,
                               workScale, frame, temporal, depth && guides.depth.valid(),
                               superiorPacked ? &EdgeCompression::Layout() : nullptr) != SuperiorRun::Began::Run)
        {
            device->Release();
            return;
        }
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    // EvaluateInternal has already combined the game's HDR flag with the authoritative output format.
    // That authority matters before SR: Color and Output may use different surface formats while still
    // representing the same frame colour space.
    const bool isHdrBuffer = frame.ColourIsLinearHdr;

    static bool reportedHdr = false;
    static bool reportedHdrValue = false;
    static bool reportedBefore = false;
    static uint32_t reportedEncoding = 0;
    static bool reportedForced = false;

    if (!reportedHdr || reportedHdrValue != isHdrBuffer || reportedBefore != frame.BeforeUpscale ||
        reportedEncoding != frame.ColourEncoding || reportedForced != frame.ColourEncodingForced)
    {
        reportedHdr = true;
        reportedHdrValue = isHdrBuffer;
        reportedBefore = frame.BeforeUpscale;
        reportedEncoding = frame.ColourEncoding;
        reportedForced = frame.ColourEncodingForced;
        if (!frame.ColourEncodingForced)
            LOG_INFO("DLSS-NR {} SR: the game's DLSS colour space is {} so the colour transform is {}",
                     frame.BeforeUpscale ? "before" : "after",
                     isHdrBuffer ? "linear HDR" : "already tone-mapped",
                     isHdrBuffer ? "on" : "off");
        else
            LOG_INFO("DLSS-NR {} SR: colour encoding forced to {}, so the colour transform is {}",
                     frame.BeforeUpscale ? "before" : "after",
                     DlssNrColourEncoding::Name((DlssNrColourEncoding::Encoding) frame.ColourEncoding),
                     isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const bool dump = g_captureIsDump;
        const auto captureDir = Util::DllPath().remove_filename() / (dump ? "dlssnr-cachedump" : "dlssnr-capture");
        std::string about;
        if (dump)
        {
            std::error_code ec;
            std::filesystem::remove_all(captureDir, ec); // each dump replaces the last
            about = "motion_NN.raw is the game's motion vectors as the pass reads them (the frame note's mvScale "
                    "applies, then the subrect: motion WxH at X Y), depth_NN.raw its depth (depth WxH at X Y, inverted "
                    "1 = reversed-Z). The model ran on every frame (detail reuse stands aside during a capture). "
                    "linearHdr 0: before/after are display-encoded (sRGB-like); curve is DlssNrConstants::ReversibleMode; "
                    "white is the encode's white point.";
        }
        const auto written = g_capture.write(captureDir, about);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote {} to {}", dump ? "an edit cache dump" : "matched before/after frames", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.

    ResTrack_Dx12::HookLateNrQueue(device);
    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<DlssNrGpuTime>(device, "total");

    if (g_ngxTime == nullptr)
        g_ngxTime = std::make_unique<DlssNrGpuTime>(device, "model");

    if (g_scanTime == nullptr)
        g_scanTime = std::make_unique<DlssNrGpuTime>(device, "exposure scan");

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // Copy just the live image, not the stale right/bottom margins. Do this only after model
    // creation/pending-submission early returns, and inside the measured GPU interval. The compact
    // texture lets every existing codec/compare/hold/capture path use unmodified pixel coordinates.
    ID3D12Resource* const gameColor = target;
    if (cropColor)
    {
        TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, g_nr.activeColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
        DlssNr::CopyActiveColor(cmdList, g_nr.activeColor, gameColor, *active);
        TransitionTarget(outputArrival);
        Barrier(cmdList, g_nr.activeColor, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target = g_nr.activeColor;
        targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    const auto FinishColor = [&](bool copyBack)
    {
        if (cropColor)
        {
            if (copyBack)
            {
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmdList, gameColor, outputArrival, D3D12_RESOURCE_STATE_COPY_DEST);
                DlssNr::CopyActiveColor(cmdList, gameColor, target, *active);
                Barrier(cmdList, gameColor, D3D12_RESOURCE_STATE_COPY_DEST, outputArrival);
            }
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            TransitionTarget(outputArrival);
        }
    };

    // Fetch the game's exposure, where the game supplies one and the user asked for it.
    //
    // This used to measure the white point off the frame as well, over a 64x64 grid of tile
    // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
    // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
    // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
    // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
    // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
    // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
    // never dispatched the meter and the white point silently fell back to the slider.
    //
    // The ring carries the game's exposure or the automatic one, and a slot written for one source must
    // never be read as the other's, so a change of source starts it over.
    DlssNr::NoteExposureFrame(cfg, frame.ExposureTexture != nullptr && !frame.GameAutoExposure,
                              g_nr.gameExposure > 1e-6f);
    const uint32_t whitePointSource = DlssNr::WhitePointSourceEffective(cfg);

    if (whitePointSource != g_nr.exposureReadbackSource)
    {
        InvalidateExposureMeter();
        g_nr.exposureReadbackSource = whitePointSource;
    }

    const bool exposureSettingOn = whitePointSource == 1;

    // Nothing held from before the option was switched off may survive switching it back on. See
    // InvalidateExposureMeter for what froze and why it read as a colour cast.
    if (exposureSettingOn && !g_nr.exposureSettingWasOn)
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    const bool wantExposure = exposureSettingOn && frame.ExposureTexture != nullptr;

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;

        // One pixel. Only tile (0,0) is read back, and it is a courier: the shader copies the game's
        // exposure into it rather than averaging tile pixels, which is what MeterCopiesExposure asks for.
        meterParams.Width = 1;
        meterParams.Height = 1;
        meterParams.MeterCopiesExposure = 1;

        const D3D12_RESOURCE_STATES priorTargetState = targetState;
        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr,
                     (ID3D12Resource*) frame.ExposureTexture, nullptr, g_nr.meter, nullptr);
        TransitionTarget(priorTargetState);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    // Automatic exposure (source 3). Meter the linear HDR frame on the GPU, reduce the 4096 tile means
    // to a 1x1 exposure texture, and let the encode and resolve read it this frame -- no readback
    // wait. Not in finished-picture mode, which keeps its own display white point, and not on a frame
    // the game already tone mapped, where there is no linear scene to meter.
    //
    // The meter reads `target` as it stands before this pass writes anything: the upscaler's fresh
    // output. Nothing this pass writes is measured, which is what the removed statistical meter got
    // wrong.
    bool usingAutoExposure = false;

    // Nor on display light with a pinned white point (forced PQ: its reference white, see ApplyColourEncoding).
    if (whitePointSource == 3 && !frame.FinishedPicture && isHdrBuffer && frame.WhitePointOverride <= 0.0f &&
        g_nr.meter != nullptr && g_nr.autoExposure != nullptr)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;
        meterParams.Width = kDlssNrMeterGrid;
        meterParams.Height = kDlssNrMeterGrid;
        meterParams.MeterCopiesExposure = 0;
        meterParams.InputEncoding = frame.InputEncoding; // reads the game's frame

        const D3D12_RESOURCE_STATES priorTargetState = targetState;
        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr, nullptr, nullptr, g_nr.meter, nullptr);
        TransitionTarget(priorTargetState);

        Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_nr.autoExposureReadable)
            Barrier(cmdList, g_nr.autoExposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DlssNrConstants autoParams {};
        autoParams.Mode = DlssNrMode_AutoExposure;
        autoParams.Width = 1;
        autoParams.Height = 1;
        autoParams.PreExposure = frame.PreExposure;
        autoParams.ExposureSourceWidth = width;
        autoParams.ExposureSourceHeight = height;
        autoParams.AutoExposureShadowProtection =
            std::clamp(cfg.DlssNrAutoExposureShadowProtection.value_or_default(), 0.0f, 100.0f);
        SetAutoExposureMeter(autoParams, cfg.DlssNrAutoExposureMeter.value_or_default() != DlssNrExposureMeter::kAverage,
                             cfg.DlssNrAutoExposureMeterLowPercent.value_or_default(),
                             cfg.DlssNrAutoExposureMeterHighPercent.value_or_default());

        // Eye adaptation (DlssNr_ExposureAdapt.h): the reading goes to autoExposureRaw and a one-texel pass eases
        // autoExposure toward it. Without that texture or the pass, the meter writes autoExposure itself, as before;
        // the evaluations it does so leave a gap the adapter snaps across.
        const float brighterSeconds = DlssNrExposureAdapt::Seconds(
            cfg.DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(), DlssNrExposureAdapt::kDefaultBrighterSeconds);
        const float darkerSeconds = DlssNrExposureAdapt::Seconds(cfg.DlssNrAutoExposureAdaptDarkerSeconds.value_or_default());
        const bool adapting = (brighterSeconds > 0.0f || darkerSeconds > 0.0f) && g_nr.autoExposureRaw != nullptr &&
                              ExposureAdaptReady();
        g_nr.autoExposureAdapting = adapting;

        DispatchPass(cmdList, autoParams, g_nr.meter, nullptr, nullptr, nullptr, nullptr,
                     adapting ? g_nr.autoExposureRaw : g_nr.autoExposure, nullptr);

        if (adapting)
        {
            Barrier(cmdList, g_nr.autoExposureRaw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // g_frames counts NR evaluations, so one Automatic skipped (another source, finished picture) is a gap;
            // g_nr.reset is the game's cut (and a new feature). NR off counts none: the elapsed time does it.
            const double now =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const DlssNrExposureAdapt::Step step =
                g_nr.autoExposureAdapter.Next(g_frames, now, brighterSeconds, darkerSeconds, g_nr.reset);

            // Overlays the first fields (dlssnr_exposure_adapt.hlsl): WhitePoint carries the brighter blend, Width the snap, Height the darker blend.
            DlssNrConstants adaptParams {};
            adaptParams.Mode = DlssNrMode_AutoExposure;
            adaptParams.WhitePoint = step.blendBrighter;
            adaptParams.Width = step.snap ? 1u : 0u;
            adaptParams.Height = step.DarkerBits();

            if (!DispatchExposureAdapt(cmdList, adaptParams, g_nr.autoExposureRaw, g_nr.autoExposure))
            {
                g_nr.autoExposureAdapter.Invalidate();
                g_nr.autoExposureAdapting = false;
            }
        }

        Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, g_nr.autoExposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_nr.autoExposureReadable = true;
        usingAutoExposure = true;

        // The game's exposure into the meter's tile (0,0), read back beside Automatic's: the follow-game calibration
        // needs the two from the same frame. The meter's tiles have been reduced already; nothing else reads them now.
        // Same rule as Vulkan: only while following the game's exposure (DlssNr_GameDefaults.h), so nothing is learned
        // while it is off (switching it on later starts learning at that moment).
        // Also while the exposure scan runs: its recording needs the game's exposure beside Automatic's reading.
        const bool pairGameExposure =
            frame.ExposureTexture != nullptr && (DlssNr::FollowGameOn(cfg) || DlssNr::ExposureScan::Scanning());

        if (pairGameExposure)
        {
            DlssNrConstants courierParams {};
            courierParams.Mode = DlssNrMode_Meter;
            courierParams.Width = 1;
            courierParams.Height = 1;
            courierParams.MeterCopiesExposure = 1;

            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DispatchPass(cmdList, courierParams, target, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture,
                         nullptr, g_nr.meter, nullptr);
            TransitionTarget(priorTargetState);
        }

        // The reading rides home beside the eased value, and autoExposureRaw goes back to the UAV state the meter
        // writes it in.
        if (adapting)
            Barrier(cmdList, g_nr.autoExposureRaw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);

        CopyAutoExposureToReadback(cmdList, frame.PreExposure, pairGameExposure,
                                   adapting ? g_nr.autoExposureRaw : nullptr);

        if (adapting)
            Barrier(cmdList, g_nr.autoExposureRaw, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ConsumeMeterReadback();
    }

    // Automatic follows the game's own exposure when that is on (DlssNr_GameDefaults.h: a known unexposed game or the
    // user's choice), once the calibration has locked and while the game is still supplying its exposure this frame.
    // See DlssNr_FollowGame.h.
    g_nr.followingGame = usingAutoExposure && frame.ExposureTexture != nullptr && DlssNr::FollowGameOn(cfg) &&
                         DlssNrFollowGame::Instance().Locked() && g_nr.autoPairGameExposure > 1e-8f;
    const float exposureBaseScale = g_nr.followingGame ? DlssNrFollowGame::Instance().Scale() : 1.0f;

    // "Tune for this scene" (DlssNr_ExposureCalibrate_Dx12.inl): before the white point is resolved, so a run's
    // pinned white point (CalibrationWhitePoint) reaches the encode and resolve below. While no run is on and the menu
    // is not looking, only a timestamp.
    if (CalibrationWanted())
        CalibrationBeginFrame(cfg, device, width, height,
                              CalibrationSituation(cfg, usingAutoExposure, isHdrBuffer, frame.FinishedPicture,
                                                   frame.ExposureTexture != nullptr,
                                                   DlssNrColourEncoding::ShaderConverts(frame.InputEncoding)),
                              CalibrationBase(cfg));
    else
        CalibrationIdleFrame();

    g_nr.gamePreExposure = frame.PreExposure;

    // Auto's found exposure (story 5): the game's own movement at Automatic's level, only where Automatic runs, and
    // never while following the game (that one is handed over, so Auto has no scan match then). Moving to and from it
    // is eased over a second, in stops (DlssNr_ExposureFoundBlend.h): Cyberpunk 2077 took over 0.84 EV apart, in a
    // scene it keeps darker than the meter would.
    {
        float found = 0.0f;
        float foundPre = 1.0f;
        const float target = usingAutoExposure && !g_nr.followingGame &&
                                     DlssNr::ExposureScan::FoundExposure(&found, &foundPre) && found > 1e-8f
                                 ? foundPre / found
                                 : 0.0f;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float own = AutoOwnBaseWhitePoint();
        g_nr.foundBaseWhitePoint = g_nr.foundBlend.Step(target, own, now, usingAutoExposure);

        switch (g_nr.foundBlend.LastEvent())
        {
        case DlssNrExposureFoundBlend::Event::TookOver:
            LOG_INFO("DLSS-NR automatic exposure: the game's own exposure (found by the scan) moves the white point from "
                     "here: {:.4g}, from {:.4g} ({:+.2f} EV), eased over a second",
                     target, g_nr.foundBaseWhitePoint, std::log2(g_nr.foundBaseWhitePoint / target));
            break;
        case DlssNrExposureFoundBlend::Event::Leaving:
            LOG_INFO("DLSS-NR automatic exposure: the game's exposure is gone; back to Automatic's own meter, eased");
            break;
        case DlssNrExposureFoundBlend::Event::Left:
            LOG_INFO("DLSS-NR automatic exposure: Automatic's own meter again");
            break;
        default:
            break;
        }
    }

    float whitePoint = frame.WhitePointOverride > 0.0f ? frame.WhitePointOverride : ResolveWhitePoint(cfg, isHdrBuffer);

    // While Tune for this scene runs, the white point is pinned on the CPU (the base frozen at its start times the
    // step's Trim) and the shader is not asked to recompute it from the live exposure, so the steps differ only by the
    // Trim. The sweep aborts if the live base drifts.
    const bool calibrationPinned = CalibrationWhitePoint() > 0.0f;

    if (calibrationPinned)
        whitePoint = CalibrationWhitePoint();

    // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
    // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
    // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
    // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    // Only with the game's-setting scale: the shader recomputes PreExposure / exposure, and Native look's base is 1
    // whatever the exposure (DlssNr_GameScale.h), so there the host value, 1 x the Trim, is the white point. Binding
    // the texture under Native look made the shader put the game's exposure back (NBA 2K27: ~0.4 EV).
    if (DlssNr::WhitePointSourceEffective(cfg) == 1 && frame.ExposureTexture != nullptr &&
        frame.WhitePointOverride <= 0.0f &&
        DlssNrGameScale::ShaderReadsGameExposure(DlssNr::GameExposureScaleEffective(cfg)))
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        // The Trim no longer rides in here. It goes to the shader in the trim fields, so anchors can
        // move it with the live base white point.
        exposurePreMul = g_nr.gamePreExposure;
    }
    else if (usingAutoExposure)
    {
        // The automatic exposure texture takes the same t4 slot. It does not set UseGameExposure:
        // that flag means the game's own texture, and UseExposureWhitePoint says this one. Following the game, the
        // game's texture is bound in its place and ExposureBaseScale carries the calibration.
        exposureTex = g_nr.followingGame ? (ID3D12Resource*) frame.ExposureTexture : g_nr.autoExposure;
    }

    // Tune for this scene pins the white point for Game exposure too: not recomputed from the game's live texture.
    if (calibrationPinned)
        useGameExposure = 0;

    // The encode's, the resolve's and the LUT pass's live white point fields, one rule for all three: the shader
    // recomputes the white point from exposureTex where these say so, else uses the host's whitePoint.
    const auto fillLiveWhitePoint = [&](DlssNrConstants& params)
    {
        params.WhitePoint = whitePoint;
        params.UseGameExposure = useGameExposure;
        params.ExposurePreMul = exposurePreMul;
        params.UseExposureWhitePoint =
            usingAutoExposure && !calibrationPinned && !(g_nr.foundBaseWhitePoint > 0.0f) ? 1u : 0u;
        params.ExposureBaseScale = exposureBaseScale;
        FillExposureConstants(params, cfg, usingAutoExposure ? 3u : 1u, frame.PreExposure);
    };

    // LUT-apply epic (dlssnr-lut-apply), Story 2: grade `target` through a
    // loaded .cube file before the model sees it. After the white point is resolved above (it grades for that
    // white), and deliberately AFTER the crop and the
    // exposure measurement above that, not right after g_gpuTime->Start where Story 2 originally put
    // it -- that measured the LUT's own graded output as if it were the clean upscaler frame, repeating
    // (from a different angle) the exact mistake the Automatic-exposure meter's own comment above warns
    // about ("nothing this pass writes is measured"). A nonlinear grade shifts apparent scene brightness
    // by a different, content-dependent amount per scene, so the symptom wasn't a fixed bias but
    // exposure needing a different correction shot to shot (found 2026-10-04, user report). Grading
    // here instead means the meter and crop both still see the clean frame, exactly as they did before
    // this epic existed; only the model and everything after it see the grade. Writes back onto
    // `target` itself (via lutScratch and a GPU copy) rather than reassigning the pointer, so codec,
    // model and resolve all still run exactly as they did before the epic existed, LUT loaded or not.
    {
        const std::string lutPath = cfg.DlssNrLutFile.value_or_default();

        if (!lutPath.empty())
        {
            // Copied back onto `target` (CopyTextureRegion): exact format
            if (g_nr.lutScratch != nullptr && g_nr.lutScratch->GetDesc().Format != desc.Format)
            {
                ParkNrResource(g_nr.lutScratch);
                g_nr.lutScratchFailed = false;
            }

            if (g_nr.lutScratch == nullptr && !g_nr.lutScratchFailed)
            {
                g_nr.lutScratch = CreateScratch(device, desc.Format, width, height);
                g_nr.lutScratchFailed = g_nr.lutScratch == nullptr;

                if (g_nr.lutScratchFailed)
                    LOG_ERROR("DLSS-NR: could not allocate the LUT pass's scratch target; LutFile is ignored");
            }

            if (g_nr.lutScratchFailed)
                DlssNr::ReportLutStatus(true, _lutState.loadedPath, _lutState.lut.size, true,
                                        "could not allocate the LUT pass's scratch target", lutPath,
                                        cfg.DlssNrLutStrength.value_or_default());

            if (g_nr.lutScratch != nullptr)
            {
                const D3D12_RESOURCE_STATES priorTargetState = targetState;
                TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                const float lutStrength = std::clamp(cfg.DlssNrLutStrength.value_or_default(), 0.0f, 1.0f);
                // The white point the encode below uses, so a linear HDR frame lands in the LUT's 0-1 domain where the
                // model's own proxy puts it. Not AutoTrimEffective: that is only the Trim on top of the base white point,
                // and a game whose frame is scaled by its exposure (RDR2: paper white ~1700x) would otherwise be seen
                // as ~1000x over white -- the whole picture pinned at the top of the curve. With the encode's live
                // fields and texture, so where the encode recomputes it this frame the LUT does too (it used to take
                // the host value, 3-4 frames old, made for a different white than the model was shown).
                DlssNrConstants lutWhite {};
                fillLiveWhitePoint(lutWhite);
                const bool graded =
                    DispatchLut(cmdList, target, g_nr.lutScratch, width, height, lutStrength, frame.InputEncoding,
                                isHdrBuffer, lutWhite, exposureTex, lutPath);

                DlssNr::ReportLutStatus(true, _lutState.loadedPath, _lutState.lut.size, _lutState.failed,
                                        _lutState.error, _lutState.attemptedPath, lutStrength);

                if (graded)
                {
                    // Copy the graded scratch back onto `target` itself: nothing after this point needs to
                    // know a LUT ran, including the model dispatch ahead, which otherwise has no idea its
                    // own source (`target`) might be aliased to a texture it does not own.
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                    Barrier(cmdList, g_nr.lutScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    DlssNr::CopyActiveColor(cmdList, target, g_nr.lutScratch, DlssNr::ColorExtent { width, height });
                    Barrier(cmdList, g_nr.lutScratch, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }

                // Parsed-but-unusable (a malformed file) or the pipeline failed to build takes the same path
                // back: restore exactly the state target was in before this block touched it.
                TransitionTarget(priorTargetState);
            }
        }
        else
        {
            // LutFile was cleared: neither the scratch target nor the uploaded 3D texture (up to ~16 MB for
            // a 128^3 lattice) is reused by anything else, so there is no reason to keep holding either --
            // the same hold-only-while-wanted discipline activeColor's own scratch follows for cropColor.
            // Fixed (Review Pass, 2026-10-04): this used to park only the scratch target, so _lutTexture
            // stayed resident for the rest of the session once any LUT had ever loaded. A no-op once
            // already released/parked.
            ParkNrResource(g_nr.lutScratch);
            ReleaseLutTexture();
            DlssNr::ReportLutStatus(false, "", 0, false, "", "", cfg.DlssNrLutStrength.value_or_default());
        }
    }

    // Frame statistics diagnostic (ini [DlssNr] FrameStats). Every 120th frame: average every tile of the
    // frame NR was handed into the meter grid and queue it for readback, then courier the game's exposure
    // texture (when there is one) into tile 0 and queue that too. Runs whatever the white point source,
    // and after the blocks above have finished with the meter, so it can overwrite anything in it: the
    // next frame's own dispatch writes it all again. The grid is copied out before the courier touches
    // tile 0. Read eight frames later, below, once the white point in force is known.
    if (cfg.DlssNrFrameStats.value_or_default() && g_nr.meter != nullptr && g_nr.diagGridReadback != nullptr &&
        g_nr.diagExposureReadback != nullptr && g_nr.diagQueuedAt == 0 && (g_frames % 120) == 0)
    {
        const D3D12_RESOURCE_STATES priorTargetState = targetState;
        const D3D12_RESOURCE_DESC gameDesc = gameColor->GetDesc();

        DlssNrConstants gridParams {};
        gridParams.Mode = DlssNrMode_Meter;
        gridParams.Width = kDlssNrMeterGrid;
        gridParams.Height = kDlssNrMeterGrid;
        gridParams.MeterCopiesExposure = 0;
        gridParams.InputEncoding = frame.InputEncoding; // reads the game's frame

        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, gridParams, target, nullptr, nullptr, nullptr, nullptr, g_nr.meter, nullptr);
        TransitionTarget(priorTargetState);
        CopyMeterGridTo(cmdList, g_nr.diagGridReadback);

        if (frame.ExposureTexture != nullptr)
        {
            DlssNrConstants courierParams {};
            courierParams.Mode = DlssNrMode_Meter;
            courierParams.Width = 1;
            courierParams.Height = 1;
            courierParams.MeterCopiesExposure = 1;

            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DispatchPass(cmdList, courierParams, target, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture,
                         nullptr, g_nr.meter, nullptr);
            TransitionTarget(priorTargetState);
            CopyMeterGridTo(cmdList, g_nr.diagExposureReadback);
        }

        g_nr.diagQueuedAt = g_frames;
        g_nr.diagProxyQueued = false;
        g_nr.diagPassthrough = !isHdrBuffer;
        g_nr.diagFormat = gameDesc.Format;
        g_nr.diagWidth = (unsigned int) gameDesc.Width;
        g_nr.diagHeight = gameDesc.Height;
        g_nr.diagPreExposure = frame.PreExposure;
        g_nr.diagExposureSupplied = frame.ExposureTexture != nullptr;
    }

    if (g_nr.diagQueuedAt != 0 && g_frames >= g_nr.diagQueuedAt + 8)
        ReportFrameStats(whitePoint, whitePointSource);

    // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
    // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
    // live output before the encode reads it while held, so the encode's own path and barriers below
    // are untouched and the default (hold off) is byte-identical. See design/frame-hold.md.
    //
    // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
    // left in COPY_SOURCE after capture and stays there for every restore.
    {
        const bool hold = cfg.DlssNrHoldFrame.value_or_default();

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();
            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth ||
                                     td.Height != g_nr.heldHeight || td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                // restore -- target already holds the frame to freeze, and the pass runs on it.
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    const D3D12_RESOURCE_STATES priorTargetState = targetState;
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    TransitionTarget(priorTargetState);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                // Held: restore the frozen frame onto the live output before the encode reads it.
                const D3D12_RESOURCE_STATES priorTargetState = targetState;
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, g_nr.heldColor);
                TransitionTarget(priorTargetState);
            }

            // Suspend white-point measurement while held: use the snapshot so it cannot drift and
            // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
            if (g_nr.heldActive)
                whitePoint = g_nr.heldWhitePoint;
        }
        else if (g_nr.heldActive)
        {
            // Released: let go of the frozen frame and resume live input next frame.
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
        }
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.InputEncoding = frame.InputEncoding;
    fillLiveWhitePoint(encodeParams);
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    // Linear stores the light as it is, and the proxy takes the colour's own format: in 8 or 10 bits that leaves too
    // few codes in the shadows, so they band (DlssNr_ProxyCurve.h). Said once per format per session; the curve runs.
    if (encodeParams.ReversibleMode == DlssNrProxyCurve::kLinear && isHdrBuffer)
    {
        static std::set<DXGI_FORMAT> warnedLinearFormats; // render thread only
        const DXGI_FORMAT proxyFormat = g_nr.colorCopy->GetDesc().Format;
        if (DlssNrProxyCurve::LinearBandsIn(proxyFormat) && warnedLinearFormats.insert(proxyFormat).second)
            LOG_WARN("DLSS-NR Linear curve on a {}-bit buffer ({}): shadows will band",
                     proxyFormat == DXGI_FORMAT_R10G10B10A2_UNORM || proxyFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS ? 10 : 8,
                     (int) proxyFormat);
    }
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, exposureTex,
                        g_nr.colorCopy, g_nr.hdrCopy);

    if (targetSupportsUav)
        TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Is this frame a cut from the last one (DlssNr_SceneCut.inl)? Judged on the frame the encode just wrote, which is
    // display-referred whatever the game's encoding. With SceneCut=2, Reuse detail reads the answer as its distrust.
    ID3D12Resource* const sceneCutDistrust =
        SceneCut::Run(cmdList, device, cfg, frame, g_nr.colorCopy,
                      TranslateTypelessFormats(g_nr.colorCopy->GetDesc().Format));
    // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
    // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
    // of a 4096-thread dispatch, a readback and an nth_element every frame.)

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CalibrationCopyInput(cmdList, device, g_nr.hdrCopy);

    // Frame statistics diagnostic: on the frame it queued a sample, meter the proxy the encode just wrote (an
    // sRGB-encoded picture, so the tile means are of encoded luma) and queue that readback too. What the model
    // is really shown, measured rather than computed from the white point.
    if (g_nr.diagQueuedAt == g_frames && g_nr.diagProxyReadback != nullptr && g_nr.meter != nullptr)
    {
        DlssNrConstants proxyParams {};
        proxyParams.Mode = DlssNrMode_Meter;
        proxyParams.Width = kDlssNrMeterGrid;
        proxyParams.Height = kDlssNrMeterGrid;
        proxyParams.MeterCopiesExposure = 0;
        DispatchPass(cmdList, proxyParams, g_nr.colorCopy, nullptr, nullptr, nullptr, nullptr, g_nr.meter, nullptr);
        CopyMeterGridTo(cmdList, g_nr.diagProxyReadback);
        g_nr.diagProxyQueued = true;
    }

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    bool superiorInputReady = true;
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && (!spatial || replaceCorrection) && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
            // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
            // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
            // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
            // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
            // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
            // of Output Scaling, so the two can run different filters at once. superDown is built here
            // and used after the model (the down-leg below).
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler)
            {
                if (g_nr.superUp != nullptr)   { delete g_nr.superUp;   g_nr.superUp = nullptr; }
                if (g_nr.superDown != nullptr) { delete g_nr.superDown; g_nr.superDown = nullptr; }
                g_nr.nrScaler = nrScaler;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (g_nr.superUp != nullptr &&
                g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                // Wanted to supersample but the upscaler was not available -- warn once; the box path
                // below can only enlarge blockily, so the user should know the clean path is off.
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            // Needed so the shader's tap-decode (DecodeProxyToLinear/EncodeLinearToProxy, averaging
            // in true linear light instead of biasing toward the curve's own concavity) knows which
            // curve the proxy it's reading is actually in -- left default-zero before, which happened
            // to be harmless only because the old box-average never branched on either field.
            down.Passthrough = isHdrBuffer ? 0u : 1u;
            down.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
            if (superior)
            {
                DlssNr::Superior::DlssNrConstants area {};
                area.Mode = 2u;
                area.Width = workWidth;
                area.Height = workHeight;
                area.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
                superiorInputReady = DispatchSuperiorPass(cmdList, area, modelInput, nullptr, nullptr, nullptr, nullptr,
                                                          nullptr, g_nr.colorSmall, nullptr);
            }
            else
                DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr, g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    // Read the exposure scan's candidates on the pass's own command list, once a frame, with what is known about the
    // frame's brightness for its recording: Automatic's raw reading (Automatic only) and the game's exposure (read on
    // Game exposure, paired on Automatic while scanning).
    {
        DlssNr::ExposureScan::FrameExposure scanFrame;
        scanFrame.handed = (ID3D12Resource*) frame.ExposureTexture;
        scanFrame.meter = whitePointSource == 3 ? g_nr.autoExposureRawValue : 0.0f;
        scanFrame.game = whitePointSource == 1   ? g_nr.gameExposure
                         : whitePointSource == 3 ? g_nr.autoPairGameExposure
                                                 : 0.0f;
        // The pre-exposure of the frame the meter read (it is read back a few frames late), not this frame's
        scanFrame.pre = whitePointSource == 3 ? g_nr.autoExposurePreExposure : frame.PreExposure;
        // Timed only on frames that record copies: an empty pair would pull the average toward 0
        const bool timeScan = g_scanTime != nullptr && DlssNr::ExposureScan::Holding();

        if (timeScan)
            g_scanTime->Start(cmdList);

        const bool copied = DlssNr::ExposureScan::Tick(device, cmdList, scanFrame);

        if (timeScan && copied)
            g_scanTime->End(cmdList);
        else if (timeScan)
            g_scanTime->Abandon();
    }

    // No depth gives depthIn == nullptr; only a depth that could not be cloned is a failure.
    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

    if (motionIn == nullptr || (depth != nullptr && depthIn == nullptr))
    {
        g_nr.failed = true;
        g_nr.reason = motionIn == nullptr ? "the game's motion vectors could not be made readable"
                                          : "the game's depth could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        FinishColor(false);
        device->Release();
        return;
    }

    // Compress screen edges: pack the proxy, the depth and the motion into the smaller picture the model works on. The
    // vectors are taken to native pixels with the game's scale first, and the packed ones are exact in packed pixels.
    ID3D12Resource* modelDepth = depthIn;
    ID3D12Resource* modelMotion = motionIn;

    // What the uncompressed path would show the model at this Model resolution, on the ordinary grid: the encoded proxy
    // itself at 100%, else the picture built just above the way that path builds it (box downsample below 100%, the NR
    // upscaler above), from the same encoded proxy. Only Replace needs it.
    ID3D12Resource* spatialReference = modelInput;

    if (spatial)
    {
        using DlssNr::Spatial::MakeConstants;
        const auto& spatialLayout = EdgeCompression::Layout();
        const DlssNr::GuideExtent motionReference =
            frame.MotionVectorsLowResolution
                ? DlssNr::GuideExtent { frame.RenderSubrectWidth, frame.RenderSubrectHeight }
                : DlssNr::GuideExtent { frame.OutputWidth, frame.OutputHeight };
        auto guideConstants = DlssNr::Spatial::MakeEnlargementGuideConstants(
            superior, spatialGuideLayout, guides, motionReference, g_nr.guideMvScaleX, g_nr.guideMvScaleY);
        guideConstants.padding[0] = guides.depth.valid() ? 1.f : 0.f;

        if (!guides.depth.valid())
        {
            guideConstants.depthRect[0] = guideConstants.depthRect[1] = 0.0f;
            guideConstants.depthRect[2] = guideConstants.depthRect[3] = 1.0f;
        }

        const bool packedColor =
            DispatchSpatial(cmdList, MakeConstants(spatialLayout, 100, guides, 1.0f, 1.0f), g_nr.colorCopy, nullptr,
                            nullptr, EdgeCompression::color);
        const bool packedGuides =
            packedColor && DispatchSpatial(cmdList, guideConstants, g_nr.colorCopy, depthIn, motionIn,
                                           EdgeCompression::depth, EdgeCompression::motion, superior ? 2u : 0u);

        if (!packedGuides)
        {
            // The frame is given up, not NR: put everything back as the next frame finds it, and run it ordinary.
            EdgeCompression::TurnOff(DlssNr::Spatial::Status::TurnedOffDispatch);
            g_nr.reset = true;
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // The Replace reference was left readable by the block that built it; the end of a normal frame puts it back.
            if (reduced && replaceCorrection && g_nr.colorSmall != nullptr)
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            FinishColor(false);

            if (g_gpuTime != nullptr)
                g_gpuTime->End(cmdList);

            if (g_nr.depthClone != nullptr && depthIn == g_nr.depthClone)
                Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);

            if (g_nr.motionClone != nullptr && motionIn == g_nr.motionClone)
                Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);

            device->Release();
            return;
        }

        for (ID3D12Resource* packed : { EdgeCompression::color, EdgeCompression::depth, EdgeCompression::motion })
            Barrier(cmdList, packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        modelInput = EdgeCompression::color;
        modelDepth = depth != nullptr ? EdgeCompression::depth : nullptr;
        modelMotion = EdgeCompression::motion;
    }

    // Experimental input styles (Config.h DlssNrInputFlatten ..., DlssNr_InputStyles.h): restyle the model's picture once
    // it is final (after the shrink and the edge packing), each style that is on in turn (flatten, then structure hint),
    // each on the one before's result, alternating between two textures. The last result is both the
    // model input and the resolve's proxy (ordinaryProxy below, and through the edge unpacking), so Composed hands back
    // only what the model painted on top of it. Skipped above 100% model resolution (the resolve compares against
    // colorCopy there) and on the deferred path.
    bool inputRestyled = false;
    {
        const DlssNrInput::Styles styles = cfg.DlssNrInputStyles();
        DlssNrInput::Pass passes[2];
        const int passCount = DlssNrInput::Passes(styles, passes);

        const char* skip = passCount == 0                              ? nullptr
                           : cfg.DlssNrDeferredDlss.value_or_default() ? "the deferred Super Resolution path"
                           : workScale > 1.0f                          ? "model resolution above 100%"
                                                                       : nullptr;
        int ran = 0;

        if (passCount > 0 && skip == nullptr)
        {
            const D3D12_RESOURCE_DESC in = modelInput->GetDesc();

            // One texture for one style, two to alternate between for more (the second is let go while one is enough)
            ID3D12Resource** targets[2] = { &g_nr.styled, &g_nr.styledB };
            D3D12_RESOURCE_STATES* targetStates[2] = { &g_nr.styledState, &g_nr.styledBState };

            if (passCount < 2 && g_nr.styledB != nullptr)
            {
                ParkNrResource(g_nr.styledB);
                g_nr.styledBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }

            for (int t = 0; t < (passCount > 1 ? 2 : 1); ++t)
            {
                ID3D12Resource*& target = *targets[t];

                if (target != nullptr && (target->GetDesc().Width != in.Width || target->GetDesc().Height != in.Height ||
                                          target->GetDesc().Format != in.Format))
                {
                    ParkNrResource(target);
                    *targetStates[t] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }

                if (target == nullptr)
                {
                    target = CreateScratch(device, in.Format, (unsigned int) in.Width, in.Height);
                    *targetStates[t] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            }

            ID3D12Resource* source = modelInput;

            for (int k = 0; k < passCount; ++k)
            {
                const uint32_t style = passes[k].style;
                ID3D12Resource* target = *targets[k % 2];
                D3D12_RESOURCE_STATES& targetState = *targetStates[k % 2];

                if (target == nullptr)
                {
                    skip = "its texture could not be created";
                    break;
                }

                Barrier(cmdList, target, targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                DlssNrConstants paint {};
                paint.Mode = StyleMode(style);
                paint.Width = (unsigned int) in.Width;
                paint.Height = in.Height;
                paint.Passthrough = isHdrBuffer ? 0u : 1u;
                paint.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
                paint.TransferStrength = passes[k].strength;
                paint.SkinProtection = styles.facesOnly ? 1u : 0u; // the style passes read it as "faces only"

                const bool painted = DispatchPass(cmdList, paint, source, nullptr, nullptr, nullptr, nullptr, target, nullptr);

                Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                targetState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

                // A failed dispatch left its texture as it was: the model gets the picture up to the last style that ran
                if (!painted)
                {
                    skip = "its dispatch failed";
                    break;
                }

                source = target;
                ++ran;
            }

            if (ran > 0)
            {
                modelInput = source;
                inputRestyled = true;
            }
        }

        // Said when it changes, so a log shows each A/B toggle.
        static std::string said = "-";
        std::string state;

        for (int k = 0; k < ran; ++k)
            state += std::format("{}{} {:.2f}", k == 0 ? "" : ", ", StyleName(passes[k].style), passes[k].strength);

        if (skip != nullptr)
            state += std::format("{}skipped ({})", ran > 0 ? "; then " : "", skip);

        if (state != said)
        {
            said = state;

            if (ran > 0)
                LOG_INFO("DLSS-NR input style: {} ({}x{}{}{})", state, modelInput->GetDesc().Width,
                         modelInput->GetDesc().Height, styles.facesOnly ? ", faces only" : "",
                         spatial ? ", edge-compressed picture" : "");
            else if (skip != nullptr)
                LOG_INFO("DLSS-NR input style: {}", state);
            else
                LOG_INFO("DLSS-NR input style: off");
        }
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    // The vectors were scaled to full-frame pixels; the image the model reprojects is the
    // working size.
    const float mvToWorkX = width != 0 ? (float) workWidth / (float) width : 1.0f;
    const float mvToWorkY = height != 0 ? (float) workHeight / (float) height : 1.0f;


    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, g_nr.output, workWidth, workHeight,
            guideWidth, guideHeight, motionWidth, motionHeight, depthBaseX, depthBaseY,
            motionBaseX, motionBaseY, g_nr.guideDepthInverted, g_nr.reset,
            g_nr.guideMvScaleX * mvToWorkX, g_nr.guideMvScaleY * mvToWorkY);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session",
                      proxyResult, NgxResultName(proxyResult));
        }

        FinishColor(false);
        device->Release();
        return;
    }

    // Count only a contiguous set of ready, separate feature histories. A failed extra creation never
    // falls back to reusing the main feature: that tells one temporal model several frames elapsed in
    // one game frame and makes its history fight the later layers.
    unsigned int effectivePasses = 1;
    if (g_nr.passScratch != nullptr && g_nr.passClampScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] == nullptr || g_nr.passPendingSubmission[pass])
                break;
            ++effectivePasses;
        }
    }

    // A "Tune for this scene" run measures the first pass alone: it is the only one that sees the game's frame, the later
    // ones refine the model's own answer and make up whatever detail it missed, which flattens the measure until only the
    // shadow and highlight penalties decide (3 passes in NBA 2K27 picked +1 EV over a flat curve). The result then holds
    // for any pass count. The later passes keep their features and sit the run out; they restart their history after it,
    // as after any skipped frame.
    if (calibrationPinned && effectivePasses > 1)
    {
        for (unsigned int skipped = 1; skipped < effectivePasses; ++skipped)
            g_nr.passNeedsReset[skipped] = true;

        effectivePasses = 1;
    }

    {
        static unsigned int loggedConfigured = 0;
        static unsigned int loggedEffective = 0;
        if (loggedConfigured != configuredPasses || loggedEffective != effectivePasses)
        {
            loggedConfigured = configuredPasses;
            loggedEffective = effectivePasses;
            LOG_INFO("DLSS-NR model passes: configured {}, effective {}", configuredPasses,
                     effectivePasses);
        }
    }

    auto* superiorGeneration = superior ? superiorState.current.get() : nullptr;
    const auto superiorGainScales = superior ? DlssNr::SuperiorScaling::MakeFrequencyScales(
                                                   width, height, workWidth, workHeight, modelWidth, modelHeight)
                                             : DlssNr::SuperiorScaling::FrequencyScales { workScale, workScale };
    const auto superiorSchedule = superior ? DlssNr::SuperiorScaling::Schedule(cfg, effectivePasses, superiorGainScales,
                                                                               g_nr.superiorMainOnlyGains)
                                           : DlssNr::InterPass::GainSchedule {};
    unsigned superiorCovered = 0, superiorCompleted = 0;
    bool superiorBoundaryFailed = false;
    uint32_t superiorMask = 0, superiorClampSlots[4] { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    if (superiorGeneration)
    {
        auto& g = *superiorGeneration;
        if (!g.haveSchedule || g.expectedMask != superiorSchedule.expectedMask || g.previousPasses != effectivePasses)
        {
            g_nr.reset = true;
            g.reset = true;
            g.haveSchedule = true;
            g.expectedMask = superiorSchedule.expectedMask;
            g.previousPasses = effectivePasses;
        }
    }

    // Encode happened once above. Keep that base proxy immutable and ping-pong only model answers:
    //   pass 0: base -> A, pass 1: A -> B, pass 2: B -> A.
    // The final answer is resolved once against the original base, so matched-residual transfer is the
    // cumulative final-minus-base edit and colour/transfer controls are not compounded.
    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = g_nr.output;
    ID3D12Resource* passClampTarget = g_nr.passClampScratch;
    ID3D12Resource* finalAnswer = nullptr;
    bool outputReadable = false;
    bool scratchReadable = false;
    bool clampReadable = false;
    bool clamp2Readable = false;

    // g_nr.passClampScratch/passClampScratch2 are a third and fourth participant in this same
    // UAV/NPSR dance: an intermediate pass's raw answer lands in whichever one is the current
    // passClampTarget, saturated against the proxy it replaces, and is read back as the next pass's
    // input -- written and read again at most once per remaining intermediate boundary, exactly like
    // output/passScratch. Two of them, ping-ponging, because the clamp step now also reads the
    // *previous* boundary's clamped proxy (as gModel, to rescale the edit against) while writing the
    // next one -- with only one buffer the second boundary would alias its own read and write.
    //
    // The fallthrough below assumes the only resource this lambda is ever called with, besides the
    // three named explicitly, is g_nr.passScratch -- true for every call site in this function today.
    // A future call site passing anything else here (colorSmall, activeColor, ...) would silently
    // share passScratch's tracked state instead of getting its own, with no compiler or runtime
    // signal -- add it as a named case above rather than relying on the fallthrough.
    const auto ReadableFlag = [&](ID3D12Resource* resource) -> bool&
    {
        if (resource == g_nr.output)
            return outputReadable;
        if (resource == g_nr.passClampScratch)
            return clampReadable;
        if (resource == g_nr.passClampScratch2)
            return clamp2Readable;
        return scratchReadable;
    };

    const auto MakeModelReadable = [&](ID3D12Resource* resource)
    {
        bool& readable = ReadableFlag(resource);
        if (!readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            readable = true;
        }
    };

    const auto MakeModelWritable = [&](ID3D12Resource* resource)
    {
        bool& readable = ReadableFlag(resource);
        if (readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            readable = false;
        }
    };

    // A pass whose own feature already exists but that skips evaluating this frame -- because the
    // chain stopped short at an earlier boundary -- must not resume next time with passReset=false:
    // NGX would then treat the skipped frame(s) as continuous history instead of a gap, the same
    // ghosting risk a camera cut or a freshly-created layer already guards against.
    const auto ArmSkippedPassResets = [&](unsigned int firstSkipped)
    {
        for (unsigned int skipped = firstSkipped; skipped < effectivePasses; ++skipped)
            g_nr.passNeedsReset[skipped] = true;
    };

    int result = superiorInputReady ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;

    // What the model is handed as its guides: the game's, at the game's sizes and scale; or packed, where all three are
    // the packed picture's own (motion in packed pixels, so a scale of 1).
    unsigned int evalGuideW = spatial ? spatialGuideLayout.modelW : guideWidth;
    unsigned int evalGuideH = spatial ? spatialGuideLayout.modelH : guideHeight;
    unsigned int evalMotionW = spatial ? spatialGuideLayout.modelW : motionWidth;
    unsigned int evalMotionH = spatial ? spatialGuideLayout.modelH : motionHeight;
    unsigned int evalDepthX = spatial ? 0u : depthBaseX;
    unsigned int evalDepthY = spatial ? 0u : depthBaseY;
    float evalMvScaleX = spatial ? 1.0f : g_nr.guideMvScaleX * mvToWorkX;
    float evalMvScaleY = spatial ? 1.0f : g_nr.guideMvScaleY * mvToWorkY;

    // Reuse detail between frames (DlssNr_DetailReuse.inl): every other frame skips the model and moves the previous
    // frame's detail onto this frame's input instead. On such a frame the answer lands in g_nr.output (at rest, UAV).
    // The frame as reuse sees it: a scene cut's distrust where the frame has no history distrust of its own.
    DlssNrFrameInfo reuseInfo = frame;
    if (reuseInfo.HistoryDistrust == nullptr && sceneCutDistrust != nullptr)
        reuseInfo.HistoryDistrust = sceneCutDistrust;

    DetailReuse::Frame reuseFrame;
    reuseFrame.pass = this;
    reuseFrame.cmdList = cmdList;
    reuseFrame.device = device;
    reuseFrame.cfg = &cfg;
    reuseFrame.info = &reuseInfo;
    reuseFrame.answerFormat = colourFormat;
    reuseFrame.workWidth = workWidth;
    reuseFrame.workHeight = workHeight;
    reuseFrame.motionWidth = motionWidth;
    reuseFrame.motionHeight = motionHeight;
    reuseFrame.motionBaseX = motionBaseX;
    reuseFrame.motionBaseY = motionBaseY;
    reuseFrame.motionAllocWidth = (unsigned int) motionDesc.Width;
    reuseFrame.motionAllocHeight = motionDesc.Height;
    reuseFrame.depthWidth = guideWidth;
    reuseFrame.depthHeight = guideHeight;
    reuseFrame.depthBaseX = depthBaseX;
    reuseFrame.depthBaseY = depthBaseY;
    reuseFrame.depthInverted = g_nr.guideDepthInverted;
    reuseFrame.passthrough = !isHdrBuffer;
    reuseFrame.reversibleMode = encodeParams.ReversibleMode;
    reuseFrame.mvScaleX = g_nr.guideMvScaleX;
    reuseFrame.mvScaleY = g_nr.guideMvScaleY;
    reuseFrame.modelInput = modelInput;
    reuseFrame.motion = motionIn;
    reuseFrame.depth = depthIn;
    reuseFrame.output = g_nr.output;
    reuseFrame.modelReset = g_nr.reset;
    reuseFrame.edgeCompression = spatial; // the two do not run together; Reuse detail says so in the menu
    // A Tune step pins the white point (calibrationPinned); a Measure detail run copies and measures without pinning,
    // and measures Reuse bottleneck as it runs.
    // A capture shows the model on every frame: before/after are a control, and an edit cache dump measures carrying
    // the model's real answers
    reuseFrame.blocked = calibrationPinned || g_nr.heldActive || g_capture.isActive();
    reuseFrame.frameNumber = frame.SubmissionEpoch != 0 ? frame.SubmissionEpoch : g_frames;
    {
        // The motion size is left out on purpose: saved vectors are uv displacements, so a render-size change does not
        // invalidate them.
        unsigned long long revision = (unsigned long long) (uintptr_t) g_nr.feature;
        for (const unsigned long long part :
             { g_nr.featureCreateEpoch, (unsigned long long) effectivePasses, (unsigned long long) workWidth,
               (unsigned long long) workHeight })
            revision = revision * 1000003ull ^ part;
        reuseFrame.revision = revision;
    }
    const DetailReuse::Plan reusePlan = DetailReuse::BeforeModel(reuseFrame);
    const bool reused = reusePlan.reused;
    if (reused)
    {
        finalAnswer = g_nr.output;
        MakeModelReadable(finalAnswer);
    }
    if (reusePlan.resetModel)
        g_nr.reset = true;

    // The edit cache (DlssNr_EditCache.inl): the model one frame in N, its edit carried along the vectors in between.
    // Every frame its vectors go into the ring; a refresh runs the model (with the vectors back to its last run) and
    // becomes the anchor after the resolve; a cached frame skips the model and the resolve, and ApplyCached writes the
    // output from the carried edit. It stands aside (every frame runs the model) where it cannot run, and never runs
    // beside Reuse detail (every other frame).
    EditCache::Frame cacheFrame;
    cacheFrame.pass = this;
    cacheFrame.cmdList = cmdList;
    cacheFrame.device = device;
    cacheFrame.cfg = &cfg;
    {
        auto& ci = cacheFrame.inputs;
        ci.enabled = cfg.DlssNrEditCache.value_or_default();
        ci.interval = cfg.DlssNrEditCacheInterval.value_or_default();
        ci.crossfade = cfg.DlssNrEditCacheCrossfade.value_or_default();
        ci.detailReuse = cfg.DlssNrDetailReuse.value_or_default();
        ci.afterUpscale = !frame.BeforeUpscale && !frame.FinishedPicture;
        ci.targetWritable = targetSupportsUav;
        ci.workAbove100 = workScale > 1.0f;
        ci.edgeCompression = spatial;
        ci.convertedEncoding = frame.InputEncoding != 0;
        ci.blocked = calibrationPinned || g_nr.heldActive || g_capture.isActive() || CalibrationActive();
        // The skin mask preview is painted by the resolve: carried, it would be taken for the model's edit
        ci.debugOrCompare = cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrCompare.value_or_default() != 0 ||
                            cfg.DlssNrShowSkinMask.value_or_default();
        ci.reset = g_nr.reset;
        ci.revision = reuseFrame.revision;
        ci.width = width;
        ci.height = height;
        ci.motionWidth = motionWidth;
        ci.motionHeight = motionHeight;
        ci.clock = reuseFrame.frameNumber;
    }
    cacheFrame.motionBaseX = motionBaseX;
    cacheFrame.motionBaseY = motionBaseY;
    cacheFrame.motionAllocWidth = (unsigned int) motionDesc.Width;
    cacheFrame.motionAllocHeight = motionDesc.Height;
    cacheFrame.depthWidth = guideWidth;
    cacheFrame.depthHeight = guideHeight;
    cacheFrame.depthBaseX = depthBaseX;
    cacheFrame.depthBaseY = depthBaseY;
    cacheFrame.depthInverted = g_nr.guideDepthInverted;
    cacheFrame.mvScaleX = g_nr.guideMvScaleX;
    cacheFrame.mvScaleY = g_nr.guideMvScaleY;
    cacheFrame.passthrough = !isHdrBuffer;
    cacheFrame.white = encodeParams.WhitePoint;
    cacheFrame.frame = g_nr.hdrCopy;
    cacheFrame.motion = motionIn;
    cacheFrame.depth = depthIn;
    const EditCache::Plan cachePlan = EditCache::BeforeModel(cacheFrame);
    const bool cached = cachePlan.cached;
    if (cachePlan.resetModel)
        g_nr.reset = true;
    ID3D12Resource* evalMotion = cachePlan.runs ? cachePlan.motion : reusePlan.motion;
    unsigned int evalMotionBaseX = cachePlan.runs ? cachePlan.motionBaseX : reusePlan.motionBaseX;
    unsigned int evalMotionBaseY = cachePlan.runs ? cachePlan.motionBaseY : reusePlan.motionBaseY;

    bool superiorMatched = false;
    if (superiorGeneration && !superiorPacked && !cached &&
        (superiorGeneration->temporal || workWidth != width || workHeight != height))
    {
        auto matchedRegions = guides;
        matchedRegions.motion.x = evalMotionBaseX;
        matchedRegions.motion.y = evalMotionBaseY;
        // Uncompressed Superior matches its guides once; compressed NR/private DLSS share packed guides.
        const bool ok =
            SuperiorRun::MatchGuides(*superiorGeneration, *this, cmdList, depthIn, spatial ? motionIn : evalMotion,
                                     spatial ? guides : matchedRegions, frame);
        if (!ok)
            result = NVSDK_NGX_Result_Fail;
        else if (!spatial)
        {
            superiorMatched = true;
            modelDepth = superiorGeneration->depth.Get();
            evalMotion = superiorGeneration->motion.Get();
            evalGuideW = evalMotionW = workWidth;
            evalGuideH = evalMotionH = workHeight;
            evalDepthX = evalDepthY = evalMotionBaseX = evalMotionBaseY = 0;
        }
    }
    if (superiorMatched)
    {
        // NR reads displacement in pixels of its motion subrect: the matched grid. Only when the temporal path matched
        // the guides above; otherwise ordinary NR keeps its own guideMvScale * work / width units (see g_nr.guideMvScaleX).
        evalMvScaleX = DlssNr::SuperiorScaling::MotionScale(
            frame.MvScaleX, evalMotionW,
            frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth, guides.motion.width);
        evalMvScaleY = DlssNr::SuperiorScaling::MotionScale(
            frame.MvScaleY, evalMotionH,
            frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight, guides.motion.height);
    }
    const bool superiorReset = g_nr.reset || frame.Reset;

    // The model's own time: the passes alone, so the detail reuse work above counts as surrounding work (and a reused
    // frame's model time is about zero).
    if (g_ngxTime != nullptr)
        g_ngxTime->Start(cmdList);

    for (unsigned int pass = 0; !reused && !cached && pass < effectivePasses && result == NVSDK_NGX_Result_Success;
         ++pass)
    {
        void* const passFeature = pass == 0 ? g_nr.feature : g_nr.passFeature[pass];
        const bool passReset = g_nr.reset || (pass > 0 && g_nr.passNeedsReset[pass]);
        const auto tuning = PassTuning(cfg, pass);

        MakeModelWritable(passOutput);
        // ViT reuse of the NVIDIA model: tell the NvAPI wrapper which feature this is, whether it starts over, how often to compute the bottleneck,
        // and the frame slot (successfulDispatches counts NR frames and is constant across one frame's passes, so all passes compute on the
        // same frame and all reuse on the next; see DlssNrVitReuse.h for why the passes must not be offset)
        // While detail reuse runs, full frames are every other frame: a ViT slot keyed to them would compute only
        // every fourth game frame (or never, on the odd parity), so the bottleneck is computed every time.
        // The edit cache likewise: the model runs one frame in N.
        // Copy mode (DlssNrVitReuse.h): the plain set always reuses through our own copy of the ViT output. A Vulkan game
        // reaching this through the D3D12 bridge (IFeature_VkwDx12) uses it for the fp8 set too: on the native Vulkan path
        // the kept result was overwritten between frames (dark scenes flashed), and the copy does not depend on it staying.
        const bool vitEveryFrame = reusePlan.active || cachePlan.runs;
        const unsigned copySets =
            DlssNrVitReuse::kCopyPlain | (State::Instance().api == Vulkan ? DlssNrVitReuse::kCopyFp8 : 0u);
        DlssNrNative::BeginEvaluate(passFeature, passReset,
                                    vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEvery.value_or_default(), 1u, 2u),
                                    vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEveryPlain.value_or_default(), 1u, 2u),
                                    (long long) (g_nr.successfulDispatches & 0x3FFFFFFFFFFFFFFFull), cmdList,
                                    cfg.DlssNrKernelProfile.value_or_default(), copySets);
        result = g_nr.evaluate(
            cmdList, passFeature, g_nr.capabilityParams, passInput, modelDepth, spatial ? modelMotion : evalMotion,
            passOutput, modelWidth, modelHeight, evalGuideW, evalGuideH, evalMotionW, evalMotionH,
            evalDepthX, evalDepthY, spatial ? 0u : evalMotionBaseX, spatial ? 0u : evalMotionBaseY,
            g_nr.guideDepthInverted ? 1 : 0,
            passReset ? 1 : 0, tuning.intensity,
            (int) PassStyle(cfg, pass), tuning.structure,
            tuning.tone, tuning.skin,
            tuning.autoMask ? 1 : 0, evalMvScaleX,
            evalMvScaleY);
        if (DlssNrNative::EndEvaluate(cmdList))
            LOG_WARN("DLSS-NR: Reuse bottleneck is {}", DlssNrNative::VitStatus());

        for (const std::string& report : DlssNrNative::TakeProfileReports())
            LOG_INFO("{}", report);

        if (result != NVSDK_NGX_Result_Success)
            break;

        if (pass > 0)
            g_nr.passNeedsReset[pass] = false;

        finalAnswer = passOutput;
        MakeModelReadable(finalAnswer);
        superiorCompleted = pass + 1u;
        if (superiorGeneration && (superiorGeneration->temporal || (spatial && superiorGainScales.low < 1.0f)) &&
            pass + 1u < effectivePasses)
        {
            const bool due = superiorSchedule.Due(pass, superiorCovered);
            const bool ran =
                due &&
                SuperiorRun::InterPass(*superiorGeneration, *this, cmdList, cfg, spatial ? modelInput : g_nr.colorCopy,
                                       modelInput, finalAnswer, superiorCovered, superiorCompleted, superiorGainScales);
            if (ran)
            {
                superiorCovered = superiorCompleted;
                superiorMask |= 1u << pass;
            }
            else if (due)
            {
                superiorBoundaryFailed = true;
                g_nr.reset = true;
                superiorGeneration->reset = true;
            }
            auto* nextInput = ran ? superiorGeneration->interWorking.Get() : finalAnswer;
            const float feedback = std::clamp(cfg.DlssNrPassFeedback.value_or_default(), 0.0f, 1.0f);
            if (!ran || feedback != 1.0f)
            {
                if (!passClampTarget)
                {
                    result = NVSDK_NGX_Result_Fail;
                    ArmSkippedPassResets(pass + 1u);
                    break;
                }
                MakeModelWritable(passClampTarget);
                bool clamped = false;
                if (feedback == 1.0f)
                {
                    DlssNr::Superior::DlssNrConstants clamp {};
                    clamp.Mode = 8u;
                    clamp.Width = modelWidth;
                    clamp.Height = modelHeight;
                    const auto slot =
                        (passClampTarget == g_nr.passClampScratch ? 0u : 2u) + (finalAnswer == g_nr.output ? 0u : 1u);
                    clamped = DispatchSuperiorPass(cmdList, clamp, nextInput, nullptr, nullptr, nullptr, nullptr,
                                                   nullptr, passClampTarget, nullptr, &superiorClampSlots[slot]);
                }
                else
                {
                    DlssNrConstants clamp {};
                    clamp.Mode = DlssNrMode_ClampProxy;
                    clamp.Width = modelWidth;
                    clamp.Height = modelHeight;
                    clamp.PassFeedback = feedback;
                    clamped = DispatchPass(cmdList, clamp, nextInput, passInput, nullptr, nullptr, nullptr,
                                           passClampTarget, nullptr);
                }
                if (!clamped)
                {
                    result = NVSDK_NGX_Result_Fail;
                    ArmSkippedPassResets(pass + 1u);
                    break;
                }
                MakeModelReadable(passClampTarget);
                nextInput = passClampTarget;
                passClampTarget =
                    passClampTarget == g_nr.passClampScratch ? g_nr.passClampScratch2 : g_nr.passClampScratch;
            }
            passInput = nextInput;
            passOutput = passOutput == g_nr.output ? g_nr.passScratch : g_nr.output;
            continue;
        }

        if (pass + 1 < effectivePasses)
        {
            // passClampScratch2 is only allocated when the base clamp buffer (passClampScratch) is,
            // not gated into whether multipass runs at all (a 2-pass chain, one boundary, never needs
            // it) -- so a transient allocation failure on just this second buffer must not disable a
            // 2-pass chain that would otherwise have worked. Ending the chain here, one pass short of
            // requested, is the same graceful-degradation shape as the feature-readiness check above
            // (a "ready contiguous prefix", never a hard failure over one missing extra layer).
            if (passClampTarget == nullptr)
            {
                static bool warnedNoClampTarget = false;
                if (!warnedNoClampTarget)
                {
                    warnedNoClampTarget = true;
                    LOG_WARN("DLSS-NR: second interpass clamp target unavailable; stopping at {} pass(es)",
                             pass + 1);
                }
                ArmSkippedPassResets(pass + 1);
                break;
            }

            // The model's raw answer is not guaranteed to stay in the [0,1]-per-channel range the
            // encode step promised it as an input (the once-per-frame resolve guard below exists for
            // exactly this reason). Restore that range here too, so an out-of-range intermediate
            // answer cannot compound across the remaining passes. Scaled back via CubeScaleResidual
            // against this pass's own proxy (passInput, already guaranteed valid) rather than a
            // per-channel saturate: a per-channel clamp is a hue distorter (the smallest channel hits
            // the bound first), the same reason the Replace guard and Composed boundedRatio elsewhere
            // in this file both rescale by one scalar instead of clamping channels independently.
            MakeModelWritable(passClampTarget);
            DlssNrConstants clampParams {};
            clampParams.Mode = DlssNrMode_ClampProxy;
            clampParams.Width = modelWidth;
            clampParams.Height = modelHeight;
            // Clamped here, not trusted from the ini: the shader treats PassFeedback as a convex
            // blend weight between two values it has already guaranteed are in the unit cube, and
            // that guarantee only holds for a weight in [0,1]. A hand-edited value outside it would
            // push the result back out of range, which is exactly what this whole boundary exists
            // to prevent.
            clampParams.PassFeedback =
                std::clamp(cfg.DlssNrPassFeedback.value_or_default(), 0.0f, 1.0f);
            if (!DispatchPass(cmdList, clampParams, finalAnswer, passInput, nullptr, nullptr, nullptr,
                                passClampTarget, nullptr))
            {
                // The buffer was never actually written -- feeding it to the next pass as input would
                // hand NGX stale or uninitialized data, the opposite of what this clamp exists to
                // prevent. Stop the chain here instead, same as the null-target case above.
                static bool warnedClamp = false;
                if (!warnedClamp)
                {
                    warnedClamp = true;
                    LOG_WARN("DLSS-NR: interpass clamp dispatch failed; stopping at {} pass(es)", pass + 1);
                }
                ArmSkippedPassResets(pass + 1);
                break;
            }
            MakeModelReadable(passClampTarget);

            passInput = passClampTarget;
            passClampTarget =
                passClampTarget == g_nr.passClampScratch ? g_nr.passClampScratch2 : g_nr.passClampScratch;
            passOutput = passOutput == g_nr.output ? g_nr.passScratch : g_nr.output;
        }
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->End(cmdList);

    // Steady a full frame's answer and save this frame's history (DlssNr_DetailReuse.inl).
    DetailReuse::AfterModel(reuseFrame, reusePlan, result == NVSDK_NGX_Result_Success, finalAnswer);
    if (cachePlan.runs)
        EditCache::AfterModel(cacheFrame);

    if (superiorGeneration)
    {
        // Edit cache skips the private DLSS evaluation too. Its next refresh must
        // not reproject an older history with only the current frame's vectors.
        if (cached)
            superiorGeneration->reset = true;
        if (reused)
        {
            superiorCompleted = superiorGeneration->lastCompleted;
            superiorCovered = superiorGeneration->lastCovered;
        }
        if (superiorGeneration->successfulMask != superiorMask && !reused && !cached)
            superiorGeneration->reset = true;
        if (!reused && !cached)
        {
            superiorGeneration->successfulMask = superiorMask;
            superiorGeneration->lastCompleted = superiorCompleted;
            superiorGeneration->lastCovered = superiorCovered;
        }
    }
    g_nr.reset = superior && (superiorBoundaryFailed || result != NVSDK_NGX_Result_Success);

    // Legacy Compress and exposure calibration unpack the pair to the ordinary grid. Superior temporal reconstruction
    // instead reads the packed pair directly and combines unwarping with its final guided resolve.
    // In the ordinary path, both halves use the same resampling so their difference cancels the resampling loss.
    ID3D12Resource* ordinaryProxy = modelInput;
    ID3D12Resource* ordinaryAnswer = finalAnswer;
    bool spatialUnpacked = false;
    bool spatialFailed = false;

    if (spatial && (!superiorPacked || CalibrationActive()) && result == NVSDK_NGX_Result_Success &&
        finalAnswer != nullptr)
    {
        spatialUnpacked =
            EdgeCompression::PrepareUnpack(device, EdgeCompression::Layout()) &&
            DispatchSpatial(cmdList,
                            DlssNr::Spatial::MakeConstants(EdgeCompression::Layout(), replaceCorrection ? 103 : 102,
                                                           guides, 1.0f, 1.0f),
                            modelInput, finalAnswer, replaceCorrection ? spatialReference : nullptr,
                            EdgeCompression::proxy, EdgeCompression::answer);

        if (spatialUnpacked)
        {
            for (ID3D12Resource* unpacked : { EdgeCompression::proxy, EdgeCompression::answer })
                Barrier(cmdList, unpacked, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ordinaryProxy = EdgeCompression::proxy;
            ordinaryAnswer = EdgeCompression::answer;
        }
        else
        {
            EdgeCompression::TurnOff(DlssNr::Spatial::Status::TurnedOffDispatch);
            g_nr.reset = true;
            spatialFailed = true;
        }
    }

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (modelWidth > width || modelHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != modelWidth || result != 1)
        {
            lastSuper = modelWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})",
                     modelWidth, modelHeight, (float) modelWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    bool composed = false;

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240 && g_nr.capabilityParams != nullptr)
    {
        tuningReported = true;

        // This checks the parameter table, not whether the neural network uses a
        // setting. Multipass leaves the final pass's values in this shared table.
        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote,
                     (uint32_t) r);
        };

        const auto lastTuning = PassTuning(cfg, effectivePasses - 1);
        LOG_INFO("DLSS-NR parameter-table readback for pass {} (not proof of visual effect)", effectivePasses);
        report("DLSSNR.Intensity", lastTuning.intensity);
        report("DLSSNR.LocalStructureStrength", lastTuning.structure);
        report("DLSSNR.LocalToneStrength", lastTuning.tone);
        report("DLSSNR.SkinStructureStrength", lastTuning.skin);
        unsigned int autoMask = 0;
        const auto maskResult = g_nr.capabilityParams->Get("DLSSNR.UseAutoMask", &autoMask);
        LOG_INFO("DLSS-NR AutoMask readback: {} (wrote {}, result 0x{:X})", autoMask,
                 lastTuning.autoMask, (uint32_t) maskResult);

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, PassPreset(cfg, 0));

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 PassStyle(cfg, 0));
    }

    if (result == NVSDK_NGX_Result_Success && !spatialFailed)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve. (A forced gamma 2.2
        // or PQ Colour encoding is the exception: the frame is converted on the way in and back on the way out.)
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        fillLiveWhitePoint(resolveParams);
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        const auto strength = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
        resolveParams.SkinProtection = cfg.DlssNrSkinProtection.value_or_default();
        resolveParams.ShowSkinMask = cfg.DlssNrShowSkinMask.value_or_default();
        resolveParams.SkinDetail = strength(cfg.DlssNrSkinDetail.value_or_default());
        resolveParams.SkinColour = cfg.DlssNrSkinToneEnabled.value_or_default() ? strength(cfg.DlssNrSkinColour.value_or_default()) : 0.0f;
        resolveParams.EnvironmentDetail = strength(cfg.DlssNrEnvironmentDetail.value_or_default());
        resolveParams.EnvironmentColour = strength(cfg.DlssNrEnvironmentColour.value_or_default());
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        // Replace's detail injection puts back the game frame's own fine texture, which is what a style removed:
        // off while the picture was restyled, so Replace shows the model's painting of the restyled picture.
        resolveParams.ReplaceDetailStrength =
            inputRestyled ? 0.0f : cfg.DlssNrReplaceDetailStrength.value_or_default();
        resolveParams.ModelWorkScale = (reduced && workScale < 1.0f) ? workScale : 1.0f;
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = std::clamp(cfg.DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
        resolveParams.Transfer = cfg.DlssNrTransfer.value_or_default();
        // The shared shader knows Classic/Matched/NVIDIA residual only. Superior's own resolve PSOs fix their transfer
        // at compile time (DLSSNR_SUPERIOR_HOST_RESOLVE); every other resolve with Superior selected (100% without
        // compression, supersampling, or Superior given up on) runs Matched residual on purpose, not by accident.
        if (DlssNr::SuperiorScaling::Enabled(resolveParams.Transfer))
            resolveParams.Transfer = DlssNr::SuperiorScaling::FallbackTransfer;
        // The debug views' scale when the shader has no live exposure to take the base white point from: the white
        // point in force on a linear HDR frame, the Paper white slider on a tone-mapped one. See DebugViewScale in
        // dlssnr.hlsl, which prefers the base white point (before the Trim) whenever the exposure texture is bound.
        resolveParams.DebugScale = isHdrBuffer ? whitePoint : cfg.DlssNrWhitePointScale.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.InputEncoding = frame.InputEncoding;
        resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        // The numbers the composition actually ran with, logged when any of them changes.
        //
        // A colour report without these cannot be read. Paper white alone decides whether the model
        // was shown a sensible picture or a blown one, and it was absent from every log in the first
        // round of reports -- one tester's "much better at 16" had to be taken on trust because
        // nothing in the file said what the value was. Debug view and compare mode are here for the
        // same reason from the other direction: both change what is on screen, and a screenshot with
        // one left on is indistinguishable from a bug.
        struct ComposeReport
        {
            bool valid;
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
            unsigned int passes;
            float passFeedback;
        };

        static ComposeReport loggedCompose {};

        // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
        // Enshrouded session, because a measured white point drifts continuously and every drift was a
        // change. A line per meaningful change is the point; a line per frame is a different problem.
        const ComposeReport composeNow { true,
                                         std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
                                         resolveParams.TransferStrength,
                                         resolveParams.ColourStrength,
                                         resolveParams.MaxRatio,
                                         resolveParams.Passthrough,
                                         resolveParams.DebugView,
                                         resolveParams.CompareMode,
                                         resolveParams.Transfer,
                                         g_nr.workWidth,
                                         g_nr.workHeight,
                                         effectivePasses,
                                         std::clamp(cfg.DlssNrPassFeedback.value_or_default(), 0.0f, 1.0f) };

        if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
            loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
            loggedCompose.maxRatio != composeNow.maxRatio ||
            loggedCompose.passthrough != composeNow.passthrough ||
            loggedCompose.debugView != composeNow.debugView ||
            loggedCompose.compareMode != composeNow.compareMode ||
            loggedCompose.residual != composeNow.residual || loggedCompose.workW != composeNow.workW ||
            loggedCompose.workH != composeNow.workH || loggedCompose.passes != composeNow.passes ||
            loggedCompose.passFeedback != composeNow.passFeedback)
        {
            loggedCompose = composeNow;
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, colour transform {}, transfer {}, model {}x{}, passes {} (feedback {:.2f}), "
                     "debug view {}, compare {}",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW,
                     composeNow.workH, composeNow.passes, composeNow.passFeedback, composeNow.debugView,
                     composeNow.compareMode);
        }

        // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
        // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
        // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
        // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
        // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
        // falls back to the Nx pair. finalAnswer is NPSR here; outputNative is UAV from last frame.
        bool superDownOk = false;
        bool pairDownOk = false; // packed and supersampling: the unpacked pair, both halves averaged to native
        if (spatial && workScale > 1.0f)
        {
            // The proxy goes through the same filter as the answer, so the resolve compares like with like. If either
            // fails, the unpacked pair goes to the resolve as it is (both halves through the resolve's own tap).
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler)
            {
                if (g_nr.superUp != nullptr)   { delete g_nr.superUp;   g_nr.superUp = nullptr; }
                if (g_nr.superDown != nullptr) { delete g_nr.superDown; g_nr.superDown = nullptr; }
                EdgeCompression::ReleaseDown();
                g_nr.nrScaler = nrScaler;
            }
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);
            // The proxy's filter has to be the answer's. Compression off and on again around a change of the NR
            // downscaler would otherwise leave this one on the old filter (superDown is rebuilt by the other path).
            if (EdgeCompression::proxyDown != nullptr && EdgeCompression::proxyDownScaler != nrScaler)
                EdgeCompression::ReleaseDown();
            if (EdgeCompression::proxyDown == nullptr)
            {
                EdgeCompression::proxyDown =
                    new OS_Dx12("DLSS-NR compress screen edges proxy down", device, false, nrScaler);
                EdgeCompression::proxyDownScaler = nrScaler;
            }

            pairDownOk = g_nr.superDown != nullptr && EdgeCompression::proxyDown != nullptr &&
                         EdgeCompression::proxyDown->Dispatch(cmdList, ordinaryProxy, EdgeCompression::proxyNative) &&
                         g_nr.superDown->Dispatch(cmdList, ordinaryAnswer, EdgeCompression::answerNative);

            if (pairDownOk)
            {
                for (ID3D12Resource* native : { EdgeCompression::proxyNative, EdgeCompression::answerNative })
                    Barrier(cmdList, native, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                superDownOk = true;
            }
        }
        else if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
                 g_nr.superDown->Dispatch(cmdList, ordinaryAnswer, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        // Reduced up-leg (mirrors the down-leg above). DlssNrReducedUpscaleMethod: 0 = Bilinear
        // (not SGSR1-enlarged), 1 = SGSR1 answer. The proxy side used to be independently
        // SGSR1-enlargeable too (old methods 2/3), removed: applied to only one side of the
        // model/proxy comparison it fed a filter mismatch into the composition instead of real
        // detail, and had no effect on Replace at all (Replace never reads the proxy) -- see
        // plans/2026-09-18-dlssnr-enlarge-filter-simplify.md. Whichever side is not
        // SGSR1-enlarged still correctly reads from modelInput (the real downsampled source the
        // model saw) via the resolve's own implicit bilinear tap (dlssnr.hlsl:931-932) -- that is
        // not the old colorCopy-vs-modelInput bug ("the low res image got combined with the final
        // image"), which was comparing against the wrong buffer entirely, not merely a
        // softer-filtered one.
        bool sgsrAnswerOk = false;
        const uint32_t upscaleMethod = cfg.DlssNrReducedUpscaleMethod.value_or_default();
        const bool wantsSgsr1Answer = !superior && upscaleMethod == 1;
        // Gated on `reduced` (the actual rounded-size flag), not just workScale < 1.0f -- a workScale
        // that rounds back to the native size (e.g. Auto's continuous ratio landing at 0.9998) would
        // otherwise engage SGSR1 at 1:1, wasted work that also isn't guaranteed identity-preserving.
        if (!cached && reduced && workScale < 1.0f && wantsSgsr1Answer)
        {
            const float sgsr1EdgeThreshold = 0.300f;
            const float sgsr1EdgeSharpness = 2.00f;

            if (g_nr.outputNative != nullptr)
            {
                if (g_nr.sgsr1UpAnswer == nullptr)
                    g_nr.sgsr1UpAnswer = new SGSR1_Dx12("DLSS-NR SGSR1 up (answer)", device);

                if (g_nr.sgsr1UpAnswer != nullptr &&
                    g_nr.sgsr1UpAnswer->Dispatch(cmdList, ordinaryAnswer, g_nr.outputNative,
                                                 resolveParams.ReversibleMode, resolveParams.Passthrough,
                                                 sgsr1EdgeThreshold, sgsr1EdgeSharpness))
                {
                    Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    sgsrAnswerOk = true;
                }
            }

            // INFO-level, change-gated like the composition log above -- the pass's own Dispatch()
            // only logs at DEBUG (matches superUp/superDown's identical silence), which wasn't
            // enough to confirm engagement while diagnosing a "still looks blurred" report.
            static bool hasLoggedSgsrUp = false;
            static uint32_t lastLoggedState = 0xFFFFFFFFu;
            const uint32_t state = sgsrAnswerOk ? 1u : 0u;
            if (!hasLoggedSgsrUp || lastLoggedState != state)
            {
                LOG_INFO("DLSS-NR SGSR1 up-leg: answer {} (method {}, workScale {:.3f}, "
                         "{}x{} -> {}x{})",
                         sgsrAnswerOk ? "engaged" : "NOT engaged",
                         upscaleMethod, workScale, workWidth, workHeight, width, height);
                lastLoggedState = state;
                hasLoggedSgsrUp = true;
            }
        }
        else if (!superior && reduced && workScale < 1.0f)
        {
            // Parity with the SGSR1 branch's own log above, change-gated the same way, so a log
            // scan can confirm the user's Bilinear choice actually took effect without needing a
            // breakpoint.
            static bool hasLoggedBilinear = false;
            if (!hasLoggedBilinear)
            {
                LOG_INFO("DLSS-NR reduced up-leg: Bilinear selected, SGSR1 skipped entirely "
                         "(workScale {:.3f}, {}x{} -> {}x{})",
                         workScale, workWidth, workHeight, width, height);
                hasLoggedBilinear = true;
            }
        }

        ID3D12Resource* resolveProxy = pairDownOk ? EdgeCompression::proxyNative
                                       : superDownOk ? g_nr.colorCopy : ordinaryProxy;
        ID3D12Resource* resolveAnswer = pairDownOk ? EdgeCompression::answerNative
                                        : (superDownOk || sgsrAnswerOk) ? g_nr.outputNative : ordinaryAnswer;

        // Pre-SR Color is not guaranteed to have UAV support. Write directly when legal; otherwise
        // resolve into hdrCopy while the original Color remains readable, then copy the result back.
        ID3D12Resource* resolveOriginal = targetSupportsUav ? g_nr.hdrCopy : target;
        resolveParams.OriginalIsGameColour = targetSupportsUav ? 0u : 1u;
        ID3D12Resource* resolveTarget = targetSupportsUav ? target : g_nr.hdrCopy;

        if (targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // A cached frame of the edit cache writes the output from the carried edit instead (the cache runs only where
        // the output takes a UAV, so resolveTarget is the output there)
        if (cached)
            EditCache::ApplyCached(cacheFrame, resolveTarget);
        else if (superiorGeneration && (superiorGeneration->temporal || (spatial && superiorGainScales.low < 1.0f)))
        {
            auto& g = *superiorGeneration;
            auto* shapingProxy = g.packed ? modelInput : g.temporal ? ordinaryProxy : resolveProxy;
            auto* residual =
                g.temporal
                    ? SuperiorRun::Stabilize(g, *this, cmdList, shapingProxy, g.packed ? finalAnswer : ordinaryAnswer,
                                             motionIn, guides, frame, superiorReset, g_nr.heldActive,
                                             g.packed ? EdgeCompression::depth : nullptr,
                                             g.packed ? EdgeCompression::motion : nullptr,
                                             { spatialGuideLayout.modelW, spatialGuideLayout.modelH },
                                             SuperiorDepthPrecision(guideDesc.Format))
                    : resolveAnswer;
            const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, superiorCovered, superiorCompleted,
                                                                   superiorGainScales, g_nr.superiorMainOnlyGains);
            auto* low = residual && (gains.high != 1.0f || gains.low != 1.0f)
                            ? SuperiorRun::FinalLow(g, *this, cmdList, shapingProxy, residual, g.temporal)
                            : nullptr;
            DlssNr::Superior::Fields::SetBandGains(resolveParams, gains.high, gains.low);
            DlssNr::SuperiorScaling::SetGuidedRange(resolveParams, workScale);
            // A raw NR answer when temporal stabilization is inactive, else the decoded signed DLSS residual.
            DlssNr::Superior::Fields::SetResolveFlags(resolveParams, low != nullptr, !g.temporal);
            auto* pso = SuperiorResolvePipeline(resolveParams, !g.temporal, g.packed, g.warpCache.Get() != nullptr);
            const bool lowReady = (gains.high == 1.0f && gains.low == 1.0f) || low != nullptr;
            composed = residual && lowReady && pso &&
                       DispatchPass(cmdList, resolveParams, shapingProxy, residual, resolveOriginal,
                                    g.packed ? (g.warpCache.Get() ? g.warpCache.Get() : g.warp.Get()) : motionIn,
                                    exposureTex, resolveTarget, nullptr, low, pso);
            SuperiorRun::Say(composed ? "" : "Superior reconstruction failed; keeping the game frame.");
        }
        else
        {
            const bool dispatched = DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, resolveOriginal,
                                                 motionIn, exposureTex, resolveTarget, nullptr);
            // Without Superior the frame counts as composed whatever the dispatch says, exactly as before.
            composed = dispatched || !superior;
        }
        if (cached)
            composed = true;
        if (superiorGeneration && !composed)
            superiorGeneration->reset = true;

        // A refresh: the model's output becomes the anchor (and the crossfade's first frame is written)
        if (composed && cachePlan.runs && cachePlan.refresh && targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            EditCache::AfterResolve(cacheFrame, target, [&] { TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS); });
        }

        if (!targetSupportsUav && composed)
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(target, g_nr.hdrCopy);
            TransitionTarget(priorTargetState);
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (!targetSupportsUav && !composed)
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (CalibrationActive())
        {
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
            CalibrationMeasure(this, cmdList, device, target, ordinaryProxy, width, height);
            TransitionTarget(priorTargetState);
        }

        MakeModelWritable(g_nr.output);
        if (g_nr.passScratch != nullptr)
            MakeModelWritable(g_nr.passScratch);
        if (g_nr.passClampScratch != nullptr)
            MakeModelWritable(g_nr.passClampScratch);
        if (g_nr.passClampScratch2 != nullptr)
            MakeModelWritable(g_nr.passClampScratch2);

        if (pairDownOk)
        {
            for (ID3D12Resource* native : { EdgeCompression::proxyNative, EdgeCompression::answerNative })
                Barrier(cmdList, native, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else if (superDownOk || sgsrAnswerOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // On-demand capture works in this path too: hdrCopy still holds the frame as the upscaler produced it
        // (the encode's untouched copy, the resolve's original), and the edited frame is the output itself. Where
        // the target takes no UAV the resolve wrote into hdrCopy instead and no original is left: "before" is then
        // the model's input picture (colorCopy, the encoded proxy), and the note says so. colorCopy was "before"
        // everywhere until the edit cache dump showed it is the proxy, not the frame. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its own.
        if (g_capture.isActive())
        {
            std::vector<capture::Source> sources {
                { "before", targetSupportsUav ? g_nr.hdrCopy : g_nr.colorCopy,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                { "after", target, targetState },
            };
            std::string note = targetSupportsUav ? "" : "beforeIsProxy 1";

            if (g_captureIsDump)
            {
                // The guides as the pass reads them, in NON_PIXEL_SHADER_RESOURCE here (ReadableGuide's contract,
                // still bound for this frame's passes)
                sources.push_back({ "motion", motionIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE });
                const bool withDepth = depthIn != nullptr && DumpableDepth(depthIn->GetDesc().Format);
                if (withDepth)
                    sources.push_back({ "depth", depthIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE });

                char text[512];
                std::snprintf(text, sizeof(text),
                              "mvScale %.6f %.6f motion %ux%u at %u %u depth %ux%u at %u %u inverted %d dumped %d "
                              "linearHdr %d curve %u white %.6f reset %d work %ux%u epoch %llu",
                              g_nr.guideMvScaleX, g_nr.guideMvScaleY, (unsigned) motionWidth, (unsigned) motionHeight,
                              (unsigned) motionBaseX, (unsigned) motionBaseY, (unsigned) guideWidth,
                              (unsigned) guideHeight, (unsigned) depthBaseX, (unsigned) depthBaseY,
                              g_nr.guideDepthInverted ? 1 : 0, withDepth ? 1 : 0, isHdrBuffer ? 1 : 0,
                              encodeParams.ReversibleMode, encodeParams.WhitePoint, g_nr.reset ? 1 : 0, workWidth,
                              workHeight, (unsigned long long) (frame.SubmissionEpoch != 0 ? frame.SubmissionEpoch
                                                                                           : g_frames));
                note = note.empty() ? std::string(text) : note + " " + text;
            }

            g_capture.record(cmdList, device, sources, note);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else if (spatialFailed)
    {
        // The unpack failed and has already turned compression off; this frame keeps the game's picture.
    }
    else if (spatial)
    {
        // The model would not take the packed picture. NR is fine; compression is what is turned off.
        EdgeCompression::TurnOff(DlssNr::Spatial::Status::TurnedOffModel);
        g_nr.reset = true;
        LOG_WARN("DLSS-NR compress screen edges: evaluate returned 0x{:X} ({}) on the packed picture", (uint32_t) result,
                 NgxResultName((unsigned int) result));
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        if (g_nr.lastModelError && *g_nr.lastModelError())
        {
            g_nr.modelError = g_nr.lastModelError();
            g_nr.reason = g_nr.modelError.c_str();
        }
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    // On an evaluation failure, intermediate A/B inputs may still be readable. Restore both persistent
    // ping-pong surfaces to the UAV state the next frame starts from.
    MakeModelWritable(g_nr.output);
    if (g_nr.passScratch != nullptr)
        MakeModelWritable(g_nr.passScratch);
    if (g_nr.passClampScratch != nullptr)
        MakeModelWritable(g_nr.passClampScratch);
    if (g_nr.passClampScratch2 != nullptr)
        MakeModelWritable(g_nr.passClampScratch2);

    DetailReuse::AfterResolve(cmdList);

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (spatial)
    {
        for (ID3D12Resource* packed : { EdgeCompression::color, EdgeCompression::depth, EdgeCompression::motion })
            Barrier(cmdList, packed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (spatialUnpacked)
            for (ID3D12Resource* unpacked : { EdgeCompression::proxy, EdgeCompression::answer })
                Barrier(cmdList, unpacked, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Failed evaluations leave the game's original image intact. A successful copy-back writes
    // only the active rectangle and restores both resources before DLSS consumes the image.
    FinishColor(composed);
    if (composed)
        ++g_nr.successfulDispatches;

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        // This path records into the game's own list, so there is no queue of ours to read from.
        // A caller that knows which queue the list goes to says so; otherwise the one the upscaler was
        // invoked on serves. The bridges have to say, because they run on a queue of their own that
        // State never learns about -- a Vulkan game creates no D3D12 swapchain, so nothing ever sets
        // currentCommandQueue and the cost went unreported.
        auto* queue = timingQueue != nullptr ? timingQueue
                                             : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

        if (queue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime(queue); ms.has_value())
                g_lastGpuTime = ms;
            // Every completed frame, not only the newest: full and reused frames alternate, and two can complete
            // between reads.
            for (const double sample : g_gpuTime->TakeFresh())
                DetailReuse::RecordGpuTime(sample);

            if (g_ngxTime != nullptr)
            {
                if (auto ngx = g_ngxTime->ReadGpuTime(queue); ngx.has_value())
                    g_lastNgxTime = ngx;
            }

            if (g_scanTime != nullptr)
            {
                g_scanTime->ReadGpuTime(queue);

                for (const double sample : g_scanTime->TakeFresh())
                {
                    g_scanTimeSum += sample;
                    g_scanTimeMax = std::max(g_scanTimeMax, sample);
                    ++g_scanTimeSamples;
                }

                static unsigned long long lastScanLog = 0;

                if (g_scanTimeSamples > 0 && g_frames - lastScanLog > 600)
                {
                    lastScanLog = g_frames;
                    LOG_INFO("DLSS-NR exposure scan GPU cost: {:.4f} ms per frame on average, {:.4f} ms at most, over {} "
                             "frames; {} candidates held ({})",
                             g_scanTimeSum / g_scanTimeSamples, g_scanTimeMax, g_scanTimeSamples,
                             DlssNr::ExposureScan::Report().size(), DlssNr::ExposureScan::AutoStatus());
                    g_scanTimeSum = g_scanTimeMax = 0.0;
                    g_scanTimeSamples = 0;
                }
            }

            // The split, once every few hundred frames. What is worth reading is not the total but the
            // remainder: the model's cost is NVIDIA's to set, and everything else is ours.
            static unsigned long long lastSplitLog = 0;

            if (g_lastGpuTime.has_value() && g_lastNgxTime.has_value() && g_frames - lastSplitLog > 600)
            {
                lastSplitLog = g_frames;
                const double total = g_lastGpuTime.value();
                const double ngx = g_lastNgxTime.value();
                LOG_INFO("DLSS-NR elapsed: {:.2f} ms total, {:.2f} ms model, {:.2f} ms surrounding work ({:.0f}%; intervals may include other GPU work)",
                         total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
                if (reusePlan.active)
                {
                    const auto status = DetailReuse::Status();
                    const auto& why = DetailReuse::cadence.Causes();
                    LOG_INFO("DLSS-NR detail reuse: {:.2f} ms per frame on average ({:.2f} to {:.2f}) over the last 16; "
                             "full {}, reused {}, fallback {}, held {}; fallbacks: reset {}, blocked {}, gap {} (last "
                             "step {} of {} allowed), settings changed {}, not recorded {}",
                             status.averageMs, status.lightMs, status.heavyMs, status.full, status.reused,
                             status.fallback, status.held, why.reset, why.blocked, why.gap, why.lastGapStep,
                             why.lastMaxStep, why.changed, why.notRecorded);
                }
            }
        }
    }

    if (cachePlan.runs)
        EditCache::EndFrame(cmdList);

    // Put the guide clones this frame copied into back where the next frame's copy expects to find them.
    // A clone this frame did not use (no depth, or a typed guide) is still in COPY_DEST.
    if (g_nr.depthClone != nullptr && depthIn == g_nr.depthClone)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr && motionIn == g_nr.motionClone)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && (!spatial || replaceCorrection) && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    device->Release();
}

namespace DlssNr
{
namespace Late { bool CaptureResidual(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, float, bool); }
#include "DlssNr_DeferredSr.inl"
std::string DeferredDlssStatus() { return SynchronousDeferredDlssStatus(); }

void RetryAfterFailure()
{
    g_nr.failed = false;
    g_nr.reason = "";
    g_nr.reset = true;
    EdgeCompression::tracker.Retry();

}

#include "DlssNr_Late.inl"

// Reads the game's parameter block and runs the pass on what it finds.
//
// This is the call site's job, not the pass's. A caller that has the resources in hand -- a
// reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
// RunPass directly and never touches an NGX parameter block.
void EvaluateInternal(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                      bool beforeUpscale, ID3D12CommandQueue* timingQueue, bool rayReconstruction,
                      unsigned long long submissionEpoch)
{
    std::lock_guard<std::recursive_mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();
    static unsigned lastPrecision=0;
    const unsigned precision=cfg.DlssNrPrecision.value_or_default();
    if(lastPrecision!=precision)
    {
        RetryAfterFailure();
        DeferredSr::Cancel();
        lastPrecision = precision;
    }
    DlssNrNative::SetPrecision(precision);
    Late::CheckRoute();
    const unsigned finishedMode = !cfg.DlssNrFinishedPicture.value_or_default() ? 0u :
        (cfg.DlssNrRunBeforeSr.value_or_default() || cfg.DlssNrDeferredDlss.value_or_default()) ? 2u : 1u;
    static unsigned lastFinishedMode = 0;
    if (lastFinishedMode != finishedMode)
    {
        g_nr.reset = true;
        if (g_gpuTime) g_gpuTime->ClearLast();
        if (g_ngxTime) g_ngxTime->ClearLast();
        if (g_scanTime) g_scanTime->ClearLast();
        g_lastGpuTime.reset();
        g_lastNgxTime.reset();
        Late::Cancel();
        DeferredSr::Cancel();
        lastFinishedMode = finishedMode;
    }
    if (finishedMode)
    {
        if (!cfg.DlssNrEnabled.value_or_default())
        { DeferredSr::Cancel(); Late::Cancel(); return; }
        if (timingQueue || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        { DeferredSr::Cancel(); Late::Cancel(); Late::Say("This option needs a native DirectX 12 game."); return; }
        // Before the pre-SR model and private DLSS run, so nothing is computed that cannot be composed.
        if (Late::PausedForGameFrameGeneration())
        {
            DeferredSr::Cancel(); Late::Cancel(); Late::Say(Late::pausedForGameFg);
            if (finishedMode == 2) DeferredSr::Say("paused: the game's own frame generation is on");
            return;
        }
        if (finishedMode == 2)
        {
            if (rayReconstruction)
            { DeferredSr::Cancel(); Late::Cancel(); Late::Say("Running the model before SR with this option does not support Ray Reconstruction."); return; }
            if (cmdList && params)
            {
                const auto submitted = State::Instance().frameCount;
                const auto epoch = g_nrSeamClock.AtSeam(beforeUpscale, false, submitted);
                if (beforeUpscale) DeferredSr::Before(cmdList, params, epoch, submitted, nullptr);
                else DeferredSr::After(cmdList, params, epoch);
            }
        }
        else
        {
            DeferredSr::Cancel();
            if (beforeUpscale) Late::Capture(cmdList, params, rayReconstruction);
        }
        return;
    }
    Late::Cancel();
    if (!cfg.DlssNrEnabled.value_or_default() || !cfg.DlssNrDeferredDlss.value_or_default() || rayReconstruction)
    {
        DeferredSr::Cancel();
        if (rayReconstruction && cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrDeferredDlss.value_or_default())
            DeferredSr::Say("inactive: Ray Reconstruction; using ordinary before/after NR placement");
    }
    else
    {
        if (cmdList != nullptr && params != nullptr)
        {
            const auto submitted = timingQueue != nullptr ? submissionEpoch : State::Instance().frameCount;
            const auto epoch = g_nrSeamClock.AtSeam(beforeUpscale, timingQueue != nullptr, submitted);
            if (beforeUpscale)
                DeferredSr::Before(cmdList, params, epoch, submitted, timingQueue);
            else
                DeferredSr::After(cmdList, params, epoch);
        }
        return;
    }

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    // Both SR and RR+SR use the same placement control. Unsupported colour subrects
    // retain the common post-upscale fallback; RR identity only separates history
    // and prevents using the SR-only deferred-residual experiment on an RR feature.
    // Skipped entirely while RR is active: configuredBefore below forces post-SR regardless of
    // this result, so there is nothing to gain from the GetResource/GetDesc work every frame.
    // A Tune runs after SR while Before SR is set, and NR goes back before SR when it ends (TuneRunsAfterSr).
    const bool tuneAfterSr = cfg.DlssNrRunBeforeSr.value_or_default() &&
                             DlssNrExposureCalibrate::TuneRunsAfterSr(DlssNrExposureCalibrate::TheRun(), GetTickCount64());
    const bool beforeSrSet = cfg.DlssNrRunBeforeSr.value_or_default() && !tuneAfterSr;

    // NR failing while a Tune moved it after SR (the model does not fit at output size, say) is a failure of that
    // placement, not of the user's: back before SR, it tries once more.
    {
        static bool failedAfterSrForTune = false;
        if (tuneAfterSr && g_nr.failed)
        {
            failedAfterSrForTune = true;
        }
        else if (!tuneAfterSr && failedAfterSrForTune)
        {
            failedAfterSrForTune = false;
            if (g_nr.failed)
            {
                LOG_WARN("DLSS-NR: failed after SR during Tune ({}); back before SR, trying again", g_nr.reason);
                RetryAfterFailure();
            }
        }
    }

    bool preSrCompatible = true;
    if (beforeSrSet && !rayReconstruction)
    {
        ID3D12Resource* preColor = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
        unsigned int renderWidth = 0, renderHeight = 0, colorBaseX = 0, colorBaseY = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &renderWidth);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &renderHeight);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &colorBaseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &colorBaseY);

        if (preColor == nullptr)
        {
            preSrCompatible = false;
        }
        else
        {
            const D3D12_RESOURCE_DESC colorDesc = preColor->GetDesc();
            const unsigned int allocationWidth = (unsigned int) colorDesc.Width;
            const unsigned int allocationHeight = colorDesc.Height;
            const auto active = PreSrColorExtent(colorDesc, renderWidth, renderHeight, colorBaseX, colorBaseY);
            preSrCompatible = active.has_value();

            if (active && (active->width != allocationWidth || active->height != allocationHeight))
            {
                static bool reportedPadding = false;
                if (!reportedPadding)
                {
                    reportedPadding = true;
                    LOG_INFO("DLSS-NR before SR: staging active {}x{} from padded Color allocation {}x{}; "
                             "only the active rectangle is copied back. Model size follows active size and WorkingScale.",
                             active->width, active->height, allocationWidth, allocationHeight);
                }
            }

            if (!preSrCompatible)
            {
                static bool warnedSubrect = false;
                if (!warnedSubrect)
                {
                    warnedSubrect = true;
                    LOG_WARN("DLSS-NR before SR requires a valid origin-zero active rectangle inside a "
                             "single-sample 2D Color texture; got allocation {}x{}, active {}x{} at {},{}. "
                             "Falling back after SR.",
                             allocationWidth, allocationHeight, renderWidth, renderHeight, colorBaseX,
                             colorBaseY);
                }
            }
        }
    }

    // Ray Reconstruction already denoises the frame before either NR seam runs, so placing NR
    // before it only exposes the edit to RR's own denoise pass -- which cannot tell a deliberate
    // edit from noise it is trained to remove unless the edit is motion-consistent across frames
    // the way real scene detail is. Running NR after RR sidesteps that entirely: RR active forces
    // post-SR placement unconditionally, regardless of the RunBeforeSR setting.
    const bool configuredBefore = beforeSrSet && preSrCompatible && !rayReconstruction;

    // Where NR would run without a Tune, for the run's wait (CalibrationSituation): kept from before the Tune moved it.
    if (!tuneAfterSr)
        g_nr.beforeSrPlacement = configuredBefore;

    if (configuredBefore != beforeUpscale)
        return;

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* target = beforeUpscale
                                 ? GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color")
                                 : output;
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Depth is optional -- NVIDIA's own retail DLL treats it as a refinement of motion-vector dilation at
    // object edges, not a required input, and it is null in every known capture including NVIDIA's own
    // native integration. Without colour/output or motion there is nothing to run on; that is not a
    // failure -- some evaluates legitimately carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr ? (beforeUpscale ? "the parameters carried no color texture"
                                                          : "the parameters carried no output texture")
                                         : "the parameters carried no motion vectors");
        return;
    }

    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    if (output != nullptr)
    {
        frame.OutputWidth = (unsigned int) output->GetDesc().Width;
        frame.OutputHeight = output->GetDesc().Height;
    }
    unsigned int outputWidth = 0, outputHeight = 0;
    params->Get(NVSDK_NGX_Parameter_OutWidth, &outputWidth);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &outputHeight);
    if (outputWidth && outputHeight)
    {
        frame.OutputWidth = outputWidth;
        frame.OutputHeight = outputHeight;
    }
    frame.BeforeUpscale = beforeUpscale;
    frame.RayReconstruction = rayReconstruction;
    frame.SubmissionEpoch = timingQueue != nullptr ? submissionEpoch : State::Instance().frameCount;

    // Color and Output may use different formats even though DLSS treats them as the same frame colour
    // space. Output is the stable authority across injection points; target is only a fallback for a
    // malformed parameter block.
    ID3D12Resource* colourAuthority = output != nullptr ? output : target;
    // Finished Picture decodes the screen, not this evaluate, and reports its own choice.
    ApplyColourEncoding(frame, Config::Instance()->DlssNrColourEncoding.value_or_default(),
                        (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0,
                        colourAuthority != nullptr ? colourAuthority->GetDesc().Format : DXGI_FORMAT_UNKNOWN,
                        !Config::Instance()->DlssNrFinishedPicture.value_or_default());

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);

    params->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &frame.FrameTimeMs);
    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;
        frame.GameAutoExposure = autoExposureFlag;

        struct ExposureReport
        {
            bool valid;
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;
        };

        static ExposureReport logged {};
        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.
        static float loggedExposure = -1.0f;

        if (g_nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - g_nr.gameExposure) > std::max(0.02f * g_nr.gameExposure, 1e-5f))
        {
            loggedExposure = g_nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            static float loggedScan = -1.0f;

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                // Read on Game exposure, paired on Automatic while scanning
                const float gameNow = g_nr.gameExposure > 1e-6f ? g_nr.gameExposure : g_nr.autoPairGameExposure;

                if (gameNow > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, gameNow, scanned / gameNow);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.
    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    device->Release();

    if (g_compose == nullptr)
    {
        ReportSkipOnce("the pass could not be created");
        return;
    }

    g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue);
}

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue, bool rayReconstruction,
                          unsigned long long submissionEpoch)
{
    EvaluateInternal(cmdList, params, false, timingQueue, rayReconstruction, submissionEpoch);
}

void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue, unsigned long long submissionEpoch,
                           bool rayReconstruction)
{
    EvaluateInternal(cmdList, params, true, timingQueue, rayReconstruction, submissionEpoch);
}

// The pass. Resources in, nothing read from anywhere the caller cannot see.

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    // Every other entry point in this file takes the lock before touching g_nr; this one was reaching
    // EnsureForwarder without it.
    std::lock_guard<std::recursive_mutex> nrLock(g_nrMutex);

    // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
    // into a subsystem on the game's own device rather than reading something we already hold.
    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
    const int bits = probe(snippet->wstring().c_str());

    // And the question NGX has an API for. Asked first because it creates nothing: if the feature
    // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*))
        GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        // The adapter the game is actually running on. Without it the query answers
        // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
        // the question -- that is what the first attempt got, on a 5080.
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            factory->EnumAdapters(0, &adapter);

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0        ? "SUPPORTED"
                              : (supported & 16)    ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)     ? "AdapterUnsupported"
                              : (supported & 2)     ? "DriverVersionUnsupported"
                              : (supported & 8)     ? "OSVersionBelowMinimum"
                              : (supported & 1)     ? "CheckNotPresent"
                                                    : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})",
             bits, (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    // Four ways of asking, since the feature has already said it supports this platform.
    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
        // with a minimum architecture this card meets, so the platform is not the obstacle and saying
        // otherwise here would be printing a conclusion the evidence does not carry.
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

const char* BackendName() { return g_nr.forwarder == nullptr ? "" : g_nr.isPort ? "vendor-neutral port" : "NVIDIA NGX"; }

// What the game offers by way of exposure, and what has been read from it. For the menu, so a user
// can see whether this game supplies one at all without having to read a log.
ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    return s;
}

// What the automatic exposure has measured, for the menu and for capturing Trim anchors.
ExposureStatus AutoExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.autoExposureFrames;
    s.offeredNow = g_nr.autoExposureReadable;
    s.everOffered = g_nr.autoExposureFrames != 0;
    s.exposure = g_nr.autoExposureValue;
    s.preExposure = g_nr.autoExposurePreExposure;
    return s;
}

FollowGameStatus FollowGameExposureStatus()
{
    FollowGameStatus s {};
    s.gameExposureSeen = g_nr.autoPairGameExposure > 0.0f;
    s.following = g_nr.followingGame;
    s.disagreementEv = FollowDisagreementEv();
    s.foundInForce = g_nr.foundBaseWhitePoint > 0.0f;
    return s;
}

// Read every menu frame, so it does not take g_nrMutex (held through NR's whole recording): the status is published
// at the end of each BeforeModel.
DetailReuseInfo DetailReuseStatus() { return DetailReuse::Published(); }

EditCacheInfo EditCacheStatus() { return { EditCache::Running(), EditCache::ModelShare(), EditCache::StandsAside() }; }

// A copy published as each evaluate's scene-cut work ends (DlssNr_SceneCut.inl): the menu never waits for g_nrMutex.
SceneCutInfo SceneCutStatus() { return SceneCut::Status(); }

int CurrentModelResolutionPercent() { return (int) lroundf(g_nr.appliedWorkScale * 100.0f); }

Spatial::Published EdgeCompressionStatus()
{
    std::lock_guard<std::mutex> lock(EdgeCompression::publishedLock);
    return EdgeCompression::published;
}

void CurrentModelSize(unsigned int& width, unsigned int& height)
{
    width = g_nr.workWidth;
    height = g_nr.workHeight;
}

std::optional<double> LastGpuTime() { return g_lastGpuTime; }



void RequestCapture(unsigned int frames)
{
    if (g_capture.isActive())
        return;
    ClearCaptureDirectory();
    g_captureIsDump = false;
    g_capture.request(frames);
}

void RequestCacheDump(unsigned int frames)
{
    if (g_capture.isActive())
        return;
    g_captureIsDump = true;
    g_capture.request(frames, capture::kMaxDumpFrames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void Shutdown()
{
    std::lock_guard<std::recursive_mutex> nrLock(g_nrMutex);
    DeferredSr::Shutdown();
    if (SuperiorRun::g_used)
        SuperiorRun::Shutdown();
    CalibrationShutdown();
    EdgeCompression::Shutdown();

    for (auto& r : g_nrRetired)
    {
        ReleaseNrFeature(r);

        if (r.resource != nullptr)
            r.resource->Release();
    }

    g_nrRetired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;
    g_nr.featurePendingSubmission = false;

    for (unsigned int pass = 1; pass < DlssNr::MaxPassCount; ++pass)
    {
        void*& f = g_nr.passFeature[pass];
        if (f != nullptr && g_nr.release != nullptr)
            g_nr.release(f);

        f = nullptr;
        g_nr.passNeedsReset[pass] = false;
        g_nr.passCreateFailed[pass] = false;
        g_nr.passPendingSubmission[pass] = false;
    }

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    DetailReuse::Release();
    EditCache::Release();
    SceneCut::Release();

    if (g_nr.passScratch != nullptr)
    {
        g_nr.passScratch->Release();
        g_nr.passScratch = nullptr;
    }
    g_nr.passScratchFailed = false;

    if (g_nr.passClampScratch != nullptr)
    {
        g_nr.passClampScratch->Release();
        g_nr.passClampScratch = nullptr;
    }
    g_nr.passClampScratchFailed = false;

    if (g_nr.passClampScratch2 != nullptr)
    {
        g_nr.passClampScratch2->Release();
        g_nr.passClampScratch2 = nullptr;
    }
    g_nr.passClampScratch2Failed = false;

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.activeColor != nullptr)
    {
        g_nr.activeColor->Release();
        g_nr.activeColor = nullptr;
    }

    if (g_nr.lutScratch != nullptr)
    {
        g_nr.lutScratch->Release();
        g_nr.lutScratch = nullptr;
    }
    g_nr.lutScratchFailed = false;

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    if (g_nr.styled != nullptr)
    {
        g_nr.styled->Release();
        g_nr.styled = nullptr;
    }
    g_nr.styledState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (g_nr.styledB != nullptr)
    {
        g_nr.styledB->Release();
        g_nr.styledB = nullptr;
    }
    g_nr.styledBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (g_nr.sgsr1UpAnswer != nullptr)
    {
        delete g_nr.sgsr1UpAnswer;
        g_nr.sgsr1UpAnswer = nullptr;
    }

    if (g_nr.outputNative != nullptr)
    {
        g_nr.outputNative->Release();
        g_nr.outputNative = nullptr;
    }

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.autoExposure != nullptr)
    {
        g_nr.autoExposure->Release();
        g_nr.autoExposure = nullptr;
    }

    if (g_nr.autoExposureRaw != nullptr)
    {
        g_nr.autoExposureRaw->Release();
        g_nr.autoExposureRaw = nullptr;
    }

    g_nr.autoExposureRawValue = 0.0f;
    g_nr.autoExposureAdapting = false;
    g_nr.autoExposureRawFailed = false;
    g_nr.autoExposureAdapter.Invalidate();

    g_nr.autoExposureReadable = false;
    g_nr.autoExposureValue = 0.0f;
    g_nr.autoExposurePreExposure = 1.0f;
    g_nr.autoExposureFrames = 0;
    g_nr.autoPairGameExposure = 0.0f;
    g_nr.autoPairPreExposure = 1.0f;
    g_nr.followingGame = false;

    // exposureReadbackSource is deliberately left alone. The ring's frame counter and slot kinds are
    // reset below, which is all a recreate needs; resetting the source too would make the next frame
    // look like a change of source, and that clears the held game exposure -- the value the note
    // further down says must survive a recreate.

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    for (ID3D12Resource** rb : { &g_nr.diagGridReadback, &g_nr.diagExposureReadback, &g_nr.diagProxyReadback })
    {
        if (*rb != nullptr)
        {
            (*rb)->Release();
            *rb = nullptr;
        }
    }

    g_nr.diagQueuedAt = 0;

    // The slots these flags describe have just been released, so nothing may vouch for what the next
    // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
    // transition within the same scene, and dropping to the slider for a few frames would be the
    // flicker the held value exists to prevent. The user switching the option off is the case where
    // the held value has to go, and that is handled at the edge in Dispatch.
    for (uint32_t& kind : g_nr.meterExposureKind)
        kind = 0u;

    g_nr.meterFrames = 0;


    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }



    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    g_gpuTime.reset();
    g_ngxTime.reset();
    g_scanTime.reset();
    g_lastNgxTime.reset();
    g_lastGpuTime.reset();

    Late::nativeHdr = {};
    g_compose.reset();
}
} // namespace DlssNr

std::string DlssNr::SuperiorScaling::Status()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return SuperiorRun::GetState().status;
}

std::string DlssNr::SuperiorScaling::Unavailable()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return SuperiorRun::GetState().unavailable;
}

bool DlssNr::SuperiorScaling::SrUnavailable()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return SuperiorRun::GetState().srUnavailable;
}

void DlssNr::SuperiorScaling::NgxDx12Shutdown()
{
    // Nothing to do (and no NR lock taken in the game's NGX shutdown) unless Superior has ever run.
    if (!SuperiorRun::g_used)
        return;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    SuperiorRun::NgxShutdown();
}
