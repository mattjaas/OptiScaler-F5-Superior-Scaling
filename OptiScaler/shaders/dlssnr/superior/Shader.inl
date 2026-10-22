// Included once by DlssNr_Dx12.cpp. Original v59 blobs use their own constant ABI.
namespace DlssNr::Superior::Bytecode
{
#include "precompile/dlssnr_interpass_cross5_Shader.h"
#include "precompile/dlssnr_interpass_fast_Shader.h"
#include "precompile/dlssnr_interpass_low36_cached_Shader.h"
#include "precompile/dlssnr_interpass_low36_Shader.h"
#include "precompile/DlssNr_Shader.h"
#include "precompile/dlssnr_superior_area_Shader.h"
#include "precompile/dlssnr_superior_carrier_Shader.h"
#include "precompile/dlssnr_superior_cross5_Shader.h"
#include "precompile/dlssnr_superior_encode_Shader.h"
#include "precompile/dlssnr_superior_exposure_Shader.h"
#include "precompile/dlssnr_superior_hdr0_Shader.h"
#include "precompile/dlssnr_superior_hdr2_Shader.h"
#include "precompile/dlssnr_superior_hdr4_Shader.h"
#include "precompile/dlssnr_superior_low_Shader.h"
#include "precompile/dlssnr_superior_meter_Shader.h"
#include "precompile/dlssnr_superior_normal_Shader.h"
#include "precompile/dlssnr_superior_resolve_Shader.h"
#include "precompile/dlssnr_superior_sdr_Shader.h"
} // namespace DlssNr::Superior::Bytecode

bool DlssNr_Dx12::EnsureSuperiorRing()
{
    if (_superiorRingAttempted)
        return _superiorRingReady;
    _superiorRingAttempted = true;
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNr::Superior::DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (auto& buffer : _superiorConstantBuffers)
        if (FAILED(_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer))))
        {
            LOG_ERROR("[{0}] Superior Scaling: constant buffers could not be created", _name);
            return false;
        }
    _superiorHeaps = std::make_unique<FrameDescriptorHeap[]>(DLSSNR_SUPERIOR_NUM_OF_HEAPS);
    _superiorRingReady = InitHeaps(_device, _superiorHeaps.get(), DLSSNR_SUPERIOR_NUM_OF_HEAPS);
    return _superiorRingReady;
}

ID3D12PipelineState* DlssNr_Dx12::SuperiorPipeline(const DlssNr::Superior::DlssNrConstants& constants,
                                                   ID3D12Resource* source)
{
    using namespace DlssNr::Superior;
    using namespace DlssNr::Superior::Bytecode;
    const bool carrier = constants.Mode == DlssNr::Superior::DlssNrMode_SuperiorCarrierEncode ||
                         constants.Mode == DlssNr::Superior::DlssNrMode_SuperiorCarrierDecode ||
                         constants.Mode == DlssNr::Superior::DlssNrMode_SuperiorCarrierMotion;
    if (!carrier && (constants.DirectResolveFlags & DlssNr::SuperiorScaling::ShaderFlag) == 0u)
        return nullptr;
    const auto d = (constants.Mode == DlssNr::Superior::DlssNrMode_Resolve ||
                    constants.Mode == DlssNr::Superior::DlssNrMode_Encode ||
                    constants.Mode == DlssNr::Superior::DlssNrMode_GuidedResidualLow ||
                    constants.Mode == DlssNr::Superior::DlssNrMode_Downsample ||
                    constants.Mode == DlssNr::Superior::DlssNrMode_Meter ||
                    constants.Mode == DlssNr::Superior::DlssNrMode_AutoExposure) &&
                           source
                       ? source->GetDesc()
                       : D3D12_RESOURCE_DESC {};
    const bool inputEncode = DlssNr::SuperiorScaling::InputEncode(constants, d.Width, d.Height);
    const bool lowResidual = DlssNr::SuperiorScaling::LowResidual(constants, d.Width, d.Height);
    if (constants.Mode == 18u && constants.Transfer == 0u && constants.DirectResolveFlags == 65536u && d.Width &&
        d.Height && constants.Width == (d.Width + 7u) / 8u && constants.Height == (d.Height + 7u) / 8u)
        return SuperiorRawLowPipeline();
    const bool areaDownsample = DlssNr::SuperiorScaling::AreaDownsample(constants, d.Width, d.Height);
    const auto exposureStage = DlssNr::SuperiorScaling::ExposureStage(constants, d.Width, d.Height);
    // Fixed input/exposure, Area, final low and radius-one stages; other uses retain the general fallback.
    if (!carrier && !inputEncode && !lowResidual && !areaDownsample && exposureStage == 0u &&
        constants.Mode != DlssNr::Superior::DlssNrMode_InterPassFastCross5 &&
        (constants.Mode != DlssNr::Superior::DlssNrMode_Resolve || constants.ResidualHistoryValid != 1u))
        return nullptr;
    const bool normalResolve = DlssNr::SuperiorScaling::NormalResolve(constants, d.Width, d.Height);
    const unsigned index = inputEncode           ? 7u
                           : lowResidual         ? 8u
                           : areaDownsample      ? 9u
                           : exposureStage != 0u ? 9u + exposureStage
                           : carrier             ? 0u
                           : normalResolve       ? 3u + DlssNr::SuperiorScaling::NormalColorProfile(constants)
                           : constants.Mode == DlssNr::Superior::DlssNrMode_InterPassFastCross5 ? 2u
                                                                                                : 1u;
    const void* const blobs[] { dlssnr_superior_carrier_cso, dlssnr_superior_resolve_cso, dlssnr_superior_cross5_cso,
                                dlssnr_superior_normal_cso,  dlssnr_superior_sdr_cso,     dlssnr_superior_hdr2_cso,
                                dlssnr_superior_hdr4_cso,    dlssnr_superior_encode_cso,  dlssnr_superior_low_cso,
                                dlssnr_superior_area_cso,    dlssnr_superior_meter_cso,   dlssnr_superior_exposure_cso,
                                dlssnr_superior_hdr0_cso };
    const size_t sizes[] { sizeof(dlssnr_superior_carrier_cso), sizeof(dlssnr_superior_resolve_cso),
                           sizeof(dlssnr_superior_cross5_cso),  sizeof(dlssnr_superior_normal_cso),
                           sizeof(dlssnr_superior_sdr_cso),     sizeof(dlssnr_superior_hdr2_cso),
                           sizeof(dlssnr_superior_hdr4_cso),    sizeof(dlssnr_superior_encode_cso),
                           sizeof(dlssnr_superior_low_cso),     sizeof(dlssnr_superior_area_cso),
                           sizeof(dlssnr_superior_meter_cso),   sizeof(dlssnr_superior_exposure_cso),
                           sizeof(dlssnr_superior_hdr0_cso) };
    const auto get = [&](unsigned selected) -> ID3D12PipelineState*
    {
        if (!_superiorAttempted[selected])
        {
            _superiorAttempted[selected] = true;
            if (!CreateComputePipeline(_device, &_superiorPipeline[selected], blobs[selected], sizes[selected],
                                       nullptr))
                LOG_WARN("Superior Scaling PSO {} creation failed", selected);
        }
        return _superiorPipeline[selected];
    };
    // Try literal HDR soft knee only for the normal signed-residual path. A failed
    // specialization still tries the unchanged normal PSO before the main shader.
    if (normalResolve && DlssNr::SuperiorScaling::SoftKneeResolve(constants))
        if (auto* pipeline = get(12u))
            return pipeline;
    if (auto* pipeline = get(index))
        return pipeline;
    // Input, exposure, Area and low failures retain their own mode in the main shader, never Mode 1.
    return normalResolve && index > 3u ? get(3u) : nullptr;
}

bool DlssNr_Dx12::DispatchSuperiorPass(ID3D12GraphicsCommandList* InCmdList,
                                       const DlssNr::Superior::DlssNrConstants& InConstants, ID3D12Resource* InSource,
                                       ID3D12Resource* InModel, ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                                       ID3D12Resource* InPrevEdit, ID3D12Resource* InAux2, ID3D12Resource* OutTarget,
                                       ID3D12Resource* OutKeep, uint32_t* immutableSlot)
{
    using namespace DlssNr::Superior::Bytecode;
    ID3D12PipelineState* pipeline = SuperiorPipeline(InConstants, InSource);
    const auto get = [&](unsigned index, const void* blob, size_t size)
    {
        if (!_superiorAttempted[index])
        {
            _superiorAttempted[index] = true;
            CreateComputePipeline(_device, &_superiorPipeline[index], blob, size, nullptr);
        }
        return _superiorPipeline[index];
    };
    if (!pipeline && (InConstants.Mode == 37u || InConstants.Mode == 38u || InConstants.Mode == 39u))
        return false;
    if (!pipeline && InConstants.Mode == 34u)
    {
        if (InConstants.DirectResolveFlags & 32768u)
            pipeline = get(14u, dlssnr_interpass_low36_cached_cso, sizeof(dlssnr_interpass_low36_cached_cso));
        if (!pipeline)
            pipeline = get(15u, dlssnr_interpass_low36_cso, sizeof(dlssnr_interpass_low36_cso));
    }
    if (!pipeline)
        pipeline = get(13u, DlssNr::Superior::Bytecode::DlssNr_cso, sizeof(DlssNr::Superior::Bytecode::DlssNr_cso));
    if (!_init || !pipeline || !InCmdList || !_device || !InSource || !OutTarget || !EnsureSuperiorRing())
        return false;

    const bool reuse = immutableSlot && *immutableSlot != UINT32_MAX;
    const uint32_t slot = reuse ? *immutableSlot : _superiorHeapIndex;
    if (!reuse)
        _superiorHeapIndex = (_superiorHeapIndex + 1) % DLSSNR_SUPERIOR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _superiorHeaps[slot];
    if (!reuse)
    {

        // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
        // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
        // nothing of its own to put there.
        ID3D12Resource* const srvs[kSrvCount] = {
            InSource,
            InModel != nullptr ? InModel : InSource,
            InOriginal != nullptr ? InOriginal : InSource,
            InMotion != nullptr ? InMotion : InSource,
            InPrevEdit != nullptr ? InPrevEdit : InSource,
            InAux2 != nullptr ? InAux2 : InSource,
        };

        for (uint32_t i = 0; i < kSrvCount; ++i)
        {
            // Copied depth guides already use an SRV format; the upstream translator maps it back to a DSV.
            CreateShaderResourceView(_device, srvs[i],
                                     currentHeap.GetSrvCPU(i)); // janblade already handles typed depth SRVs
        }

        ID3D12Resource* const uavs[kUavCount] = {
            OutTarget,
            OutKeep != nullptr ? OutKeep : OutTarget,
        };

        for (uint32_t i = 0; i < kUavCount; ++i)
            CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

        if (!CreateConstantsBuffer(_device, _superiorConstantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
            return false;
        }

        if (immutableSlot)
            *immutableSlot = slot;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(pipeline);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = InConstants.Mode == DlssNr::Superior::DlssNrMode_Meter
                                   ? InConstants.Width
                                   : (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    // Final-low PSO uses 8x16, the generic fallback 8x8; dispatch for the selected PSO.
    const UINT threadsY = pipeline == _superiorPipeline[8] || pipeline == _superiorRawLow
                              ? DlssNr::SuperiorScaling::LowGroupHeight
                              : _numThreadsY;
    const UINT dispatchHeight = InConstants.Mode == DlssNr::Superior::DlssNrMode_Meter
                                    ? InConstants.Height
                                    : (InConstants.Height + threadsY - 1) / threadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

namespace DlssNr::Superior::HostBytecode
{
#include "precompile/host_resolve_hdr_Shader.h"
#include "precompile/host_resolve_sdr_Shader.h"
#include "precompile/host_resolve_raw_Shader.h"
#include "precompile/host_low_raw_Shader.h"
#include "precompile/host_resolve_packed_hdr_Shader.h"
#include "precompile/host_resolve_packed_sdr_Shader.h"
#include "precompile/host_normal_sdr_Shader.h"
#include "precompile/host_normal_hdr0_Shader.h"
#include "precompile/host_normal_hdr2_Shader.h"
#include "precompile/host_normal_hdr4_Shader.h"
#include "precompile/host_normal_hdr6_Shader.h"
#include "precompile/host_normal_packed_sdr_Shader.h"
#include "precompile/host_normal_packed_hdr0_Shader.h"
#include "precompile/host_normal_packed_hdr2_Shader.h"
#include "precompile/host_normal_packed_hdr4_Shader.h"
#include "precompile/host_normal_packed_hdr6_Shader.h"
#include "precompile/host_cached_packed_sdr_Shader.h"
#include "precompile/host_cached_packed_hdr0_Shader.h"
#include "precompile/host_cached_packed_hdr2_Shader.h"
#include "precompile/host_cached_packed_hdr4_Shader.h"
#include "precompile/host_cached_packed_hdr6_Shader.h"
#include "precompile/host_warp_cache_Shader.h"
#include "precompile/host_native_packed_hdr_Shader.h"
#include "precompile/host_native_packed_sdr_Shader.h"
#include "precompile/host_native_cached_packed_sdr_Shader.h"
#include "precompile/host_native_cached_packed_hdr0_Shader.h"
#include "precompile/host_native_cached_packed_hdr2_Shader.h"
#include "precompile/host_native_cached_packed_hdr4_Shader.h"
#include "precompile/host_native_cached_packed_hdr6_Shader.h"
} // namespace DlssNr::Superior::HostBytecode

ID3D12PipelineState* DlssNr_Dx12::SuperiorResolvePipeline(const DlssNrConstants& params, bool rawAnswer, bool packed,
                                                          bool cachedWarp)
{
    using namespace DlssNr::Superior::HostBytecode;
    // Every packed PSO fixes gGuideHeight to 0 (precompile/dlssnr.hlsl, DLSSNR_SUPERIOR_HOST_RESOLVE): it adds the
    // decoded signed DLSS residual to the proxy. A raw NR answer there would be added as if it were a residual. A
    // packed generation is always temporal (NeedsTemporal), so this only guards that invariant: no PSO, game frame kept.
    if (packed && rawAnswer)
    {
        static std::once_flag once;
        std::call_once(once, [] { LOG_ERROR("Superior Scaling: packed resolve asked for a raw NR answer; skipped"); });
        return nullptr;
    }
    int normal =
        DlssNr::SuperiorScaling::NormalResolveVariant(rawAnswer, packed, params.Passthrough, params.ReversibleMode,
                                                      params.DebugView, params.CompareMode, params.ApplyModel);
    if (normal >= 5 && cachedWarp)
        normal += 5;
    // Keep all below-native PSOs byte-identical. Native identity-region filtering gets separate PSOs.
    if (packed && !rawAnswer && params.ModelWorkScale == 1.0f)
    {
        const void* blobs[] { host_native_cached_packed_sdr_cso,  host_native_cached_packed_hdr0_cso,
                              host_native_cached_packed_hdr2_cso, host_native_cached_packed_hdr4_cso,
                              host_native_cached_packed_hdr6_cso, host_native_packed_hdr_cso,
                              host_native_packed_sdr_cso };
        const size_t sizes[] { sizeof(host_native_cached_packed_sdr_cso),  sizeof(host_native_cached_packed_hdr0_cso),
                               sizeof(host_native_cached_packed_hdr2_cso), sizeof(host_native_cached_packed_hdr4_cso),
                               sizeof(host_native_cached_packed_hdr6_cso), sizeof(host_native_packed_hdr_cso),
                               sizeof(host_native_packed_sdr_cso) };
        const auto getNative = [&](unsigned index) -> ID3D12PipelineState*
        {
            if (!_superiorNativeResolveAttempted[index])
            {
                _superiorNativeResolveAttempted[index] = true;
                CreateComputePipeline(_device, &_superiorNativeResolve[index], blobs[index], sizes[index], nullptr);
            }
            return _superiorNativeResolve[index];
        };
        if (normal >= 10)
            if (auto* pso = getNative(unsigned(normal - 10)))
                return pso;
        if (auto* pso = getNative(params.Passthrough ? 6u : 5u))
            return pso;
    }
    if (normal >= 0)
    {
        auto*& pso = _superiorNormalResolve[normal];
        if (!_superiorNormalResolveAttempted[normal])
        {
            _superiorNormalResolveAttempted[normal] = true;
            const void* blobs[] {
                host_normal_sdr_cso,         host_normal_hdr0_cso,        host_normal_hdr2_cso,
                host_normal_hdr4_cso,        host_normal_hdr6_cso,        host_normal_packed_sdr_cso,
                host_normal_packed_hdr0_cso, host_normal_packed_hdr2_cso, host_normal_packed_hdr4_cso,
                host_normal_packed_hdr6_cso, host_cached_packed_sdr_cso,  host_cached_packed_hdr0_cso,
                host_cached_packed_hdr2_cso, host_cached_packed_hdr4_cso, host_cached_packed_hdr6_cso
            };
            const size_t sizes[] { sizeof(host_normal_sdr_cso),         sizeof(host_normal_hdr0_cso),
                                   sizeof(host_normal_hdr2_cso),        sizeof(host_normal_hdr4_cso),
                                   sizeof(host_normal_hdr6_cso),        sizeof(host_normal_packed_sdr_cso),
                                   sizeof(host_normal_packed_hdr0_cso), sizeof(host_normal_packed_hdr2_cso),
                                   sizeof(host_normal_packed_hdr4_cso), sizeof(host_normal_packed_hdr6_cso),
                                   sizeof(host_cached_packed_sdr_cso),  sizeof(host_cached_packed_hdr0_cso),
                                   sizeof(host_cached_packed_hdr2_cso), sizeof(host_cached_packed_hdr4_cso),
                                   sizeof(host_cached_packed_hdr6_cso) };
            CreateComputePipeline(_device, &pso, blobs[normal], sizes[normal], nullptr);
        }
        if (pso)
            return pso;
    }
    const bool sdr = params.Passthrough != 0;
    // The raw (non-temporal) resolve compiles to the same bytecode for SDR and HDR: one blob, one PSO (slot 2).
    const auto index = packed ? 4u + (sdr ? 1u : 0u) : rawAnswer ? 2u : (sdr ? 1u : 0u);
    auto*& pso = _superiorResolve[index];
    if (!_superiorResolveAttempted[index])
    {
        _superiorResolveAttempted[index] = true;
        const void* blobs[] { host_resolve_hdr_cso, host_resolve_sdr_cso,        host_resolve_raw_cso,
                              host_resolve_raw_cso, host_resolve_packed_hdr_cso, host_resolve_packed_sdr_cso };
        const size_t sizes[] { sizeof(host_resolve_hdr_cso),        sizeof(host_resolve_sdr_cso),
                               sizeof(host_resolve_raw_cso),        sizeof(host_resolve_raw_cso),
                               sizeof(host_resolve_packed_hdr_cso), sizeof(host_resolve_packed_sdr_cso) };
        CreateComputePipeline(_device, &pso, blobs[index], sizes[index], nullptr);
    }
    return pso;
}

bool DlssNr_Dx12::BuildSuperiorWarpCache(ID3D12GraphicsCommandList* cmd, unsigned width, unsigned height,
                                         ID3D12Resource* metadata, ID3D12Resource* output)
{
    using namespace DlssNr::Superior::HostBytecode;
    if (!_superiorWarpCacheAttempted)
    {
        _superiorWarpCacheAttempted = true;
        CreateComputePipeline(_device, &_superiorWarpCache, host_warp_cache_cso, sizeof(host_warp_cache_cso), nullptr);
    }
    if (!_superiorWarpCache || !metadata || !output || !cmd || !_init || !EnsureSuperiorRing())
        return false;
    DlssNr::Superior::DlssNrConstants params {};
    params.Width = width;
    params.Height = height;
    const uint32_t slot = _superiorHeapIndex;
    _superiorHeapIndex = (_superiorHeapIndex + 1) % DLSSNR_SUPERIOR_NUM_OF_HEAPS;
    auto& heap = _superiorHeaps[slot];
    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, metadata, heap.GetSrvCPU(i));
    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, output, heap.GetUavCPU(i), 0);
    if (!CreateConstantsBuffer(_device, _superiorConstantBuffers[slot], params, heap.GetCbvCPU(0)))
        return false;
    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(_rootSignature);
    cmd->SetPipelineState(_superiorWarpCache);
    cmd->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmd->Dispatch((std::max(11u, std::max(width, height)) + 7u) / 8u, 1, 1);
    return true;
}

ID3D12PipelineState* DlssNr_Dx12::SuperiorRawLowPipeline()
{
    using namespace DlssNr::Superior::HostBytecode;
    if (!_superiorRawLowAttempted)
    {
        _superiorRawLowAttempted = true;
        CreateComputePipeline(_device, &_superiorRawLow, host_low_raw_cso, sizeof(host_low_raw_cso), nullptr);
    }
    return _superiorRawLow;
}
