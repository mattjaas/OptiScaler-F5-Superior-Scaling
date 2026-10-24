#pragma once

// The composition pass for Neural Rendering.
//
// Neural Rendering is two things, and only one of them is a shader. The model is an NGX feature --
// created and evaluated, not dispatched -- and that stays where it is. This is the other half: the
// pass that builds the tone-mapped proxy the model is shown, and then transfers the model's answer
// back onto the real frame.
//
// It is an ordinary compute shader with a constant struct, so it belongs here alongside RCAS and
// Output Scaling rather than owning a bespoke root signature and descriptor ring of its own.
//
// One shader, three modes, because all three read and write the same set of resources and differ
// only in what they compute:
//
//   Encode   the frame -> a tone-mapped proxy, plus an untouched copy to transfer against later
//   Down     the proxy -> a smaller proxy, when the model is asked to work below full resolution
//   Resolve  proxy + model answer + untouched copy -> the frame, edited

#include "DlssNr_Common.h"
#include "superior/Fields.h"
#include "DlssNr_DetailReuseConstants.h"
#include "DlssNr_EditCacheConstants.h"
#include "DlssNr_LutConstants.h"
#include "DlssNr_Spatial.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <dlssnr/DlssNr_Lut.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Three dispatches are recorded per frame and several frames can be in flight at once, more so with
// frame generation. Each dispatch needs descriptors and constants the GPU is not still reading, so
// there has to be enough for three passes times the deepest pipeline we might sit behind.
// Descriptor and constant slots, consumed one per dispatch and reused round-robin with no fence.
//
// A frame records the meter, its reduce, eye adaptation, the exposure courier, encode, downsample, one clamp per
// extra model pass, resolve and, during a Tune run, the stats pass: about ten with the default passes. The model
// layers themselves are NGX evaluates and do not consume this ring; their A/B resources and feature histories are
// persistent. Reuse detail between frames (DlssNr_DetailReuse.inl) adds up to six more on a full frame (compose,
// estimate, coverage, steady or the spread, capture, the spread's shown capture) and seven on a reused one (estimate,
// coverage, fill, save motion, the spread's estimate and blend, capture), which skips the clamps.
// Compress screen edges (DlssNr_Spatial.inl) adds three (colour pack, depth + motion pack, unpack), and replaces Reuse
// detail's, which does not run beside it. The edit cache (DlssNr_EditCache.inl) adds seven on a cached frame (ring,
// context, carry, three cell levels, apply) and up to five on a refresh (ring, compose, context, capture, a crossfade).
// Ninety-six slots leave four or more such frames before descriptor/constant reuse; many more passes leave fewer.
#define DLSSNR_NUM_OF_HEAPS 96

// Superior Scaling's own passes (superior/Shader.inl) go through a separate ring, made only the first time Superior
// runs, so nobody else pays for it. Up to two dispatches per pass boundary plus about ten per frame: six in-flight
// 30-pass frames.
#define DLSSNR_SUPERIOR_NUM_OF_HEAPS 416

// Goes through the same constant-buffer ring as DlssNrConstants.
static_assert(sizeof(DlssNrDetailReuseConstants) == sizeof(DlssNrConstants));
static_assert(sizeof(DlssNrEditCacheConstants) == sizeof(DlssNrConstants));
static_assert(sizeof(DlssNr::Spatial::Constants) == sizeof(DlssNrConstants));

class DlssNr_Dx12 : public Shader_Dx12, public DlssNr_Common
{
  private:
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];

    // One constant buffer per heap, not one for the class.
    //
    // The shared buffer in the base class suits a shader that dispatches once a frame. Three
    // dispatches recorded onto one command list all map and overwrite the same upload buffer before
    // any of them executes, so every pass ends up reading whichever constants were written last --
    // encode and downsample would run with the resolve's parameters.
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] = {};

    uint32_t _heapIndex = 0;

    std::unique_ptr<FrameDescriptorHeap[]> _superiorHeaps;
    ID3D12Resource* _superiorConstantBuffers[DLSSNR_SUPERIOR_NUM_OF_HEAPS] = {};
    uint32_t _superiorHeapIndex = 0;
    bool _superiorRingAttempted = false;
    bool _superiorRingReady = false;
    bool EnsureSuperiorRing();

    ID3D12PipelineState* _superiorResolve[6] {};
    bool _superiorResolveAttempted[6] {};
    ID3D12PipelineState* _superiorNormalResolve[15] {};
    bool _superiorNormalResolveAttempted[15] {};
    ID3D12PipelineState* _superiorNativeResolve[7] {};
    bool _superiorNativeResolveAttempted[7] {};
    ID3D12PipelineState* _superiorSkipGuided[4] {};
    bool _superiorSkipGuidedAttempted[4] {};
    ID3D12PipelineState* _superiorWarpCache = nullptr;
    bool _superiorWarpCacheAttempted = false;
    ID3D12PipelineState* SuperiorResolvePipeline(const DlssNrConstants& params, bool rawAnswer, bool packed,
                                                 bool cachedWarp);
    ID3D12PipelineState* _superiorRawLow = nullptr;
    bool _superiorRawLowAttempted = false;
    ID3D12PipelineState* SuperiorRawLowPipeline();
    ID3D12PipelineState* _superiorPipeline[17] {};
    bool _superiorAttempted[17] {};
    ID3D12PipelineState* SuperiorPipeline(const DlssNr::Superior::DlssNrConstants&, ID3D12Resource*);

    // The shader reads five inputs and writes two, and not every mode uses all of them. Unused slots
    // still need a view bound -- an unbound descriptor is not an empty read, it is a read from
    // nothing -- so a stand-in is written into whichever are spare.
    static constexpr uint32_t kSrvCount = 6; // t5: read only by the detail reuse pass (its history distrust)
    static constexpr uint32_t kUavCount = 2;

    uint32_t _numThreadsX = 8;
    uint32_t _numThreadsY = 8;

    // Finished Picture's second compute PSO, built from dlssnr_finished_color.hlsl's own blob,
    // reusing this class's root signature and descriptor table. Kept separate so the main
    // dlssnr.hlsl blob is never regenerated (a current dxc produces materially different DXIL
    // from the committed one). Null on backends/builds where the finished-colour shader is absent.
    ID3D12PipelineState* _finishedColorPipelineState = nullptr;

    // "Tune for this scene"'s stats pass (dlssnr_detail_stats.hlsl), built on first use the same way. Only
    // dispatched while a calibration runs.
    ID3D12PipelineState* _detailStatsPipelineState = nullptr;

    // Automatic exposure's eye adaptation pass (dlssnr_exposure_adapt.hlsl), built on first use the same way. Not
    // retried once it failed to build: the meter then writes the exposure directly, as before.
    ID3D12PipelineState* _exposureAdaptPipelineState = nullptr;
    bool _exposureAdaptPipelineFailed = false;

    // Reuse detail between frames (dlssnr_detail_reuse.hlsl), built on first use the same way. Not retried once it
    // failed to build.
    ID3D12PipelineState* _detailReusePipelineState = nullptr;
    bool _detailReusePipelineFailed = false;
    ID3D12PipelineState* _editCachePipelineState = nullptr;
    bool _editCachePipelineFailed = false;

    // Compress screen edges (dlssnr_spatial.hlsl): the colour pack / unpack and the depth + motion pack are two
    // pipelines of one shader, built on first use the same way. Not retried once they failed to build.
    ID3D12PipelineState* _spatialPipelineState = nullptr;
    ID3D12PipelineState* _spatialGuidesPipelineState = nullptr;
    ID3D12PipelineState* _superiorSpatialGuides[4] = {};
    bool _superiorSpatialGuidesAttempted[4] = {};
    bool _spatialPipelineFailed = false;

    // The LUT pass (dlssnr_lut.hlsl, LUT-apply epic Story 2: dlssnr-lut-apply).
    // Its own root signature, PSO and small descriptor-heap ring -- not the shared table above -- because it
    // is the first pass here to sample a 3D texture, and keeping it separate means DispatchPass/
    // DispatchDetailReuse/DispatchExposureAdapt/DispatchResidualPass and their compiled bytecode need no
    // changes at all. Eight heap/constant-buffer slots: this dispatches at most once a frame, unlike the 64
    // the shared ring budgets for several passes deep in flight.
    static constexpr uint32_t kLutHeapCount = 8;
    ID3D12RootSignature* _lutRootSignature = nullptr;
    ID3D12PipelineState* _lutPipelineState = nullptr;
    bool _lutPipelineFailed = false;
    FrameDescriptorHeap _lutHeaps[kLutHeapCount];
    ID3D12Resource* _lutConstantBuffers[kLutHeapCount] = {};
    uint32_t _lutHeapIndex = 0;

    // What is currently loaded (DlssNr_Lut.h's CPU half) and the GPU texture it was uploaded into (DEFAULT
    // heap, TEXTURE3D, R16G16B16A16_FLOAT, size^3). Neither depends on render resolution, so neither is
    // parked by NrState's resolution-change handling -- only by a changed/cleared LutFile.
    DlssNr_LutState _lutState;
    ID3D12Resource* _lutTexture = nullptr;
    // The path _lutTexture's content was actually uploaded from -- the cache key, not _lutTextureSize alone:
    // two different .cube files sharing a lattice size (17/33/65 are near-universal) must not look like the
    // same texture just because neither resized it. Size is still tracked (_lutTextureSize) because the
    // resource itself must be recreated, not merely re-uploaded, when it changes.
    std::string _lutTextureSourcePath;
    int _lutTextureSize = 0;

    // Lazily builds the root signature, PSO and heap ring on first use -- so a game that never sets LutFile
    // never allocates any of it. Not retried once it failed to build, same as the other lazy PSOs here.
    bool LutPipelineReady();

    // Builds/rebuilds _lutTexture from _lutState.lut when the loaded file (path, not just size) changed,
    // recording the upload on InCmdList. False if there is nothing loaded or the texture could not be
    // (re)built.
    bool EnsureLutTexture(ID3D12GraphicsCommandList* InCmdList);

    // Releases _lutTexture and forgets its source path, so the next EnsureLutTexture call re-uploads from
    // scratch rather than reading _lutTextureSourcePath against a texture that no longer exists.
    void ReleaseLutTexture();

  public:
    bool BuildSuperiorWarpCache(ID3D12GraphicsCommandList* cmd, unsigned width, unsigned height,
                                ID3D12Resource* metadata, ID3D12Resource* output);
    DlssNr_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Dx12();

    // The pass. Resources in, and nothing read from anywhere the caller cannot see.
    //
    // This is the whole filter: it brings the model up if it is not already, builds the feature and
    // rebuilds it when the tuning or the resolution changes, evaluates it, and runs the compute passes
    // that show it the frame and bring its answer back. One call, like any other shader here.
    //
    // Sizes come from the resources. Everything the pass cannot work out for itself is in
    // DlssNrFrameInfo; everything the user chose stays in Config. colour and output may be the same
    // resource. timingQueue is the queue this list will be executed on, when the caller knows it.
    void Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue* timingQueue = nullptr);

    // Records one pass. Resources that a given mode does not read may be null; a stand-in is bound in
    // their place so every descriptor in the table is valid.
    // One compute pass. The public entry below drives three of these plus the model.
    bool DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                  ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                  ID3D12Resource* InMotion,
                  // Vestigial. Fed to the slot the removed edit accumulator read its history from;
                  // nothing reads it now and every caller passes nullptr. Kept only so the binding
                  // table keeps its shape -- not evidence that temporal accumulation exists.
                  ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                  ID3D12Resource* OutKeep, ID3D12Resource* InLow = nullptr, ID3D12PipelineState* overridePipeline = nullptr);

    // v59 shaders keep their ABI and share the existing descriptor/constant ring.
    bool DispatchSuperiorPass(ID3D12GraphicsCommandList* cmd, const DlssNr::Superior::DlssNrConstants& constants,
                              ID3D12Resource* source, ID3D12Resource* model, ID3D12Resource* original,
                              ID3D12Resource* motion, ID3D12Resource* previous, ID3D12Resource* low,
                              ID3D12Resource* target, ID3D12Resource* keep, uint32_t* immutableSlot = nullptr);

    // One compute pass of Finished Picture's shader (dlssnr_finished_color.hlsl). Same descriptor
    // table shape as DispatchPass; binds _finishedColorPipelineState instead of _pipelineState.
    // t4/u1 are bound with a stand-in for parity. Returns false (no-op) if that PSO is absent.
    bool DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                              ID3D12Resource* InSource, ID3D12Resource* InModel,
                              ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                              ID3D12Resource* OutTarget);

    // One stats pass of "Tune for this scene" (dlssnr_detail_stats.hlsl): 64x64 thread groups, one per tile, into
    // the 256x64 RGBA32F grid. Same descriptor table shape as DispatchPass. False (no-op) if the PSO cannot be built.
    bool DispatchDetailStats(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                             ID3D12Resource* InOutput, ID3D12Resource* InPrevOutput, ID3D12Resource* InInput,
                             ID3D12Resource* InPrevInput, ID3D12Resource* InProxy, ID3D12Resource* OutGrid);

    // Automatic exposure's eye adaptation (dlssnr_exposure_adapt.hlsl, DlssNr_ExposureAdapt.h): one thread eases the
    // 1x1 OutEased toward the 1x1 InReading. ExposureAdaptReady builds the PSO on first use and says whether there is
    // one; DispatchExposureAdapt is false (no-op) without it. Same descriptor table shape as DispatchPass.
    bool ExposureAdaptReady();
    bool DispatchExposureAdapt(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InReading, ID3D12Resource* OutEased);

    // Reuse detail between frames (dlssnr_detail_reuse.hlsl, DlssNr_DetailReuse.inl). DetailReuseReady builds the PSO
    // on first use and says whether there is one; DispatchDetailReuse is false (no-op) without it. Same descriptor
    // table shape as DispatchPass; null inputs get t0 as a stand-in, a null OutSecond gets OutTarget. Dispatches Width
    // x Height threads. InHistoryDistrust is t5 (DlssNrDetailReuseConstants::HistoryDistrust), read by Reproject and
    // Estimate only.
    bool DetailReuseReady();
    bool DispatchDetailReuse(ID3D12GraphicsCommandList* InCmdList, const DlssNrDetailReuseConstants& InConstants,
                             unsigned int Width, unsigned int Height, ID3D12Resource* In0, ID3D12Resource* In1,
                             ID3D12Resource* In2, ID3D12Resource* In3, ID3D12Resource* In4, ID3D12Resource* OutTarget,
                             ID3D12Resource* OutSecond, ID3D12Resource* InHistoryDistrust = nullptr);

    // The edit cache (dlssnr_edit_cache.hlsl, DlssNr_EditCache.inl). EditCacheReady builds the PSO on first use;
    // DispatchEditCache is false (no-op) without it. Same descriptor table shape as DispatchPass (t0-t5, u0-u1); null
    // inputs get In0 as a stand-in, a null OutSecond gets OutTarget. Dispatches Width x Height threads.
    bool EditCacheReady();
    bool DispatchEditCache(ID3D12GraphicsCommandList* InCmdList, const DlssNrEditCacheConstants& InConstants,
                           unsigned int Width, unsigned int Height, ID3D12Resource* const (&In)[kSrvCount],
                           ID3D12Resource* OutTarget, ID3D12Resource* OutSecond);

    // Compress screen edges (dlssnr_spatial.hlsl, DlssNr_Spatial.h). SpatialReady builds the pipelines on first use and
    // says whether there are any; DispatchSpatial is false (no-op) without them. Same descriptor table shape as
    // DispatchPass; Width x Height threads come from the constants. By mode:
    //   100  In0 the encoded colour (native size)                               -> Out0 the packed colour
    //   101  In1 depth, In2 motion (a null In0 gets In1)                        -> Out0 R32F depth, Out1 RG32F motion
    //   102  In0 the packed model input, In1 the model's answer                 -> Out0 the unpacked input, Out1 the answer
    //   103  as 102, and In2 the picture the uncompressed path would show the model (ordinary grid): the answer gets
    //        In2 minus the unpacked input added (the Replace curves, which never read the proxy)
    //   104  Superior's uniform-grid guide resample (method 1/2): In1 depth, In2 raw motion -> matched guides
    // The sampler is the shared linear clamp. Resources are left in the state they arrived in.
    bool SpatialReady();
    bool DispatchSpatial(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Spatial::Constants& InConstants,
                         ID3D12Resource* In0, ID3D12Resource* In1, ID3D12Resource* In2, ID3D12Resource* Out0,
                         ID3D12Resource* Out1 = nullptr, uint32_t motionMethod = 0u);

    // The LUT pass (dlssnr_lut.hlsl, DlssNr_Lut.h, LUT-apply epic Story 2:
    // dlssnr-lut-apply). Grades InSource (Width x Height, already in the colour
    // encoding InputEncoding names) through the .cube file at LutPath, scaled by Strength, into OutTarget
    // (caller-owned, same size and format as InSource). Reparses/reuploads only when LutPath differs from
    // what is already loaded. False and OutTarget untouched when LutPath is empty or fails to parse --
    // LutError() then has the reason -- so an empty setting costs nothing beyond this call's own early-out.
    // ColourIsLinearHdr (Review Pass, 2026-10-04 fix): InputEncoding alone cannot tell scene-linear
    // HDR apart from tone-mapped sRGB (both collapse to the same value), so the caller passes
    // frame.ColourIsLinearHdr directly. White carries the encode's white point fields for this frame (its WhitePoint
    // is what to divide linear light by, not the Trim on top of it) and ExposureTex the live exposure the encode reads,
    // so the LUT grades for the white the model is shown (DlssNr_LutConstants.h CopyLiveWhitePoint).
    bool DispatchLut(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InSource, ID3D12Resource* OutTarget,
                     unsigned int Width, unsigned int Height, float Strength, uint32_t InputEncoding,
                     bool ColourIsLinearHdr, const DlssNrConstants& White, ID3D12Resource* ExposureTex,
                     const std::string& LutPath);
    const std::string& LutError() const { return _lutState.error; }
};
