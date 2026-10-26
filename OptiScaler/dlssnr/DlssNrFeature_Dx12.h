#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <vector>

#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_Spatial.h>
#include <nvsdk_ngx.h>
#include <menu/MenuPages.h>
#include <menu/HeaderBanner.h>
#include <imgui/imgui.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;

namespace DlssNr
{
inline constexpr unsigned int MaxPassCount = 30;
inline constexpr unsigned int DefaultMaxPassCount = 3;

// The model runs immediately after the game's upscaler, before the interface is drawn. It is shown a
// display-referred proxy of that frame -- the sort of picture it was trained on -- and its answer is
// composed back over the untouched original.
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// timingQueue is the queue this command list will be executed on, when the caller knows it.
// State::currentCommandQueue only exists once a D3D12 swapchain has been created, which a Vulkan
// game never does -- so without this the pass runs and never reports what it cost.
// rayReconstruction identifies the feature for history reset and the SR-only deferred experiment.
// Placement and model cost controls are shared by SR and RR+SR.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr, bool rayReconstruction = false,
                          unsigned long long submissionEpoch = 0);

// Runs the same pass over Color immediately before SR or RR+SR consumes it. The call is a no-op
// unless RunBeforeSR is enabled. Color is returned in its original readable state.
void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue = nullptr,
                           unsigned long long submissionEpoch = 0, bool rayReconstruction = false);



// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.




// The settings panel, drawn inside OptiScaler's menu.
void RenderMenu(::Config* config, float menuResScale, MenuPages::Page page);

// The main menu's header line for the native modes (Optical F5Low), by the rule in menu/HeaderBanner.h. True when it
// drew the line and its button, false when the line is the menu's own (a game's feature, or no offer to make).
// `upscalerFileChecks` draws the menu's upscaler file checks inside the offer's tooltip.
bool RenderHeaderBanner(::Config* config, HeaderBanner::Feature feature, bool upscalerFiles,
                        const std::string& upscalerNames, const std::string& backendName, const ImVec4& offerColour,
                        void (*upscalerFileChecks)());

// A line on the Upscaler page while the current upscaler is Optical F5Low's own (the game makes no upscaler call), and
// a hint on the Frame Generation page where Optical F5Low could give frame generation its input (`noUpscalerFeature`:
// no upscaler is running).
void RenderF5LowUpscalerNote();
void RenderF5LowFrameGenHint(::Config* config, bool noUpscalerFeature);

// The "Vulkan route" choice on the Frame Generation page, for a Vulkan game that is not run through dxvk: native
// swapchain or the bridge with the game's upscaler ([DlssNr] NativeFrameGenerationOnly). Optical F5Low's NR + frame
// generation is the third route and is chosen on its own page.
void RenderVulkanRoute(::Config* config);

// Once per frame: after about ten seconds with no upscaler call (and the header making its Optical F5Low offer), one
// toast per session points to the Optical F5Low page. [Menu] F5LowHint=false silences it.
void UpdateF5LowHint(::Config* config);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();

std::string FinishedPictureStatus();
bool WaitForFinishedPicture();
void FinishedPictureResetCommandList(ID3D12CommandList* cmd);
void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
// Streamline app-facing buffer, still PRESENT, on its original render queue before interpolation.
void ApplyToStreamlinePicture(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue);

// The native input producer's entry: runs DLSS-NR on the picture `color` (a back buffer in the PRESENT state, or any resource) with a depth and a motion
// texture of the picture's size made from the depth finder and the optical flow (both NON_PIXEL_SHADER_RESOURCE, motion in
// full-resolution pixels towards the previous frame). Records onto `cmd`, a list of the caller's own, and leaves `color` in
// PRESENT. False when it did not run (the reason is in FinishedPictureStatus()).
// `colorSpace` is what the picture's values mean (NativeInputColourSpace() reads it off a swap chain); `pictureState` is the
// state `color` is in on entry and is left in. `historyDistrust` (optional) is the producer's trust mask
// (DlssNrFrameInfo::HistoryDistrust). `epoch` numbers the frame: one present count of the API the picture came from, so it
// moves by exactly 1 per presented frame and keeps moving on frames NR skips (native::PresentEpoch).
bool ApplyNativeInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                      bool depthReversed, bool reset,
                      DXGI_COLOR_SPACE_TYPE colorSpace, D3D12_RESOURCE_STATES pictureState, unsigned long long epoch);
// The same for Optical F5Low's virtual upscaler (native/VirtualUpscalerDriver.h), which keeps its upscaler backend only for
// frame generation and runs NR on the game's picture here. It skips the check that is about NR Only mode: Finished Picture
// does not have to be on (the "NR + frame generation" mode keeps it off). Needs Enable Neural Rendering.
bool ApplyVirtualUpscalerInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                               ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                               bool depthReversed, bool reset, DXGI_COLOR_SPACE_TYPE colorSpace,
                               D3D12_RESOURCE_STATES pictureState, unsigned long long epoch);
// A native-input frame went by without NR (NR off, a mode without NR, the virtual upscaler's own pass): the next NR frame
// starts its history over instead of blending with the picture from before the gap. The two entries above do this
// themselves for a frame they did not run on.
void NativeInputSkipped();
// The colour space of a swap chain's picture, from what the game set (SDR when it set nothing), for a native-input frame.
DXGI_COLOR_SPACE_TYPE NativeInputColourSpace(IDXGISwapChain* swapchain, DXGI_FORMAT format);
void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace);


// Asks the model whether it will work on Direct3D 11 at all, once, and logs the answer.
//
// The bridge exists because of a claim nobody tested: "the model refuses on DX11, it answers
// FeatureNotSupported". Nothing in this project has ever called the snippet's own D3D11 entry points
// -- it exports ten of them, implemented in ngx_d3d11.cpp and sharing CreateFeatureCommon and
// EvaluateFeatureCommon with the D3D12 path. Nothing is created and nothing changes; it resolves the
// entry points and initialises on the game's own device, which is where a refusal would appear.
void ProbeD3D11(void* d3d11Device);

// What scale this game's buffer is on, measured from the untouched copy of each frame.
//
// A suggestion only. Nothing applies it: the menu shows it and the user takes it or does not, which
// keeps the number visible and adjustable rather than a value that moved on its own. Confidence is
// how settled recent readings are -- 1 means they agree, 0 means the scene is changing under the
// measurement and no single value would serve.
struct CalibrationReading
{
    float suggestion = 0.0f;

    // How much recent readings agree. This is steadiness, not correctness: a frozen frame agrees with
    // itself perfectly, so a loading screen scores full marks for a number that means nothing. Read it
    // together with usable.
    float steadiness = 0.0f;

    unsigned long long samples = 0;

    // Whether the scene is worth measuring at all. False when the frame is already tone mapped -- the
    // divisor does nothing there and the reading would be a meaningless 0.9 -- or when too little of
    // the picture is lit to say where the top of the range is. A dark cave gives a small number very
    // steadily, which is the trap this exists to close.
    bool usable = false;
    const char* why = "";
};

CalibrationReading Calibration();

// Whether the model is loaded and running, for the overlay.
bool IsRunning();
// Private residual-upscaler status; separate from the NR model's own running status/time.
std::string DeferredDlssStatus();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// Which model backend the loaded nvngx.dll_dlssnr.dll is: "NVIDIA NGX" or "vendor-neutral port". Empty until it has been loaded.
const char* BackendName();

// What the game offers by way of exposure. Observed every frame whether or not the setting is on, so
// the menu can say whether turning it on would do anything here.
struct ExposureStatus
{
    unsigned long long seenFrames = 0;   // evaluates observed; 0 means nothing has run yet
    bool offeredNow = false;             // a texture on the most recent frame
    bool everOffered = false;            // a texture on any frame so far
    float exposure = 0.0f;               // last value read back, 0 if none
    float preExposure = 1.0f;
};

ExposureStatus GameExposureStatus();

// What the OptiScaler-owned automatic exposure (white point source 3) has measured from the frame.
ExposureStatus AutoExposureStatus();

// Automatic following the game's own exposure on an unexposed frame (shaders/dlssnr/DlssNr_FollowGame.h). D3D12 only;
// the calibration itself is read from DlssNrFollowGame::Instance().
struct FollowGameStatus
{
    bool gameExposureSeen = false; // the game supplied an exposure beside an Automatic reading
    bool following = false;        // the last frame followed the game's exposure
    // While following: how far the followed base sits from Automatic's own, in EV (D3D12; 0 on Vulkan). Beyond
    // DlssNrExposureCalibrate::kFollowDisagreementLimitEv the learned calibration is stale and wants a Re-learn.
    float disagreementEv = 0.0f;
    // Auto's found exposure (story 5) moves the white point this frame (DlssNr_ExposureScan.h FoundExposure): what the
    // menu names Auto by, since the scan can hold a match the picture is not using (SDR, a finished picture, ...)
    bool foundInForce = false;
};

FollowGameStatus FollowGameExposureStatus();

// "Tune for this scene" for Automatic exposure and Game exposure (shaders/dlssnr/DlssNr_ExposureCalibrate.h), on D3D12
// and Vulkan; the API lives in shaders/dlssnr/DlssNr_ExposureCalibrate.cpp. The menu starts a run, shows its progress
// and curve, and on Apply writes the result into the tuned slider's Trim itself.
struct ExposureCalibrationStatus
{
    bool available = false;  // a run can start on the current frames
    uint32_t source = 3;     // the white point source of the last run: 3 Automatic, 1 Game exposure (its EVs are
                             // in that source's slider units)
    std::string unavailable; // why not, when it cannot
    bool starting = false;   // asked for, not yet picked up by the render thread
    std::string startError;  // why the last start did not happen
    bool running = false;
    bool finished = false;
    float progress = 0.0f;
    float stepEv = 0.0f;      // the step on screen while running
    unsigned stepIndex = 0;   // 0-based
    unsigned stepCount = 0;
    unsigned pass = 0;        // 0-based, of `passes` over the same steps
    unsigned passes = 1;
    std::string aborted; // why the last run stopped early, empty if it did not
    float currentEv = 0.0f;
    float baseWhitePoint = 0.0f; // the base the run was tuned at (what the Trim multiplies)
    float anchorKey = 0.0f; // the scene's brightness the run was tuned at: the key of a point Tune's result is saved as
    bool changed = false; // the chosen value differs from the current one (a flat curve keeps the current)
    bool unsure = false;  // detail varied no more than the measurement's own noise: the current value is kept
    bool unrepeated = false; // the passes disagreed (firstPassEv, lastPassEv): the current is kept
    float firstPassEv = 0.0f, lastPassEv = 0.0f;
    float resultEv = 0.0f;
    bool rawAgreed = false; // both passes picked the same raw best (within a step): "Apply raw instead" is honest
    float bestRawEv = 0.0f;
    float bestBandEv = 0.0f;
    std::vector<float> ev, scoreRaw, scoreBand; // measured steps
    // What the model did at the step nearest the result: saturation (0.1 = 10% more chroma than the game's), warmth
    // (OkLab b, + warmer), shadows darkened (0.1 = 10% darker) and the share of the picture crushed toward black.
    float resultStepEv = 0.0f, resultSaturation = 0.0f, resultWarmth = 0.0f, resultShadowDarkening = 0.0f,
          resultCrushed = 0.0f;
    bool measure = false; // the run above (running, finished or stopped) is a Measure detail, not a Tune

    // "Measure detail" (the Compare section): a still scene's detail and flicker at the current settings, for A/B.
    bool measureAvailable = false;
    std::string measureUnavailable;
    unsigned measurements = 0; // taken this session
    // The latest measurement and the one before it (hasPrevious false when there is none): detail is the output's
    // band-pass detail minus the input's, raw the output's Laplacian, flicker the output's frame-to-frame change beyond
    // the input's; all display-encoded luma, frames the evaluations measured.
    struct Measurement
    {
        float detail = 0.0f, detailOut = 0.0f, detailIn = 0.0f, raw = 0.0f;
        float flicker = 0.0f, flickerOut = 0.0f, flickerIn = 0.0f;
        unsigned frames = 0;
    };
    Measurement latest, previous;
    bool hasPrevious = false;
    bool comparable = false; // latest and previous were measured at the same scale (source and white point)
    // The latest in words (DlssNrExposureCalibrate::DetailWords and the rest), and against the previous when comparable.
    std::string detailWords, flickerWords, colourWords, shadowWords, compareWords;
};

ExposureCalibrationStatus ExposureCalibration();
// `source` is the panel it was pressed in (3 Automatic, 1 Game exposure), so that panel shows the run and its result.
void StartExposureCalibration(uint32_t source);
void StartMeasureDetail(); // cancelled and dismissed like a Tune run
void CancelExposureCalibration();
void DismissExposureCalibration();

// The model resolution actually applied last frame, as a percentage of the frame it processes --
// the manual slider, or (post-SR + Auto) the derived render:output ratio. So the menu can show the
// live value instead of the stale manual one while Auto is overriding it.
int CurrentModelResolutionPercent();

// Compress screen edges (shaders/dlssnr/DlssNr_Spatial.h), as of the last frame NR ran on Direct3D 12: a status code, the
// size the model works on and the one it would have without it, and the boxes to draw on screen (fractions of the
// frame). Status::Off until a frame has run since the setting changed. The menu owns every word for it.
Spatial::Published EdgeCompressionStatus();

// The size NR hands the model, in pixels, or 0x0 before the first frame. The network pools that 2x2 before
// its body runs, so the body sees half of it (rounded up); the menu and log show both so tuning is not
// done against the wrong number.
void CurrentModelSize(unsigned int& width, unsigned int& height);

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// Reuse detail between frames (DlssNrDetailReuse.h): frames that ran the model, frames that reused the last one's detail, and frames
// that would have reused but had to run the model (reset, settings change, gap). `why` is empty while it is available
// on this route, else the reason it is not. lightMs / heavyMs: the cheapest and dearest NR GPU time over the last 16
// measured frames (0 before any), which shows how unevenly full and reused frames cost -- what frame pacing sees.
// averageMs: their mean, the real per-frame cost while full and reused frames alternate (a single reading is one or
// the other). active: reuse ran on the last NR frame (and the bottleneck reuse was then off). baseFps: the rendered
// frame rate the minimum is checked against (0 before a reading).
struct DetailReuseInfo
{
    unsigned long long full = 0, reused = 0, fallback = 0;
    unsigned long long held = 0; // reuses given up because the picture moved too fast (MotionGuard)
    std::string why;
    double lightMs = 0.0, heavyMs = 0.0, averageMs = 0.0;
    double baseFps = 0.0;
    bool active = false;
    bool holding = false;   // reuse is paused right now: the picture is moving too fast
    float dropped = -1.0f;  // share of the last measured frame that had no detail to move, or negative if unmeasured
};
DetailReuseInfo DetailReuseStatus();

// The edit cache (DlssNr_EditCache.inl), published at every evaluate so the menu never waits: running on the last
// NR frame, the share of frames the model ran on (over the last second), and why it stands aside (null while it
// runs or is off).
struct EditCacheInfo
{
    bool running = false;
    float modelShare = 0.0f;
    const char* standsAside = nullptr;
};
EditCacheInfo EditCacheStatus();

// Scene cuts the game does not flag (dlssnr/DlssNrSceneCut.h), on D3D12 game input: cuts the detector found since the
// game started, how many the game flagged too, how many it did not, and the model resets made for those (SceneCut=2).
// lateBy: how many evaluates after its frame the last answer arrived. running: the detector ran on the last NR
// evaluate (not on Optical F5Low's native input, nor with SceneCut=0); failed: it could not be made.
struct SceneCutInfo
{
    unsigned long long found = 0, flagged = 0, silent = 0, resets = 0, lateBy = 0;
    bool running = false;
    bool failed = false;
};
SceneCutInfo SceneCutStatus();

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// What the white point meter last settled on, or 0 when it is not running. For the menu.


// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
// An edit cache dump: a capture that also copies the game's motion vectors and depth (dlssnr-cachedump beside
// OptiScaler; the trigger file dlssnr-cachedump.trigger asks for one too)
void RequestCacheDump(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
