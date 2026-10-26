// Included inside namespace DlssNr. All entry points share g_nrMutex.
namespace Late
{
using Microsoft::WRL::ComPtr;
struct Slot
{
    ComPtr<ID3D12Resource> depth, motion, linear, encoded, residual;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12CommandQueue> producerQueue; // real queue that signals `ready`; held so its identity stays valid
    ID3D12CommandList* producer = nullptr; // identity only; never dereferenced
    DlssNrFrameInfo frame {};
    uint64_t ready = 0, done = 0, serial = 0;
    unsigned route = 0;
    bool pending = false, submitted = false, residualOnly = false, sceneLinear = true;
};
std::array<Slot, 4> slots;
ComPtr<ID3D12Device> device;
uint64_t serial = 0, successes = 0, stillRendering = 0;
bool loggedCrossQueue = false;
inline constexpr const char* pausedForGameFg = "Paused while the game's own frame generation is on.";
std::string status = "Waiting for a finished picture.";
bool reset = true;
// Native input's own history reset (ApplyNativeInput, ApplyVirtualUpscalerInput), apart from `reset` above: the swap chain
// path cancels, and so sets `reset`, on every present while Finished Picture is off, which is how Optical F5Low's
// "NR + frame generation" mode runs. Shared, it reset NR's history on every frame there: NR's edits flickered.
bool nativeReset = true;

// Native input's HDR10 scratch pair (no slot to keep it in): NR's linear scRGB picture and the PQ result in the picture's
// own format. Recreated when the picture's size or format changes. A replaced buffer is kept until the frame number has
// moved kRetireFrames past the frame it was replaced in: lists recorded before that may still be reading it on the GPU,
// however many resizes come in between (dragging a window edge resizes every frame). Released with the NR state (Shutdown).
struct NativeHdr
{
    struct Retired
    {
        ComPtr<ID3D12Resource> resource;
        unsigned long long epoch = 0; // the frame it was replaced in
    };

    // Well past the frames a swap chain keeps in flight (three at most here).
    static constexpr unsigned long long kRetireFrames = 8;

    ComPtr<ID3D12Resource> linear, encoded;
    std::vector<Retired> retired;
};
NativeHdr nativeHdr;

std::atomic<bool> tracking { false };
inline constexpr GUID colorSpaceKey = { 0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 } };

void Say(const char* message)
{
    if (status != message)
    {
        status = message;
        LOG_INFO("DLSS-NR finished picture: {}", message);
    }
}

bool Finished(const Slot& slot)
{
    return !slot.fence || (slot.fence->GetCompletedValue() != UINT64_MAX &&
                          slot.fence->GetCompletedValue() >= slot.done);
}

// The pair for a picture of this size and format, in frame `epoch` (native::PresentEpoch). False when it could not be made.
bool EnsureNativeHdr(const D3D12_RESOURCE_DESC& desc, unsigned long long epoch)
{
    // Buffers replaced long enough ago that no list still in flight can read them. A frame number that went backwards (a
    // new frame source) keeps them a while longer, which only costs memory.
    std::erase_if(nativeHdr.retired, [epoch](const NativeHdr::Retired& old)
                  { return epoch >= old.epoch && epoch - old.epoch > NativeHdr::kRetireFrames; });

    const auto ensure = [&](ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format)
    {
        if (resource)
        {
            const auto have = resource->GetDesc();

            if (have.Width == desc.Width && have.Height == desc.Height && have.Format == format)
                return true;

            nativeHdr.retired.push_back({ std::move(resource), epoch });
        }

        resource.Attach(CreateScratch(device.Get(), format, (unsigned) desc.Width, desc.Height));
        return resource != nullptr;
    };

    return ensure(nativeHdr.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(nativeHdr.encoded, desc.Format);
}

ID3D12CommandQueue* RealQueue(ID3D12CommandQueue* queue)
{
    ID3D12CommandQueue* real = nullptr;
    return Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &real) ? real : queue;
}

// Experimental branch-only override: allow Finished Picture with the game's own FG.
// Keep the same decision for capture, deferred processing and composition. This
// only unblocks the janblade path. The optional Streamline handoff below selects the
// before-FG path; queue-readiness and resource-lifetime safeguards apply to both.
bool PausedForGameFrameGeneration()
{
    return false;
}

void Cancel()
{
    // Submitted copies may still be in flight; their fences still protect reuse.
    for (auto& slot : slots)
        if (slot.submitted)
            slot.pending = false;
    reset = true;
}

// Called under g_nrMutex at capture, evaluate and composition. Already submitted resources retain
// their fence protection; captures recorded for another route cannot be composed after a live switch.
void CheckRoute()
{
    static unsigned previous = 0;
    const unsigned route = Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() == 1 ? 1u : 0u;
    if (previous == route)
        return;
    Cancel();
    DeferredSr::Cancel();
    g_nr.reset = true;
    previous = route;
    Say(route ? "Waiting for Streamline's real application picture (Wilsjo2)."
              : "Waiting for a finished picture (Janblade - unblocked).");
}

bool Clone(ComPtr<ID3D12Resource>& copy, ID3D12Resource* source)
{
    const auto want = source->GetDesc();
    if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1 ||
        want.DepthOrArraySize != 1 || want.MipLevels != 1)
        return false;
    if (copy)
    {
        const auto have = copy->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
            copy.Reset(); // caller checked the previous GPU fence
    }
    if (!copy)
        copy.Attach(CreateGuideClone(device.Get(), source));
    return copy != nullptr;
}

Slot* Acquire(ID3D12GraphicsCommandList* cmd)
{
    CheckRoute();
    if (!cmd || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
    { Say("This option needs a native DirectX 12 game."); return nullptr; }
    if (PausedForGameFrameGeneration())
    { Say(pausedForGameFg); return nullptr; } // no guide copies nothing will compose
    ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return nullptr;
    if (device && device != currentDevice)
    {
        Say("The graphics device changed. Restart the game to use this option.");
        return nullptr;
    }
    device = currentDevice;
    tracking.store(true);
    // Only the lightweight submission hook is needed, including when FG is disabled.
    ResTrack_Dx12::HookLateNrQueue(device.Get());
    Slot* next = nullptr;
    for (auto& slot : slots)
        if (!slot.pending && Finished(slot)) { next = &slot; break; }
    if (!next)
    {
        // This frame gets no capture: an older frame's pending one must not be composed onto its picture.
        // Captures of this frame (a game may evaluate more than once) stay.
        const auto now = State::Instance().frameCount;
        for (auto& slot : slots)
            if (slot.submitted && slot.pending && slot.frame.SubmissionEpoch < now) { slot.pending = false; reset = true; }
        Say("Waiting for the previous picture to finish.");
        return nullptr;
    }
    auto& slot = *next;
    if (!slot.commands)
    {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&slot.commands))) || FAILED(slot.commands->Close()))
        {
            slot.commands.Reset(); slot.allocator.Reset(); slot.fence.Reset();
            Say("Could not prepare the finished-picture option.");
            return nullptr;
        }
        slot.commands->SetName(L"DLSS-NR finished picture"); // named for the D3D12 device-removed report
    }
    return next;
}

void Arm(Slot& slot, ID3D12GraphicsCommandList* cmd)
{
    slot.producer = cmd;
    ID3D12GraphicsCommandList* real = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real)) slot.producer = real;
    DlssNrWatchedLists::Add(slot.producer); // the submit/reset hooks must hear about this list
    slot.serial = ++serial;
    slot.route = Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() == 1 ? 1u : 0u;
    slot.ready = slot.done + 1;
    slot.done = slot.ready;
    slot.submitted = false;
    slot.pending = true;
}

void Capture(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, bool rr)
{
    if (!params) return;
    auto* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    // Depth is optional: NR runs without it.
    if (!motion || !output)
    { Cancel(); Say("Waiting for the game's movement data."); return; }
    auto* next = Acquire(cmd);
    if (!next) return;
    auto& slot = *next;
    if (!depth) slot.depth.Reset(); // no stale depth from an earlier frame
    if ((depth && !Clone(slot.depth, depth)) || !Clone(slot.motion, motion))
    { Say("The game's depth or movement data is not supported."); return; }
    slot.residualOnly = false;
    // Copy at the NGX seam, where guide states and lifetimes are defined. Keep typed,
    // shader-readable copies until both the producing queue and NR have finished.
    for (auto pair : { std::pair { depth, slot.depth.Get() }, std::pair { motion, slot.motion.Get() } })
    {
        if (!pair.first) continue;
        Barrier(cmd, pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(pair.second, pair.first);
        Barrier(cmd, pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // The copy returns to COPY_DEST after the late dispatch.
    }
    slot.frame = {};
    auto& frame = slot.frame;
    unsigned flags = 0, gameReset = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    params->Get(NVSDK_NGX_Parameter_Reset, &gameReset);
    frame.Reset = gameReset != 0;
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.RayReconstruction = rr;
    frame.OutputWidth = (unsigned) output->GetDesc().Width;
    frame.OutputHeight = output->GetDesc().Height;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY);
    frame.ColourIsLinearHdr = false;
    frame.IndependentCommands = true;
    frame.FinishedPicture = true;
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
    frame.SubmissionEpoch = State::Instance().frameCount;
    Arm(slot, cmd);
}

bool CaptureResidual(ID3D12GraphicsCommandList* cmd, ID3D12Resource* clean, ID3D12Resource* residual,
                     float scale, bool sceneLinear)
{
    auto* next = Acquire(cmd);
    if (!next) return false;
    auto& slot = *next;
    if (!Clone(slot.residual, residual))
    { Say("The upscaled changes could not be saved."); return false; }
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(slot.residual.Get(), residual);
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.frame = {};
    slot.frame.OutputWidth = (unsigned) clean->GetDesc().Width;
    slot.frame.OutputHeight = clean->GetDesc().Height;
    slot.frame.PreExposure = scale;
    slot.frame.SubmissionEpoch = State::Instance().frameCount;
    slot.residualOnly = true;
    slot.sceneLinear = sceneLinear;
    Arm(slot, cmd);
    return true;
}

} // namespace Late

void FinishedPictureResetCommandList(ID3D12CommandList* cmd)
{
    // Nearly every reset is of a list NR never recorded on: no lock for those (DlssNr_WatchedLists.h)
    const void* list = cmd;
    if (!DlssNrWatchedLists::Any(1, &list)) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    DlssNrWatchedLists::Remove(1, &list);
    SuperiorRun::ResetRecording(cmd);
    if (g_gpuTime) g_gpuTime->ResetRecording(cmd);
    if (g_ngxTime) g_ngxTime->ResetRecording(cmd);
    if (g_scanTime) g_scanTime->ResetRecording(cmd);
    // Reuse's coverage readbacks: a recording thrown away frees its slot, since no fence will ever be signalled for it.
    DetailReuse::ResetRecording(cmd);
    SceneCut::ResetRecording(cmd); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    for (auto& slot : Late::slots)
        if (slot.pending && !slot.submitted && slot.producer == cmd)
        {
            slot.pending = false;
            slot.done = slot.ready - 1; // discarded recording: no GPU signal was promised
            Late::reset = true;
        }
}

bool WaitForFinishedPicture()
{
    if (!Late::tracking.load()) return true;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::Cancel();
    for (auto& slot : Late::slots)
    {
        // A capture-only slot signals on the producer queue, which may itself wait on a present.
        if (!slot.submitted || !TouchedBackBuffer(slot.ready, slot.done) || Late::Finished(slot)) continue;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const auto hr = slot.fence->SetEventOnCompletion(slot.done, event);
        const bool finished = SUCCEEDED(hr) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!finished) return false;
    }
    return true;
}

std::string FinishedPictureStatus()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return Late::status;
}

void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // Every submit in the process comes here, on any thread: only one with a list NR recorded on takes the lock NR
    // holds while it records (DlssNr_WatchedLists.h)
    const auto watched = reinterpret_cast<const void* const*>(lists);
    if (!DlssNrWatchedLists::Any(count, watched)) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    DlssNrWatchedLists::Remove(count, watched);
    SuperiorRun::Submitted(queue, count, lists);
    if (g_gpuTime) g_gpuTime->Submitted(queue, count, lists);
    if (g_ngxTime) g_ngxTime->Submitted(queue, count, lists);
    if (g_scanTime) g_scanTime->Submitted(queue, count, lists);
    // Reuse's coverage readbacks: a copy is safe to read once the list it was recorded on has been executed.
    DetailReuse::Submitted(queue, count, lists);
    SceneCut::Submitted(queue, count, lists); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    for (auto& slot : Late::slots)
        if (slot.pending && !slot.submitted)
            for (UINT i = 0; i < count; ++i)
                if (lists[i] == slot.producer)
                {
                    // Signal after ExecuteCommandLists, never when merely recording the copy.
                    slot.submitted = true;
                    slot.producerQueue = Late::RealQueue(queue);
                    if (FAILED(queue->Signal(slot.fence.Get(), slot.ready)))
                    {
                        // The copy already executed. Keep its unsignalled fence protecting
                        // the slot instead of treating this as a discarded recording.
                        slot.pending = false;
                        Late::Say("The graphics queue stopped. Restart the game to retry.");
                    }
                    break;
                }
}

void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
{
    swapchain->SetPrivateData(Late::colorSpaceKey, sizeof(colorSpace), &colorSpace);
}

// An HDR10 (PQ) picture goes to NR as linear scRGB and comes back as PQ. Both finished-picture paths (the game's swap chain
// above and native input below) record the two conversions here, so they cannot drift. `pictureState` is the state the
// picture is in on entry and is left in; `linear` rests in UNORDERED_ACCESS before the decode's result is read.

// PQ picture -> `linear` (R16G16B16A16_FLOAT). False when the pass could not run.
static bool RecordPqDecode(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, D3D12_RESOURCE_STATES pictureState,
                           ID3D12Resource* linear, const DlssNrConstants& conversion)
{
    Barrier(cmd, color, pictureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool decoded = g_compose->DispatchResidualPass(cmd, conversion, color, nullptr, nullptr, nullptr, linear);
    Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pictureState);
    return decoded;
}

// `linear` (NR's result) -> PQ over `color`, through `encoded` (the picture's own format). The original picture is the
// second input of the pass (what the encode keeps of it). `linear` is left in UNORDERED_ACCESS.
static void RecordPqEncode(ID3D12GraphicsCommandList* cmd, ID3D12Resource* linear, ID3D12Resource* color,
                           D3D12_RESOURCE_STATES pictureState, ID3D12Resource* encoded, const DlssNrConstants& decoded)
{
    DlssNrConstants conversion = decoded;
    conversion.Mode = 1;
    Barrier(cmd, linear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, color, pictureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (g_compose->DispatchResidualPass(cmd, conversion, linear, nullptr, color, nullptr, encoded))
    {
        Barrier(cmd, encoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(color, encoded);
        Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, encoded, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pictureState);
    Barrier(cmd, linear, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The caller holds g_nrMutex. Handoff callers read all swapchain metadata before entering
// this lock: Streamline can hold its own lock while submitting commands that take g_nrMutex.
static void ApplyFinishedColor(ID3D12Resource* picture, ID3D12CommandQueue* queue, DXGI_COLOR_SPACE_TYPE colorSpace,
                               bool gameFrameHandoff)
{
    Late::CheckRoute();
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Cancel();
        return;
    }
    if (!picture || !queue || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    Late::ComPtr<ID3D12Resource> color = picture;
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&currentDevice))) || currentDevice != Late::device)
        return;
    const auto desc = color->GetDesc();
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    // [DlssNr] ColourEncoding: Auto keeps the colour-space rule; a forced choice picks the path, which is what lets
    // an FP16 buffer that holds gamma values through as SDR (DlssNr_ColourEncoding.h).
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "finished picture", true);
    if (!screenChoice.supported)
    {
        Late::Cancel();
        Late::Say("This screen colour format is not supported.");
        return;
    }
    const bool pq = screenChoice.screen == Screen::Pq;
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    const auto epoch = State::Instance().frameCount;
    const bool residualOnly = Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
                              Config::Instance()->DlssNrDeferredDlss.value_or_default();
    auto* presentQueue = Late::RealQueue(queue);
    std::array<FinishedInput, std::tuple_size_v<decltype(Late::slots)>> inputs {};
    for (size_t i = 0; i < Late::slots.size(); ++i)
    {
        auto& slot = Late::slots[i];
        if (!slot.pending || !slot.submitted)
            continue;
        const unsigned route = Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() == 1 ? 1u : 0u;
        if (slot.route != route ||
            (!gameFrameHandoff && (epoch < slot.frame.SubmissionEpoch || epoch - slot.frame.SubmissionEpoch > 1)))
        {
            slot.pending = false;
            Late::reset = true;
            continue;
        }
        inputs[i] = { slot.residualOnly == residualOnly && slot.frame.OutputWidth == desc.Width &&
                          slot.frame.OutputHeight == desc.Height,
                      slot.serial, slot.producerQueue.Get() == presentQueue, slot.fence->GetCompletedValue(),
                      slot.ready };
    }
    // Never queue a cross-queue wait here: the producer's signal can depend on this present.
    size_t index = 0;
    const auto pick = gameFrameHandoff ? PickReadyFinishedInput(inputs.data(), inputs.size(), index)
                                       : PickFinishedInput(inputs.data(), inputs.size(), index);
    if (pick == FinishedPick::None)
        return; // loading screen, another swapchain, or this real frame was already consumed
    if (pick == FinishedPick::NotReady)
    {
        // Stays pending in case another present of this frame follows; history skips a frame.
        Late::reset = true;
        if (gameFrameHandoff)
            for (size_t i = 0; i < inputs.size(); ++i)
                if (inputs[i].eligible)
                {
                    index = i;
                    break;
                }
        const auto& input = inputs[index];
        if (!input.sameQueue && input.completed != UINT64_MAX && !Late::loggedCrossQueue)
        {
            Late::loggedCrossQueue = true;
            LOG_INFO("DLSS-NR finished picture: the game renders on a different queue than it presents on; "
                     "frames still rendering at present are skipped instead of waited for");
        }
        // Say changes are logged: only after a sustained run, not on every alternation.
        if (++Late::stillRendering == 120)
            Late::Say("Skipping frames: the game's picture is still rendering when it is presented.");
        return;
    }
    Late::stillRendering = 0;
    auto& slot = Late::slots[index];
    // Drop other submitted evaluates from this picture, not their in-flight resources.
    for (auto& other : Late::slots)
        if (other.submitted && other.serial <= slot.serial)
            other.pending = false;
    // Same queue: orders after the producer's signal. Another queue: already complete, so this never blocks.
    if ((!gameFrameHandoff && FAILED(queue->Wait(slot.fence.Get(), slot.ready))) || FAILED(slot.allocator->Reset()) ||
        FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
    {
        Late::reset = true;
        Late::Say("Could not prepare the finished picture.");
        return;
    }
    auto* cmd = slot.commands.Get();
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());
    const auto before = g_nr.successfulDispatches;
    bool appliedResidual = false;
    if (slot.residualOnly)
    {
        if (slot.encoded && (slot.encoded->GetDesc().Width != desc.Width ||
            slot.encoded->GetDesc().Height != desc.Height || slot.encoded->GetDesc().Format != desc.Format))
            slot.encoded.Reset();
        if (!slot.encoded)
            slot.encoded.Attach(CreateScratch(Late::device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
        if (slot.encoded && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DlssNrConstants apply {};
            apply.Mode = pq ? 4 : scrgb ? 3 : 2;
            apply.Width = (unsigned) desc.Width; apply.Height = desc.Height;
            apply.WhitePoint = slot.frame.PreExposure;
            apply.TransferStrength = slot.sceneLinear ? 1.0f : 0.0f;
            apply.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
            appliedResidual = g_compose->DispatchResidualPass(cmd, apply, color.Get(), nullptr,
                slot.residual.Get(), nullptr, slot.encoded.Get());
            if (appliedResidual)
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color.Get(), slot.encoded.Get());
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    else
    {
        auto frame = slot.frame;
        frame.ColourIsLinearHdr = pq || scrgb;
        frame.InputEncoding = screenChoice.shaderConversion; // gamma 2.2 re-encoded as sRGB by the shared pass
        frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
        frame.ColourEncodingForced = !screenChoice.choice.automatic;
        // Absolute display encodings: white at the game's HDR paper white (203 nits by default), in 80-nit scRGB units
        // (the PQ conversion above writes scRGB too).
        frame.WhitePointOverride =
            (pq || scrgb) ? DlssNrExposureState::PinnedWhitePoint(
                                Config::Instance()->DlssNrDisplayPaperWhiteNits.value_or_default(),
                                DlssNrExposureState::kNitsPerScrgbUnit)
                          : 0.0f;
        frame.Reset |= Late::reset;
        frame.SubmissionEpoch = epoch;
        if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
        ID3D12Resource* nrColor = color.Get();
        bool colorReady = true;
        DlssNrConstants conversion {};
        conversion.Width = (unsigned) desc.Width;
        conversion.Height = desc.Height;
        if (pq)
        {
            auto ensure = [&](Late::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format) {
                if (resource && (resource->GetDesc().Width != desc.Width || resource->GetDesc().Height != desc.Height ||
                                 resource->GetDesc().Format != format)) resource.Reset();
                if (!resource) resource.Attach(CreateScratch(Late::device.Get(), format, (unsigned) desc.Width, desc.Height));
                return resource != nullptr;
            };
            colorReady = ensure(slot.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(slot.encoded, desc.Format);
            if (colorReady)
            {
                colorReady = RecordPqDecode(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, slot.linear.Get(), conversion);
                // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
                nrColor = slot.linear.Get();
                frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }
        if (colorReady)
            g_compose->Dispatch(cmd, nrColor, slot.depth.Get(), slot.motion.Get(), nrColor, frame, queue);
        if (pq && colorReady && g_nr.successfulDispatches > before && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            RecordPqEncode(cmd, slot.linear.Get(), color.Get(), D3D12_RESOURCE_STATE_PRESENT, slot.encoded.Get(),
                           conversion);
        }
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (FAILED(cmd->Close()))
    {
        Late::Say("Could not finish the picture. Restart the game to retry.");
        slot.pending = true; // quarantine the slot; do not reuse possibly recorded NR resources
        slot.submitted = false;
        return;
    }
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    slot.done = slot.ready + 1;
    if (FAILED(queue->Signal(slot.fence.Get(), slot.done)))
    {
        Late::Say("The graphics queue stopped. Restart the game to retry.");
        return;
    }
    const bool ran = slot.residualOnly ? appliedResidual : g_nr.successfulDispatches > before;
    Late::reset = !ran;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden."
              : ran ? (slot.residualOnly  ? "Applying the pre-SR changes to the finished picture."
                       : gameFrameHandoff ? "Applying NR before Streamline interpolation (Wilsjo2)."
                                          : "Applying NR to the finished picture.")
                    : "Preparing NR for the finished picture.");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR finished picture: {} frames, {}x{}, FG {}", Late::successes,
                 desc.Width, desc.Height, State::Instance().currentFG &&
                 State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused());
}

DXGI_COLOR_SPACE_TYPE NativeInputColourSpace(IDXGISwapChain* swapchain, DXGI_FORMAT format)
{
    auto colorSpace = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceSize = sizeof(colorSpace);

    if (swapchain != nullptr)
        swapchain->GetPrivateData(Late::colorSpaceKey, &colorSpaceSize, &colorSpace);

    return colorSpace;
}

// Janblade route: keep the existing swapchain presentation path when the experiment is off.
void ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    if (State::Instance().isShuttingDown)
        return;
    // Read private data outside the NR lock even if Streamline currently owns its presentation lock.
    const auto& cfg = *Config::Instance();
    const bool handoff = cfg.DlssNrFinishedGameFgMode.value_or_default() == 1 &&
                         cfg.DlssNrFinishedPicture.value_or_default() && cfg.DlssNrEnabled.value_or_default();
    if (handoff && StreamlinePicture::RenderQueue(swapchain))
        return; // only the app-facing Present wrapper processes this chain; never its generated frames
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::CheckRoute();
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Cancel();
        return;
    }
    if (!swapchain || !queue || State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    Late::ComPtr<IDXGISwapChain3> sc;
    Late::ComPtr<ID3D12Resource> color;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
        FAILED(SdrToHdr::GameBuffer(sc.Get(), sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))))
        return;
    ApplyFinishedColor(color.Get(), queue, NativeInputColourSpace(swapchain, color->GetDesc().Format), false);
}

void ApplyToStreamlinePicture(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue)
{
    if (State::Instance().isShuttingDown || !swapchain || !picture || !queue ||
        Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() != 1)
        return;
    const auto space = NativeInputColourSpace(swapchain, picture->GetDesc().Format);
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    // Check again after acquiring the lock if the UI switched modes during the swapchain query.
    if (Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() == 1)
        ApplyFinishedColor(picture, queue, space, true);
}

void ApplyToStreamlinePassthrough(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    if (State::Instance().isShuttingDown || !swapchain || !queue ||
        Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() != 1 ||
        !Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default() ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    // Called only by the real app-facing Present wrapper with FG off, never from the lower
    // presentation hooks. Keep the registration: generated presents on other threads must still
    // be excluded when FG restarts. All swapchain calls precede g_nrMutex, as in the handoff path.
    Late::ComPtr<IDXGISwapChain3> sc;
    Late::ComPtr<ID3D12Resource> color;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
        FAILED(SdrToHdr::GameBuffer(sc.Get(), sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))))
        return;
    const auto space = NativeInputColourSpace(swapchain, color->GetDesc().Format);
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if (Config::Instance()->DlssNrFinishedGameFgMode.value_or_default() == 1)
        ApplyFinishedColor(color.Get(), queue, space, false);
}

// The body of ApplyNativeInput and ApplyVirtualUpscalerInput. `fromVirtualUpscaler` skips the Finished Picture setting,
// which is about NR Only mode, not the picture (NR + frame generation keeps it off, for the game's own upscaler
// calls). Neither refuses while frame generation has replaced a D3D11 or Vulkan game's swap chain: both then run from that
// swap chain's present on the paired D3D12 queue, whose picture is the one presented. (The first NR Only read the game's
// D3D12 swap chain at present, which under that interop is frame generation's, and so refused it.)
static bool ApplyFinishedPictureInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                                      bool depthReversed, bool reset, DXGI_COLOR_SPACE_TYPE colorSpace,
                                      D3D12_RESOURCE_STATES pictureState, unsigned long long epoch,
                                      bool fromVirtualUpscaler)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if ((!fromVirtualUpscaler && !Config::Instance()->DlssNrFinishedPicture.value_or_default()) ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Say(fromVirtualUpscaler ? "Enable Neural Rendering to use it on the game's picture."
                                      : "Native input needs Finished picture and Enable Neural Rendering on.");
        return false;
    }
    // Depth is optional: NR runs without it.
    if (!queue || !cmd || !color || !motion)
        return false;
    if (Late::PausedForGameFrameGeneration())
    {
        Late::Say(Late::pausedForGameFg);
        return false;
    }
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return false;
    if (Late::device && Late::device != currentDevice)
    {
        Late::Say("The graphics device changed. Restart the game to use this option.");
        return false;
    }
    Late::device = currentDevice;
    const auto desc = color->GetDesc();
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        // BGRA too: a Vulkan swapchain is usually B8G8R8A8 (DOOM Eternal's), and Optical F5Low's shared picture keeps
        // it, since swapping the channels on the game's queue needs a blit and the game may present from a compute queue
        // (a blit there is a GPU fault). The pass reads it through views, which order the channels, and its scratch
        // copies take the picture's own format.
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "native input", true);
    if (!screenChoice.supported)
    {
        Late::Say("This screen colour format is not supported.");
        return false;
    }
    const bool pq = screenChoice.screen == Screen::Pq;
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    {
        // The encoding report above only logs a mismatch: say once per change what the picture is taken for.
        static int lastScreen = -1;
        if (lastScreen != (int) screenChoice.screen)
        {
            lastScreen = (int) screenChoice.screen;
            LOG_INFO("DLSS-NR native input colour space: {}", pq ? "HDR10 (PQ)" : scrgb ? "scRGB" : "SDR");
        }
    }
    // HDR10 goes to NR as linear scRGB and back, through the scratch pair (the same two passes as the game's swap chain
    // path, recorded by the same helpers).
    if (pq && !Late::EnsureNativeHdr(desc, epoch))
    {
        Late::Say("Could not make the HDR10 buffers. Restart the game to retry.");
        return false;
    }
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());

    DlssNrFrameInfo frame {};
    frame.DepthInverted = depthReversed;
    frame.MvScaleX = 1.0f;
    frame.MvScaleY = 1.0f;
    frame.ColourIsLinearHdr = scrgb || pq;
    frame.InputEncoding = screenChoice.shaderConversion;
    frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
    frame.ColourEncodingForced = !screenChoice.choice.automatic;
    // Absolute display encodings: white at the game's HDR paper white, in 80-nit scRGB units (the PQ decode writes scRGB too)
    frame.WhitePointOverride =
        (scrgb || pq) ? DlssNrExposureState::PinnedWhitePoint(Config::Instance()->DlssNrDisplayPaperWhiteNits.value_or_default(),
                                                      DlssNrExposureState::kNitsPerScrgbUnit)
              : 0.0f;
    frame.Reset = reset || Late::nativeReset;
    frame.OutputWidth = (unsigned) desc.Width;
    frame.OutputHeight = desc.Height;
    frame.FinishedPicture = true;
    // Optical F5Low measured the motion on this picture, so detail reuse may move detail with it.
    frame.MotionMatchesPicture = true;
    frame.HistoryDistrust = historyDistrust;
    frame.IndependentCommands = true;
    frame.OutputArrivalState = pictureState;
    // The first evaluate waits for the epoch to move past the frame the model was made on, and detail reuse numbers
    // frames by it (a step over 1 is a skipped frame). The caller's present count (native::PresentEpoch): each comes
    // after the frame source executed the previous frame's list.
    frame.SubmissionEpoch = epoch;

    DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
    const auto before = g_nr.successfulDispatches;
    ID3D12Resource* nrColor = color;
    DlssNrConstants conversion {};
    conversion.Width = (unsigned) desc.Width;
    conversion.Height = desc.Height;

    if (pq)
    {
        if (!RecordPqDecode(cmd, color, pictureState, Late::nativeHdr.linear.Get(), conversion))
        {
            Late::Say("Could not convert the HDR10 picture.");
            return false;
        }

        // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
        nrColor = Late::nativeHdr.linear.Get();
        frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    g_compose->Dispatch(cmd, nrColor, depth, motion, nrColor, frame, queue);
    const bool ran = g_nr.successfulDispatches > before;

    if (pq && ran && Config::Instance()->DlssNrApplyModel.value_or_default())
        RecordPqEncode(cmd, nrColor, color, pictureState, Late::nativeHdr.encoded.Get(), conversion);

    Late::nativeReset = !ran;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden." :
        ran ? "Applying NR to the finished picture (native input)." : "Preparing NR for the finished picture (native input).");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR {}: {} frames, {}x{}", fromVirtualUpscaler ? "on the virtual upscaler's picture" : "native input",
                 Late::successes, desc.Width, desc.Height);
    return ran;
}

void NativeInputSkipped()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::nativeReset = true;
}

bool ApplyNativeInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                      bool depthReversed, bool reset,
                      DXGI_COLOR_SPACE_TYPE colorSpace, D3D12_RESOURCE_STATES pictureState, unsigned long long epoch)
{
    // Any frame NR did not run on (switched off, refused, still preparing) is a gap its history must not reach across
    const bool ran = ApplyFinishedPictureInput(queue, cmd, color, depth, motion, historyDistrust, depthReversed, reset,
                                               colorSpace, pictureState, epoch, false);
    if (!ran)
        NativeInputSkipped();
    return ran;
}

bool ApplyVirtualUpscalerInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                               ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                               bool depthReversed, bool reset, DXGI_COLOR_SPACE_TYPE colorSpace,
                               D3D12_RESOURCE_STATES pictureState, unsigned long long epoch)
{
    const bool ran = ApplyFinishedPictureInput(queue, cmd, color, depth, motion, historyDistrust, depthReversed, reset,
                                               colorSpace, pictureState, epoch, true);
    if (!ran)
        NativeInputSkipped();
    return ran;
}
