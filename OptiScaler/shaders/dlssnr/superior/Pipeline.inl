// Included by DlssNr_Dx12.cpp after janblade's model/resource helpers.
// Model creation, pass profiles, native/bridge entry points and the UI remain janblade's.
// Only Superior's scaling, guides, inter-pass and stabilization live here. Shader ABI/bytecode: v59.
namespace
{
namespace SuperiorRun
{
using Constants = DlssNr::Superior::DlssNrConstants;
namespace Fields = DlssNr::Superior::Fields;
using Microsoft::WRL::ComPtr;
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Frames to wait for the private DLSS creation's submission before giving up on it for this session.
constexpr unsigned kCreationWaitFrames = 240;

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = Uav;
    ID3D12Resource* Get() const { return resource.Get(); }
    void To(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next)
    {
        if (resource && state != next)
            Barrier(cmd, resource.Get(), state, next);
        state = next;
    }
};

// Whether a handle made under NGX runtime `epoch` still exists. NGX shutdown destroys every feature and parameter
// block itself, so after it (or after a restart) the handle is dropped rather than released or evaluated.
bool NgxAlive(uint64_t epoch) { return NVNGXProxy::IsDx12Inited() && NVNGXProxy::Dx12Epoch() == epoch; }

struct Generation
{
    ComPtr<ID3D12Device> device;
    unsigned width = 0, height = 0, workWidth = 0, workHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool inverted = false, beforeSr = false, reset = true;
    // temporal retains the signed-residual guided reconstruction path; stabilize selects private NGX inference.
    bool temporal = false, stabilize = true, packed = false;
    uint32_t diagnostics = 0;
    DlssNr::Spatial::Layout layout {};
    Texture warp, warpCache;
    ComPtr<ID3D12Resource> warpUpload;
    unsigned preset = 1;
    // Creation is gated like the NR feature's: the first evaluate waits for a later submission epoch.
    const void* creation = nullptr; // identity only, to notice a creation recording thrown away by Reset
    bool creationSubmitted = false;
    unsigned long long createEpoch = 0;
    unsigned waitedFrames = 0;
    uint64_t ngxEpoch = 0;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Texture depth, motion, interLow, interWorking, finalLow;
    Texture carrier, temporalOutput, temporalMotion, unit;
    uint32_t expectedMask = 0, successfulMask = 0;
    unsigned previousPasses = 0, lastCompleted = 0, lastCovered = 0;
    bool haveSchedule = false;
    // Forget the NGX handles without releasing them: NGX shut down (or restarted) and destroyed them already.
    void DropNgx()
    {
        feature = nullptr;
        parameters = nullptr;
    }
    ~Generation()
    {
        if (!NgxAlive(ngxEpoch))
            return;
        if (feature && NVNGXProxy::D3D12_ReleaseFeature())
            NVNGXProxy::D3D12_ReleaseFeature()(feature);
        if (parameters && NVNGXProxy::D3D12_DestroyParameters())
            NVNGXProxy::D3D12_DestroyParameters()(parameters);
    }
};

// Superior's identities that the shared 64-entry watched table could not take (DlssNr_WatchedLists.h). Counted in
// g_overflow, so the hooks still take the lock for them, and dropped when the list is reset or destroyed.
struct Overflow
{
    std::mutex mutex;
    std::vector<const void*> lists;
};
Overflow& GetOverflow()
{
    static auto* value = new Overflow;
    return *value;
}
void Watch(const void* identity)
{
    if (identity == nullptr)
        return;
    auto& overflow = GetOverflow();
    std::lock_guard lock(overflow.mutex);
    if (std::find(overflow.lists.begin(), overflow.lists.end(), identity) != overflow.lists.end())
        return;
    if (DlssNrWatchedLists::TryAdd(identity))
        return;
    overflow.lists.push_back(identity);
    ++DlssNrWatchedLists::g_overflow;
}
void Forget(const void* identity)
{
    auto& overflow = GetOverflow();
    std::lock_guard lock(overflow.mutex);
    if (const auto it = std::find(overflow.lists.begin(), overflow.lists.end(), identity); it != overflow.lists.end())
    {
        overflow.lists.erase(it);
        --DlssNrWatchedLists::g_overflow;
    }
}
// A recorded list was destroyed (GpuLifetime's watch, on the destroying thread): it can never be submitted again.
void Destroyed(const void* identity)
{
    DlssNrWatchedLists::Remove(1, &identity);
    Forget(identity);
}

struct State
{
    DlssNr::GpuLifetime lifetime { &Destroyed };
    std::shared_ptr<Generation> current;
    bool wasSelected = false, wasActive = false, wasResidualDlss = true;
    uint32_t wasDiagnostics = 0;
    std::string status;
    std::string unavailable;   // why Superior is not running this session; empty while it runs
    bool srUnavailable = false; // the NGX DLSS SR runtime itself failed: the menu greys Superior out
};
// Same process-local ownership as janblade's g_nr. Keep unresolved GPU ownership at teardown.
State& GetState()
{
    static auto* value = new State;
    return *value;
}
// Set by the first Superior frame. Until then the NGX shutdown path does not touch the NR lock at all.
std::atomic_bool g_used = false;

void Say(const char* text)
{
    auto& s = GetState();
    if (s.status == text)
        return;
    s.status = text;
    if (*text)
        LOG_INFO("Superior Scaling: {}", text);
}

// Retire the current generation: its resources and NGX handles go once every list that used them has completed.
void Drop()
{
    auto& s = GetState();
    if (!s.current)
        return;
    auto old = std::move(s.current);
    s.lifetime.Retire([old] {});
    s.lifetime.BeginGeneration();
}

// For the rest of the session (until Superior is selected again) the ordinary path runs, with Matched residual.
void GiveUp(const std::string& reason, bool srUnavailable)
{
    auto& s = GetState();
    s.unavailable = reason;
    s.srUnavailable = s.srUnavailable || srUnavailable;
    LOG_WARN("Superior Scaling unavailable: {}; using Matched residual instead", reason);
    Say("");
    Drop();
}

void Record(ID3D12GraphicsCommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.Record(cmd);
    Watch(Util::RealObjectIdentity(cmd));
}

void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& s = GetState();
    s.lifetime.Submitted(queue, count, lists);
    // The hook stopped watching these lists after this submission. A list whose recording is still open can be
    // submitted again (replayed) and is still to be Reset, so it stays watched until then; others are forgotten.
    if (s.current && s.current->creation)
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == s.current->creation)
                s.current->creationSubmitted = true;
    for (UINT i = 0; i < count; ++i)
        if (s.lifetime.Open(lists[i]))
            Watch(lists[i]);
        else
            Forget(lists[i]);
}

void ResetRecording(ID3D12CommandList* cmd)
{
    auto& s = GetState();
    s.lifetime.ResetRecording(cmd);
    Forget(cmd);
    // The private DLSS creation was recorded on a list that is being reset without ever being submitted:
    // those creation commands never run, so the feature must be built again.
    if (s.current && s.current->feature && s.current->creation == cmd && !s.current->creationSubmitted)
    {
        Drop();
        g_nr.reset = true;
    }
}

bool Ensure(Texture& t, ID3D12Device* device, DXGI_FORMAT format, unsigned w, unsigned h)
{
    if (t.resource)
    {
        const auto d = t.resource->GetDesc();
        if (d.Width == w && d.Height == h && d.Format == format)
            return true;
        auto old = std::move(t.resource);
        GetState().lifetime.Retire([old] {});
    }
    t.state = Uav;
    t.resource.Attach(CreateScratch(device, format, w, h));
    return t.resource != nullptr;
}

bool Dispatch(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Constants& c, ID3D12Resource* source,
              ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* motion, ID3D12Resource* previous,
              ID3D12Resource* low, Texture& output, Texture* keep = nullptr, uint32_t* immutableSlot = nullptr)
{
    output.To(cmd, Uav);
    if (keep)
        keep->To(cmd, Uav);
    if (!shader.DispatchSuperiorPass(cmd, c, source, model, original, motion, previous, low, output.Get(),
                                     keep ? keep->Get() : nullptr, immutableSlot))
        return false;
    output.To(cmd, Srv);
    if (keep)
        keep->To(cmd, Srv);
    return true;
}

Constants GuideConstants(unsigned w, unsigned h, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame,
                         bool pixelUnits, bool hold)
{
    Constants c {};
    c.Mode = 10u;
    c.Width = w;
    c.Height = h;
    Fields::SetDepthRegion(c, regions.depth.x, regions.depth.y, regions.depth.width, regions.depth.height);
    Fields::SetMotionRegion(c, regions.motion.x, regions.motion.y, regions.motion.width, regions.motion.height);
    c.MvScaleX = c.MvScaleY = 1.0f;
    if (pixelUnits)
    {
        const auto rw = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
        const auto rh = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
        c.MvScaleX = frame.MvScaleX * (float(w) / std::max(rw ? rw : regions.motion.width, 1u));
        c.MvScaleY = frame.MvScaleY * (float(h) / std::max(rh ? rh : regions.motion.height, 1u));
        if (hold)
            c.MvScaleX = c.MvScaleY = 0;
    }
    return c;
}

// Immutable 11-float4 warp metadata uses the resolve's otherwise unused D3D12 motion SRV (t3).
// The generation owns its upload until all recorded command lists have retired. No per-frame copy/dispatch.
bool CreateWarp(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    if (!g.packed)
        return true;
    if (!Ensure(g.warp, g.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 11, 1))
        return false;
    const auto data = pw::MakeShaderConstants(g.layout.warp);
    static_assert(sizeof(data) == 11 * 16);
    const auto desc = g.warp.Get()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&g.warpUpload))))
        return false;
    void* mapped = nullptr;
    const D3D12_RANGE noRead { 0, 0 };
    if (FAILED(g.warpUpload->Map(0, &noRead, &mapped)))
        return false;
    std::memcpy(static_cast<char*>(mapped) + footprint.Offset, &data, sizeof(data));
    g.warpUpload->Unmap(0, nullptr);
    g.warp.To(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = g.warpUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    dst.pResource = g.warp.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.warp.To(cmd, Srv);
    // Optional acceleration. Failure leaves the immutable metadata and arithmetic PSOs usable.
    if (DlssNr::SuperiorScaling::Diagnostics::CachedWarp(g.diagnostics) &&
        Ensure(g.warpCache, g.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, std::max(11u, std::max(g.width, g.height)),
               3) &&
        shader.BuildSuperiorWarpCache(cmd, g.width, g.height, g.warp.Get(), g.warpCache.Get()))
        g.warpCache.To(cmd, Srv);
    else
        g.warpCache.resource.Reset();
    return true;
}

enum class Created
{
    Ok,
    ResourcesFailed, // our own surfaces or passes: worth another try once Superior is selected again
    SrUnavailable    // the NGX DLSS SR runtime: not here on this machine/runtime
};

NVSDK_NGX_Result CreateFeature(Generation& g, ID3D12GraphicsCommandList* cmd, unsigned preset)
{
    auto* p = g.parameters;
    for (const char* key :
         { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        p->Set(key, preset);
    g.feature = nullptr;
    return NVNGXProxy::D3D12_CreateFeature()(cmd, NVSDK_NGX_Feature_SuperSampling, p, &g.feature);
}

Created CreateTemporal(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd)
{
    Record(cmd); // includes unit-exposure initialization even if NGX creation fails
    const auto w = g.workWidth, h = g.workHeight;
    if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, w, h) ||
        !Ensure(g.temporalMotion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h) || !CreateWarp(g, shader, cmd) ||
        !Ensure(g.unit, g.device.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1))
        return Created::ResourcesFailed;
    Constants unit {};
    unit.Mode = 7u;
    unit.Width = unit.Height = 1u;
    // Mode 7 only writes 1.0 (precompile/dlssnr.hlsl, "gMode == 7") and reads no SRV. g_nr.output, at rest in UAV
    // state, is just the stand-in every SRV slot must hold; it is never accessed through that descriptor.
    if (!Dispatch(shader, cmd, unit, g_nr.output, nullptr, nullptr, nullptr, nullptr, nullptr, g.unit))
        return Created::ResourcesFailed;
    if (!NVNGXProxy::InitDx12(g.device.Get()) || !NVNGXProxy::D3D12_AllocateParameters() ||
        !NVNGXProxy::D3D12_DestroyParameters() || !NVNGXProxy::D3D12_CreateFeature() ||
        !NVNGXProxy::D3D12_EvaluateFeature() || !NVNGXProxy::D3D12_ReleaseFeature())
        return Created::SrUnavailable;
    g.ngxEpoch = NVNGXProxy::Dx12Epoch();
    if (NVNGXProxy::D3D12_AllocateParameters()(&g.parameters) != NVSDK_NGX_Result_Success || !g.parameters)
    {
        g.parameters = nullptr;
        return Created::SrUnavailable;
    }
    auto* p = g.parameters;
    p->Set(NVSDK_NGX_Parameter_Width, w);
    p->Set(NVSDK_NGX_Parameter_Height, h);
    p->Set(NVSDK_NGX_Parameter_OutWidth, w);
    p->Set(NVSDK_NGX_Parameter_OutHeight, h);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                           NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                           (g.inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    Record(cmd);
    auto result = CreateFeature(g, cmd, g.preset);
    if (result != NVSDK_NGX_Result_Success || !g.feature)
    {
        // A runtime without the requested preset (newer 310.x DLLs dropped the CNN presets A-E): its default.
        static std::once_flag once;
        std::call_once(once, [&]
                       { LOG_WARN("Superior Scaling: private DLSS preset {} unavailable ({:X}); using the runtime's "
                                  "default preset", g.preset, (unsigned) result); });
        result = CreateFeature(g, cmd, NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    }
    g.creation = Util::RealObjectIdentity(cmd);
    return result == NVSDK_NGX_Result_Success && g.feature ? Created::Ok : Created::SrUnavailable;
}

Constants Shaping(const Config& cfg, unsigned first, unsigned completed,
                  DlssNr::SuperiorScaling::FrequencyScales scales)
{
    Constants c {};
    c.ResidualHistoryValid = 1u;
    c.ResidualBlend = .015f;
    c.ResidualScale = 3.0f;
    c.ResidualConfidenceSensitivity = 1.0f;
    const auto gains = DlssNr::SuperiorScaling::BlockGains(cfg, first, completed, scales, g_nr.superiorMainOnlyGains);
    Fields::SetBandGains(c, gains.high, gains.low);
    Fields::SetInterPassLowField(c, true);
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    return c;
}

bool InterPass(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, const Config& cfg,
               ID3D12Resource* nativeProxy, ID3D12Resource* originalBase, ID3D12Resource* answer, unsigned covered,
               unsigned completed, DlssNr::SuperiorScaling::FrequencyScales scales)
{
    const auto desc = originalBase->GetDesc();
    const unsigned workW = unsigned(desc.Width), workH = desc.Height;
    auto c = Shaping(cfg, covered, completed, scales);
    c.DirectResolveFlags |= 16u | 64u;
    if (Fields::HighGain(c) == Fields::LowGain(c))
    {
        Fields::SetInterPassLowField(c, false);
        c.DirectResolveFlags |= 32u;
    }
    ID3D12Resource* low = nullptr;
    if (Fields::InterPassLowField(c))
    {
        if (!Ensure(g.interLow, g.device.Get(), desc.Format, (workW + 7u) / 8u, (workH + 7u) / 8u))
            return false;
        Constants l {};
        l.Mode = 34u;
        l.Width = (workW + 7u) / 8u;
        l.Height = (workH + 7u) / 8u;
        l.DirectResolveFlags = 32768u;
        if (!Dispatch(shader, cmd, l, originalBase, answer, nullptr, nullptr, nullptr, nullptr, g.interLow))
            return false;
        low = g.interLow.Get();
    }
    if (!Ensure(g.interWorking, g.device.Get(), desc.Format, workW, workH))
        return false;
    c.Mode = 36u;
    c.Width = workW;
    c.Height = workH;
    return Dispatch(shader, cmd, c, originalBase, answer, nativeProxy, nullptr, nullptr, low, g.interWorking);
}

ID3D12Resource* Stabilize(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                          ID3D12Resource* answer, ID3D12Resource* originalMotion, const DlssNr::GuideRegions& regions,
                          const DlssNrFrameInfo& frame, bool reset, bool hold, ID3D12Resource* packedDepth,
                          ID3D12Resource* packedMotion)
{
    if (g.diagnostics & DlssNr::SuperiorScaling::Diagnostics::Carrier)
    {
        if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, g.workWidth, g.workHeight))
            return nullptr;
        Constants c {};
        c.Mode = 40u;
        c.Width = g.workWidth;
        c.Height = g.workHeight;
        c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
        if (!Dispatch(shader, cmd, c, proxy, answer, nullptr, nullptr, nullptr, nullptr, g.carrier))
            return nullptr;
        g.reset = false;
        return g.carrier.Get();
    }
    if (!g.stabilize)
    {
        if (!Ensure(g.carrier, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, g.workWidth, g.workHeight) ||
            !Ensure(g.temporalOutput, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, g.workWidth, g.workHeight))
            return nullptr;
        Constants c {};
        // Same encode, FP16 carrier storage and decode as the temporal path; only NGX inference is skipped.
        c.Mode = 37u;
        c.Width = g.workWidth;
        c.Height = g.workHeight;
        if (!Dispatch(shader, cmd, c, proxy, answer, nullptr, nullptr, nullptr, nullptr, g.temporalOutput))
            return nullptr;
        c.Mode = 38u;
        if (!Dispatch(shader, cmd, c, g.temporalOutput.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.carrier))
            return nullptr;
        g.reset = false;
        return g.carrier.Get();
    }
    if (!g.feature || !g.parameters || !NgxAlive(g.ngxEpoch))
        return nullptr;
    auto encode = GuideConstants(g.workWidth, g.workHeight, regions, frame, true, hold);
    encode.Mode = 39u; // v50: fixed carrier + motion; reuse this frame's exact NR depth.
    auto* motion = originalMotion;
    if (g.packed)
    {
        if (!packedDepth || !packedMotion)
            return nullptr;
        // Packed vectors already contain PwPack(p+v)-PwPack(p), in packed pixels. Never scale/warp twice.
        motion = packedMotion;
        Fields::SetMotionRegion(encode, 0, 0, g.workWidth, g.workHeight);
        encode.MvScaleX = encode.MvScaleY = hold ? 0.f : 1.f;
        // Fused carrier pass preserves the exact finite packed vector, sanitizes non-finite pairs,
        // and writes zero motion while held. No extra resample or guide dispatch is needed.
    }
    if (!Dispatch(shader, cmd, encode, proxy, answer, motion, nullptr, nullptr, nullptr, g.carrier, &g.temporalMotion))
        return nullptr;
    g.temporalOutput.To(cmd, Uav);
    auto* p = g.parameters;
    p->Set(NVSDK_NGX_Parameter_Color, g.carrier.Get());
    p->Set(NVSDK_NGX_Parameter_Output, g.temporalOutput.Get());
    p->Set(NVSDK_NGX_Parameter_Depth, g.packed ? packedDepth : g.depth.Get());
    p->Set(NVSDK_NGX_Parameter_MotionVectors, g.temporalMotion.Get());
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, g.unit.Get());
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, g.workWidth);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, g.workHeight);
    p->Set(NVSDK_NGX_Parameter_Reset, (unsigned) (reset || g.reset));
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
           std::isfinite(frame.FrameTimeMs) && frame.FrameTimeMs > 0 ? frame.FrameTimeMs : 16.67f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    if (NVNGXProxy::D3D12_EvaluateFeature()(cmd, g.feature, p, nullptr) != NVSDK_NGX_Result_Success)
    {
        g.reset = true;
        Say("Private DLSS evaluation failed; keeping the game frame.");
        return nullptr;
    }
    g.temporalOutput.To(cmd, Srv);
    Constants decode {};
    decode.Mode = 38u;
    decode.Width = g.workWidth;
    decode.Height = g.workHeight;
    if (!Dispatch(shader, cmd, decode, g.temporalOutput.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, g.carrier))
        return nullptr;
    g.reset = false;
    return g.carrier.Get();
}

enum class Began
{
    Run,        // record Superior this frame
    Wait,       // private DLSS creation recorded; skip NR until its submission epoch has passed (one frame)
    Unavailable // Superior cannot run: GiveUp() was called, skip this frame, the next one runs the ordinary path
};

// Called before janblade records its normal input/exposure path. Only private DLSS
// creation gets a submission-only frame, exactly like NR feature creation above it.
Began Begin(DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Device* device, DXGI_FORMAT format,
            unsigned width, unsigned height, unsigned workWidth, unsigned workHeight, const DlssNrFrameInfo& frame,
            bool temporal, bool haveDepth, const DlssNr::Spatial::Layout* layout)
{
    auto& state = GetState();
    g_used = true;
    state.lifetime.Collect();
    const auto diagnostics = DlssNr::SuperiorScaling::Diagnostics::Mask(*Config::Instance());
    const bool stabilize = DlssNr::SuperiorScaling::Diagnostics::PrivateDlss(
        Config::Instance()->DlssNrSuperiorResidualDlss.value_or_default(), diagnostics);
    if (temporal && stabilize && !haveDepth)
    {
        GiveUp("temporal stabilization needs the game's depth and motion guides", false);
        return Began::Unavailable;
    }
    // WarpMap.hlsli maps uv * (gWidth, gHeight) -- the frame size the resolve and warp cache are dispatched at -- through
    // PwPack, which expects pixels of the layout's native size (gNativeWorkSize.xy). The packed layout is built from this
    // same frame, so they are equal; refuse a layout that is not rather than warp with the wrong scale.
    if (layout && (layout->nativeW != width || layout->nativeH != height))
    {
        GiveUp("its packed layout does not match the frame size", false);
        return Began::Unavailable;
    }
    const unsigned preset =
        DlssNr::SuperiorScaling::Preset(Config::Instance()->DlssNrSuperiorScalingPreset.value_or_default());
    if (auto* previous = state.current.get())
    {
        // Handles from a runtime NGX has since shut down are gone already: never evaluate or release them.
        const bool ngxGone = (previous->feature || previous->parameters) && !NgxAlive(previous->ngxEpoch);
        if (ngxGone)
            previous->DropNgx();
        // The game's queue is not part of the key: NGX records on the given list and the lifetime tracker follows
        // the lists to whichever queue runs them.
        if (ngxGone || previous->device.Get() != device || previous->width != width || previous->height != height ||
            previous->workWidth != workWidth || previous->workHeight != workHeight || previous->format != format ||
            previous->inverted != frame.DepthInverted || previous->beforeSr != frame.BeforeUpscale ||
            previous->temporal != temporal || previous->stabilize != stabilize ||
            previous->diagnostics != diagnostics || previous->packed != (layout != nullptr) ||
            (layout && previous->layout != *layout) || previous->preset != preset)
        {
            Drop();
            g_nr.reset = true;
        }
    }
    if (!state.current)
    {
        state.current = std::make_shared<Generation>();
        auto& g = *state.current;
        g.device = device;
        g.width = width;
        g.height = height;
        g.workWidth = workWidth;
        g.workHeight = workHeight;
        g.format = format;
        g.inverted = frame.DepthInverted;
        g.beforeSr = frame.BeforeUpscale;
        g.temporal = temporal;
        g.stabilize = stabilize;
        g.diagnostics = diagnostics;
        g.packed = layout != nullptr;
        if (layout)
            g.layout = *layout;
        g.preset = preset;
    }
    auto& g = *state.current;
    if (temporal && !stabilize && g.packed && !g.warp.Get() && !CreateWarp(g, shader, cmd))
    {
        GiveUp("its packed warp metadata could not be created", false);
        return Began::Unavailable;
    }
    if (temporal && stabilize && !g.feature)
    {
        switch (CreateTemporal(g, shader, cmd))
        {
        case Created::Ok:
            g.createEpoch = frame.SubmissionEpoch;
            g.waitedFrames = 0;
            Say("Waiting for private DLSS creation submission.");
            return Began::Wait;
        case Created::ResourcesFailed:
            GiveUp("its surfaces or passes could not be created", false);
            return Began::Unavailable;
        case Created::SrUnavailable:
            GiveUp("private NVIDIA DLSS SR (NGX) is not available", true);
            return Began::Unavailable;
        }
    }
    // A later call is not proof the creation was submitted (some engines record more than one upscale per list):
    // the submission epoch must have moved on, as for the NR feature itself.
    if (temporal && stabilize && g.waitedFrames != UINT_MAX)
    {
        if (frame.SubmissionEpoch == g.createEpoch)
        {
            if (++g.waitedFrames > kCreationWaitFrames)
            {
                GiveUp("the private DLSS creation was never submitted", false);
                return Began::Unavailable;
            }
            return Began::Wait;
        }
        g.waitedFrames = UINT_MAX; // ready
        Say("");
    }
    Record(cmd);
    return Began::Run;
}

// The NGX D3D12 runtime is going away; it destroys every handle itself, so ours are dropped, not released.
void NgxShutdown()
{
    auto& s = GetState();
    if (s.current)
        s.current->DropNgx();
    Drop();
}

// NR shutdown: retire the current generation and run what has already completed; anything still in flight keeps its
// ownership for process teardown (like DeferredSr::Shutdown), never released under the GPU.
void Shutdown()
{
    auto& s = GetState();
    Drop();
    s.lifetime.Collect();
}

bool MatchGuides(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth,
                 ID3D12Resource* motion, const DlssNr::GuideRegions& regions, const DlssNrFrameInfo& frame)
{
    if (!Ensure(g.depth, g.device.Get(), DXGI_FORMAT_R32_FLOAT, g.workWidth, g.workHeight) ||
        !Ensure(g.motion, g.device.Get(), DXGI_FORMAT_R32G32_FLOAT, g.workWidth, g.workHeight))
        return false;
    const auto c = GuideConstants(g.workWidth, g.workHeight, regions, frame, false, false);
    return Dispatch(shader, cmd, c, depth, motion, nullptr, nullptr, nullptr, nullptr, g.depth, &g.motion);
}

ID3D12Resource* FinalLow(Generation& g, DlssNr_Dx12& shader, ID3D12GraphicsCommandList* cmd, ID3D12Resource* proxy,
                         ID3D12Resource* residual, bool signedResidual)
{
    const auto desc = proxy->GetDesc();
    const unsigned lowW = (unsigned(desc.Width) + 7u) / 8u, lowH = (desc.Height + 7u) / 8u;
    if (!Ensure(g.finalLow, g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, lowW, lowH))
        return nullptr;
    Constants c {};
    c.Mode = 18u;
    c.Transfer = signedResidual ? 1u : 0u;
    c.DirectResolveFlags = DlssNr::SuperiorScaling::ShaderFlag;
    c.Width = lowW;
    c.Height = lowH;
    return Dispatch(shader, cmd, c, proxy, residual, nullptr, nullptr, nullptr, nullptr, g.finalLow) ? g.finalLow.Get()
                                                                                                     : nullptr;
}
} // namespace SuperiorRun
} // namespace
