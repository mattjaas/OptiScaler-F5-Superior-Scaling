#pragma once

// Everything about the Neural Rendering composition pass that is not Direct3D 12.
//
// Two kinds of thing live here. The constants the shader reads, so a Vulkan implementation can share
// the struct rather than redefine it and drift; and the parameter names the model is driven by, which
// are the model's own and identical whatever API is calling it.
//
// The Direct3D 12 side is DlssNr_Dx12, which implements Shader_Dx12 the way RCAS and Output Scaling
// do. The model itself is separate again: creating and evaluating an NGX feature is not a dispatch,
// so it does not belong in a shader class.

#include <cstdint>

// Which of the passes a dispatch is. One shader, because they read and write the same set of
// resources and differ only in what they compute.
enum DlssNrMode : uint32_t
{
    DlssNrMode_Encode = 0,     // the frame -> a tone-mapped proxy, plus an untouched copy
    DlssNrMode_Resolve = 1,    // proxy + the model's answer + the untouched copy -> the edited frame
    DlssNrMode_Downsample = 2, // the proxy -> a smaller proxy, when the model works below full size
    DlssNrMode_Meter = 3,      // the exposure texture -> tile (0,0), for the white point
    DlssNrMode_Calibrate = 4,  // the untouched frame -> a grid of tile peak luminances
    DlssNrMode_EncodeResidual = 5, // NR-composed minus original; signed difference encoded around 0.5
    DlssNrMode_ApplyResidual = 6,  // decode private DLSS result and add to clean SR output
    DlssNrMode_UnitExposure = 7,   // constant exposure for the private DLSS feature
    DlssNrMode_NormalizeMotion = 8, // current-to-previous motion in normalized image coordinates
    DlssNrMode_ComposeMotion = 9, // compose two successive fields at the displaced coordinate
    DlssNrMode_ApplyInterpolatedResidual = 10, // t4: R8_UNORM NVIDIA suppression flag
    DlssNrMode_ZeroMotion = 11, // private reset-only NR/SR guide, never passed to residual FG
    DlssNrMode_ClampProxy = 12, // a pass's raw answer -> the same value saturated back into the proxy's valid range, before it becomes the next pass's input
    DlssNrMode_AutoExposure = 13, // the 64x64 meter's tile means -> one exposure value, 1x1
    DlssNrMode_Watercolor = 14,   // experimental input style: the model's picture -> a Kuwahara "watercolour" of it
    DlssNrMode_StructureHint = 15, // experimental input style: the model's picture with its local contrast raised
    DlssNrMode_SmoothCanvas = 16   // experimental input style: the model's picture through a cross bilateral filter
};

// An input style's number (DlssNr_InputStyles.h: 1..3) -> its pass and its name for the log. The pass reads its constants' SkinProtection as
// "faces only".
inline DlssNrMode StyleMode(uint32_t style)
{
    return style == 2u ? DlssNrMode_StructureHint : style == 3u ? DlssNrMode_SmoothCanvas : DlssNrMode_Watercolor;
}

inline const char* StyleName(uint32_t style)
{
    return style == 2u ? "structure hint" : style == 3u ? "smooth canvas" : "watercolour";
}

// A successful sample may be reused only on the immediately following frame.
// Failed composition, cuts and gaps must not turn a two-frame hold into a freeze.
struct DlssNrResidualHold
{
    uint64_t sampleEpoch = 0;
    bool valid = false;
    bool CanReuse(uint64_t epoch) const { return valid && epoch > sampleEpoch && epoch - sampleEpoch == 1; }
    void SampleSucceeded(uint64_t epoch) { sampleEpoch = epoch; valid = true; }
    void Reset() { valid = false; }
};

// The meter's grid. 64 x 64 tiles over the whole frame, whatever its size.
//
// Tiles rather than pixels because the number wanted is where white sits, not how bright the
// brightest pixel is: a single specular hit or a sky pixel is not the white point, and a frame's
// maximum is exactly the statistic that would be dominated by one. Averaging each tile first means
// anything smaller than a four-thousandth of the frame cannot decide the answer on its own.
//
// 4096 values is also small enough to read back and take a real percentile of on the CPU, rather
// than approximating one on the GPU.
constexpr uint32_t kDlssNrMeterGrid = 64;

// What the composition shader reads.
//
// The model does not replace the frame. It is shown a tone-mapped proxy of the picture, and its
// answer is transferred back onto the real frame -- so most of these describe how much of that answer
// to take, not what the model should do.
// Aligned to 256 because a constant buffer view's size must be a multiple of it. Without this the
// buffer is created at the struct's natural size, the view is invalid, and the device is removed a
// few milliseconds later -- with nothing in any log to say why. Every other shader here does the
// same thing; it is not optional.
// What the caller knows about the frame, and the pass cannot work out for itself.
//
// Everything here is a property of how the game encodes its buffers, not a setting: the user's
// choices -- preset, intensity, strengths, paper white -- stay in Config, so a caller placing this
// pass in a new pipeline does not have to plumb a dozen sliders through it.
//
// Allocation sizes come from the resource descriptors. Before SR, the reported render subrect
// also determines the active colour size; padded colour is copied through a compact work texture.
struct DlssNrFrameInfo
{
    // Which way round depth runs. The game states this when it creates its own upscaler.
    bool DepthInverted = false;

    // How the game encodes its motion vectors, as the game itself reports it. Passed through: every
    // resource already carries a subrect saying how big it is, so scaling by the resolution ratio on
    // top of that counts it twice.
    float MvScaleX = 1.0f;
    float MvScaleY = 1.0f;

    // Throw away the model's history. Set it on a cut, a teleport, or the first frame of a feature.
    bool Reset = false;

    // Whether the colour buffer holds linear, open-ended light or a frame that has already been
    // through a tonemapper. Getting this wrong encodes an encoded frame a second time, which looks
    // washed out and banded.
    bool ColourIsLinearHdr = true;

    // The Colour encoding override (DlssNr_ColourEncoding.h) for this frame. InputEncoding is what the shared pass
    // converts at its edges (DlssNrConstants::InputEncoding: 3 gamma 2.2, 4 PQ, else nothing); ColourEncoding is the
    // encoding that was chosen, for the log, and differs from it where another pass does the conversion (Finished
    // Picture's PQ). ColourIsLinearHdr is already the choice's LinearHdr(). Zero for every path that does not set it.
    uint32_t InputEncoding = 0;
    uint32_t ColourEncoding = 0;
    bool ColourEncodingForced = false;

    // The SR colour input arrives readable, whereas a completed upscaler output normally arrives as
    // a UAV. The DX12 pass uses this to preserve the caller's state and to fall back through a copy
    // when a pre-SR colour resource was not created with UAV support.
    bool BeforeUpscale = false;
    // Owned copy, not the game's Color: always arrives/returns NON_PIXEL_SHADER_RESOURCE.
    bool PrivateColorCopy = false;
    bool FinishedPicture = false;
    // The motion vectors describe this very picture, HUD and post-processing included, because they were measured on
    // it (Optical F5Low's native input) rather than rendered by the game for its scene. Reuse detail between frames
    // needs that to run on a finished picture.
    bool MotionMatchesPicture = false;
    // Optional: how far the previous frame's history can be trusted at each pixel, as distrust 0..1 in .r (0 trusts it
    // fully), at any size (read by uv). On native input it is Optical F5Low's trust mask. A D3D12 resource readable
    // from a compute shader, left in the state it came in. Null: no outside opinion, reuse trusts its own tests.
    void* HistoryDistrust = nullptr;
    uint32_t OutputArrivalState = 0;
    float WhitePointOverride = 0.0f;
    bool IndependentCommands = false; // owned command list, no game root signature to restore
    // Reset temporal history when switching between ordinary SR and Ray Reconstruction.
    bool RayReconstruction = false;

    // Submission epoch supplied by the caller. Native DX12 uses the wrapped swapchain Present count;
    // the DX11/Vulkan bridges use their successfully submitted frame counter. A feature created in an
    // epoch is never evaluated until this value changes.
    unsigned long long SubmissionEpoch = 0;

    // The game's own exposure: a 1x1 texture holding, in the SDK's words, "the final exposure scale".
    //
    // This is the number that makes a cave and a field comparable, and it is the reason a fixed paper
    // white cannot serve both. It comes from the game, decided before anything here runs, so unlike a
    // statistic measured off the frame it cannot be pulled around by what this pass writes.
    //
    // May be null on any given frame -- GTA V supplied it, then did not, three times in one session --
    // so whoever consumes it holds the last good value rather than falling back to a default.
    void* ExposureTexture = nullptr;

    // The game created its feature with the AutoExposure flag: it leaves exposure to the upscaler, so a texture it still
    // passes (an FSR or XeSS input forwarding its own, or a scale) is not the game's exposure. WhitePointSource auto does
    // not count it as offered.
    bool GameAutoExposure = false;

    // The scale the game multiplied its buffer by for float precision, which DLSS is told so it can
    // undo it. Usually 1. Divided out before the exposure is applied, exactly as FSR's PrepareRgb does.
    float PreExposure = 1.0f;
    float FrameTimeMs = 16.67f;

    // How much of the depth and motion vector textures the game actually rendered into.
    // Before SR this also selects the origin-zero active colour rectangle, not its allocation.
    //
    // Not the same thing as how big those textures are, and the difference is the whole point. A game
    // with dynamic resolution allocates its guides once at the largest size it will ever need and
    // then renders into the top-left corner of them, telling the upscaler how much is real through
    // DLSS.Render.Subrect.Dimensions. Sizing the guides from the resource instead means handing the
    // model whatever was left in the margin -- stale depth and stale vectors -- and calling it scene.
    //
    // Zero means the game did not say, in which case the resource's own size is the best answer
    // available and is what gets used.
    unsigned int RenderSubrectWidth = 0;
    unsigned int RenderSubrectHeight = 0;

    unsigned int DepthSubrectBaseX = 0;
    unsigned int DepthSubrectBaseY = 0;
    unsigned int MotionSubrectBaseX = 0;
    unsigned int MotionSubrectBaseY = 0;

    // DLSS permits motion vectors at either render or output resolution. This flag comes from the
    // feature-create flags and decides which valid-region dimensions apply to the motion texture.
    bool MotionVectorsLowResolution = false;
    // DLSS output extent, independent of the NR injection point or working scale.
    unsigned int OutputWidth = 0;
    unsigned int OutputHeight = 0;
};

struct alignas(256) DlssNrConstants
{
    uint32_t Mode;
    float WhitePoint;

    uint32_t Width;
    uint32_t Height;

    // How much of the model's edit lands, and how much of it is allowed to be colour rather than
    // luminance. Separating the two is what keeps saturated highlights from shifting hue.
    float TransferStrength;
    float ColourStrength;

    uint32_t DebugView;

    // A ceiling on how far a pixel may be brightened. The transfer is a ratio, and a ratio against a
    // near-black proxy pixel is unbounded without one.
    float MaxRatio;

    // Set when the game's buffer is already tone-mapped, in which case there is nothing to convert
    // and the transfer is the identity.
    uint32_t Passthrough;

    float MvScaleX;
    float MvScaleY;

    // Depth and motion vectors come from the upscaler's inputs and so may be at render resolution
    // while colour and output are at display resolution.
    uint32_t GuideWidth;
    uint32_t GuideHeight;

    // Showing the pass against itself. 0 off, 1 side by side, 2 a wipe.
    //
    // Both are drawn by the resolve rather than by a pass of their own, because the resolve is the
    // one place that already holds the frame as the upscaler produced it and the frame the model
    // edited. Comparing them anywhere else would mean keeping a second copy of one of them.
    uint32_t CompareMode;
    float CompareSplit;

    // How much of the frame side by side shows. 1 fits the whole thing at its right shape and
    // letterboxes what is left over; 2 fills the half and crops to the middle instead.
    float CompareZoom;

    // Which side the edited frame is on. Swapping matters because the eye is not even-handed about
    // left and right, so a difference can look like an improvement purely from where it sits.
    uint32_t CompareSwap;

    // How a model that worked below the frame's size is brought back. 0 classic, 1 matched residual.
    //
    // Classic composes the model's own low-resolution picture against the full-resolution frame, so
    // the two disagree by the blur the downsample introduced as well as by the edit -- and the
    // composition reads that disagreement as headroom the frame has and the model never saw. Matched
    // residual takes only the model's *difference* from low resolution and lays it on the frame's own
    // full-resolution proxy, so the two pictures being compared are at the same scale and the only
    // thing carried up from small is the edit itself.
    //
    // The idea and the cube-scaled residual are hhkbble's, from the multi-pass PR against this fork.
    uint32_t Transfer;

    // What the debug views are multiplied by on their way out.
    //
    // They have to be scaled into the frame's units or the game's tonemapper shows them wrong, but
    // scaling them by the live white point makes the instrument move with the thing being measured:
    // two captures at different exposures then differ by the exposure, whatever the edit did. This
    // is the user's own multiplier, which holds still while the meter works.
    float DebugScale;

    // The proxy curve (DlssNr_ProxyCurve.h; dlssnr.hlsl's ProxyCurve / IsReplace). 0 soft knee +
    // our composition (default), 1/2 Neutwo + composition / pure-inverse replace (model's answer
    // straight back, no composition), 3/4 the hybrid the same two ways, 5 HLG, 6 PQ, 7 linear (all three
    // composed, in scene light). Trailing field, mirroring the shader's cbuffer, so the layout stays a
    // flat run of 4-byte scalars that C++ and HLSL agree on.
    uint32_t ReversibleMode;

    // 0 = output the clean upscaler frame (the pass still runs, so Hold frame keeps a frozen frame
    // to A/B against), 1 = apply the model's edit. Trailing scalar, mirrored in the shader cbuffer.
    uint32_t ApplyModel;

    // D3D12 source-1 zero-latency exposure. UseGameExposure = 1 makes the shader read the game's live
    // exposure texture (bound at t4) instead of the CPU-resolved white point; ExposurePreMul is
    // preExposure * trim, so the live white point is ExposurePreMul / exposure. Mirrored in the cbuffer.
    uint32_t UseGameExposure;
    float ExposurePreMul;
    // Optional colour-based final-composition mask. Not the runtime's semantic mask.
    uint32_t SkinProtection;
    uint32_t ShowSkinMask;
    float SkinDetail;
    float SkinColour;
    float EnvironmentDetail;
    float EnvironmentColour;

    // Replace modes only (ReversibleMode 2/4): how much native high-frequency detail is
    // restored below 100% model resolution, where Replace has no native-resolution fallback
    // the way the composed modes do. 0 = current behaviour, unchanged. Trailing scalar,
    // mirrored in the shader cbuffer.
    float ReplaceDetailStrength;

    // The model's actual working-resolution scale this frame (`reduced && workScale < 1.0f ?
    // workScale : 1.0f`) -- computed here, not inferred in the shader from a buffer's bound
    // size, because SGSR1's pre-resolve enlarge (DX12) hands the resolve pass native-sized
    // proxy/answer buffers whenever it succeeds, which makes a shader-side "is this buffer
    // still small" check read as native (i.e. false) in the common case even though the model
    // itself ran small. 1.0 means "not reduced" and disables anything gated on it. Trailing
    // scalar, mirrored in the shader cbuffer.
    float ModelWorkScale;

    // Automatic exposure and exposure-dependent Trim. Trailing scalars, mirrored in the shader cbuffer
    // in the same order.
    //
    // The live white point is PreExposure / exposure * Trim, where Trim is either the slider
    // (ExposureTrim) or, when anchors exist, interpolated from them at that base white point. Eight
    // anchors at most, as key/trim pairs in consecutive fields. UseExposureWhitePoint asks for the
    // in-shader path for automatic exposure, which reads its own 1x1 texture; game exposure keeps
    // signalling through UseGameExposure. Left zero, none of it applies and the CPU white point is used.
    float PreExposure;
    // Superior tunable resolve only: these otherwise-unused meter fields carry inverse-variance float bits.
    uint32_t ExposureSourceWidth;
    uint32_t ExposureSourceHeight;
    uint32_t MeterCopiesExposure; // meter mode: 1 courier the game's exposure, 0 average every tile pixel
    float ExposureTrim;
    uint32_t UseExposureWhitePoint;
    uint32_t ExposureTrimAnchorCount;
    uint32_t ExposureTrimPreview; // 1 = ignore the anchors and apply the slider as it is
    float ExposureTrimAnchorExposure0;
    float ExposureTrimAnchorTrim0;
    float ExposureTrimAnchorExposure1;
    float ExposureTrimAnchorTrim1;
    float ExposureTrimAnchorExposure2;
    float ExposureTrimAnchorTrim2;
    float ExposureTrimAnchorExposure3;
    float ExposureTrimAnchorTrim3;
    float ExposureTrimAnchorExposure4;
    float ExposureTrimAnchorTrim4;
    float ExposureTrimAnchorExposure5;
    float ExposureTrimAnchorTrim5;
    float ExposureTrimAnchorExposure6;
    float ExposureTrimAnchorTrim6;
    float ExposureTrimAnchorExposure7;
    float ExposureTrimAnchorTrim7;
    float AutoExposureShadowProtection; // percent, 0..100

    // How much of a multipass boundary's raw answer to take, versus staying at this pass's own
    // proxy: next_input = proxy + PassFeedback * (restored - proxy). 1.0 (default) is exactly
    // today's behaviour -- the boundary hands the next pass the full restored answer. Lower values
    // keep every pass closer to what the model was actually trained to receive (a frame that has
    // not already been through this same model), rather than compounding further off-distribution
    // with each pass. Only read at DlssNrMode_ClampProxy; a single-pass configuration never reaches
    // this branch. Trailing scalar, mirrored in the shader cbuffer.
    float PassFeedback;

    // Multiplies the base white point the shader reads from a live exposure sample. While Automatic follows the game's
    // exposure (DlssNr_FollowGame.h) the game's texture is bound and this carries the learned calibration. 0 -- what a
    // zero-initialised struct holds -- and 1 both leave the white point as it was. Trailing scalar, mirrored in the shader.
    float ExposureBaseScale;

    // The Colour encoding override (DlssNr_ColourEncoding.h) at the two edges where the game's own frame is read and
    // written back: 3 tone-mapped gamma 2.2 (re-encoded as sRGB for the pass), 4 PQ (decoded to linear, 1.0 = 203
    // nits). Every other value -- 0, what a zero-initialised struct holds -- converts nothing: Auto, linear HDR and
    // sRGB are already what the pass works in. Set only on dispatches that read the game's frame (the encode, the
    // meters over it) or write it (the resolve). Trailing scalar, mirrored in the shader.
    uint32_t InputEncoding;

    // Resolve only: 1 when gOriginal is the game's own texture rather than the encode's kept copy (the colour cannot
    // take a UAV, so the resolve writes into the copy instead), and so still carries InputEncoding's encoding.
    uint32_t OriginalIsGameColour;
};
static_assert(sizeof(DlssNrConstants) == 256);

// The reduce dispatch (DlssNrMode_AutoExposure) has no use for the Trim fields, so the meter's choice rides in them: the
// percentile meter is ExposureTrimAnchorCount == 1, ExposureTrimAnchorExposure0 its low percent and
// ExposureTrimAnchorTrim0 its high percent (dlssnr.hlsl, DlssNr_ExposureMeter.h). Anything else is the plain mean.
inline void SetAutoExposureMeter(DlssNrConstants& params, bool percentile, float lowPercent, float highPercent)
{
    params.ExposureTrimAnchorCount = percentile ? 1u : 0u;
    params.ExposureTrimAnchorExposure0 = percentile ? lowPercent : 0.0f;
    params.ExposureTrimAnchorTrim0 = percentile ? highPercent : 0.0f;
}

class DlssNr_Common
{
  protected:
    // The model's own parameter names, spelled once.
    //
    // These are not ours to choose and they do not vary by API, which is the whole reason they are
    // here rather than in the Direct3D 12 file. Getting one wrong is silent: the model keeps its
    // previous value and the control simply appears to do nothing.
    static constexpr const char* kEnabled = "DLSSNR.Enabled";
    static constexpr const char* kWidth = "DLSSNR.Width";
    static constexpr const char* kHeight = "DLSSNR.Height";

    static constexpr const char* kColor = "DLSSNR.Color";
    static constexpr const char* kDepth = "DLSSNR.Depth";
    static constexpr const char* kMotion = "DLSSNR.MVec";
    static constexpr const char* kOutput = "DLSSNR.Output";

    static constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
    static constexpr const char* kReset = "DLSSNR.Reset";
    static constexpr const char* kMvScaleX = "DLSSNR.MVecScaleX";
    static constexpr const char* kMvScaleY = "DLSSNR.MVecScaleY";

    // Read once, while the feature is built. Writing these only at evaluate does nothing at all,
    // which is why several of them appeared to be dead controls for a long time.
    static constexpr const char* kPreset = "DLSSNR.Hint.Render.Preset";
    static constexpr const char* kIntensity = "DLSSNR.Intensity";
    static constexpr const char* kStyle = "DLSSNR.Style";
    static constexpr const char* kLocalStructure = "DLSSNR.LocalStructureStrength";
    static constexpr const char* kLocalTone = "DLSSNR.LocalToneStrength";
    // Not a parameter of this model. Kept named so nobody re-adds it: a scan of nvngx_dlssnr.dll for
    // DLSSNR.* yields 61 names and this is absent from them, while every other name here is present.
    // Writing it was harmless -- the block is string-keyed -- but it made a control look real when
    // nothing was listening, which is worse than not having one.
    // static constexpr const char* kGlobalTone = "DLSSNR.GlobalToneStrength";

    // Despite the name, this is the automatic *skin* mask, not an interface mask.
    static constexpr const char* kAutoMask = "DLSSNR.UseAutoMask";

    // Defaults to -1, meaning follow local structure. It is not a 0..1 strength and -1 is not "off".
    static constexpr const char* kSkinStructure = "DLSSNR.SkinStructureStrength";

    // The interface layer, its alpha, and the composited frame. The model accepts all three and is
    // currently given none of them: with no interface supplied there is nothing to correct.
    static constexpr const char* kUi = "DLSSNR.UI";
    static constexpr const char* kUiAlpha = "DLSSNR.UIAlpha";
    static constexpr const char* kBackbuffer = "DLSSNR.Backbuffer";
    static constexpr const char* kUiCorrection = "DLSSNR.UICorrection";
};
