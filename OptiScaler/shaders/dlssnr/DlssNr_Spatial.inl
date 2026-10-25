// Compress screen edges, D3D12 (menu: NR Input, "Compress screen edges"; ini [DlssNr] Spatial*).
//
// NR works on a smaller picture: the middle of the frame stays 1:1 and the edges are squeezed into the rest of the
// space (DlssNr_Spatial.h has the layout, precompile/dlssnr_spatial.hlsl the three passes). A frame goes
//   encode -> pack the colour (100) and the depth + motion (101) -> the model at the packed size ->
//   unpack the packed model input and the model's answer to the ordinary grid (102) -> the resolve.
// The ordinary grid is the one NR works on without compression (16-pixel grid, or Superior's exact raster), so the
// resolve, the enlargement and the supersample down-leg see the same kind of pair they always did, only softer at the
// edges: both halves go through the same resampling, so what the resolve carries over to the frame is the model's
// change.
//
// Motion is packed by its end points (Pack(p + v) - Pack(p)), so the packed vectors are exact in packed pixels and the
// model gets a scale of 1. The packed textures are RGBA16F, also the model's own input and output surfaces: the answer
// is a resample of a resample, not something the swap chain's format should quantise. (So against no compression an 8
// or 10-bit game's model works on RGBA16F; what is bit-identical is the 1:1 middle between the Replace correction, mode
// 103, and the plain unpack, mode 102.)
//
// Anything that goes wrong turns compression off (Status::TurnedOff*), not NR: the next frame runs the ordinary path,
// and a change of layout (or Retry) tries again. Reuse detail between frames does not run while this does.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after DlssNr_SceneCut.inl. Dispatch calls Begin where it
// decides the frame, Prepare once the layout is known to be wanted, and the rest around the model, all under g_nrMutex.
// The menu reads published, a copy made under a lock of its own.
namespace EdgeCompression
{
namespace Sp = DlssNr::Spatial;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

Sp::Tracker tracker;

const Sp::Layout& Layout() { return tracker.layout(); }

ID3D12Resource* color = nullptr;  // the packed colour (model input)
ID3D12Resource* depth = nullptr;  // R32F
ID3D12Resource* motion = nullptr; // RG32F
ID3D12Resource* proxy = nullptr;  // the packed model input, unpacked to the ordinary grid
ID3D12Resource* answer = nullptr; // the model's answer, unpacked the same way
ID3D12Resource* proxyNative = nullptr;  // supersampling only: both averaged down to native
ID3D12Resource* answerNative = nullptr;
OS_Dx12* proxyDown = nullptr; // the down-leg for the proxy; the answer goes through g_nr.superDown, the same filter
Scaler proxyDownScaler = Scaler::Count; // the filter proxyDown was built with: it must be the answer's, or halos
unsigned int madeFor[8] = {};           // colour/ordinary/native/guide sizes the textures above were made for
bool madeSuper = false, madeUnpack = true;

std::mutex publishedLock;
Sp::Published published;

void Publish(const Sp::Layout& l, Sp::Status status)
{
    Sp::Published p = Sp::Publish(l, status, false);
    std::lock_guard<std::mutex> lock(publishedLock);
    published = p;
}

void Release()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
        ParkNrResource(*r);

    for (unsigned int& v : madeFor)
        v = 0;
}

void ReleaseDown()
{
    delete proxyDown;
    proxyDown = nullptr;
    proxyDownScaler = Scaler::Count;
}

// What makes last frame's packed picture a different one besides the layout: the formats and sizes that are packed.
uint64_t SignatureOf(DXGI_FORMAT colour, DXGI_FORMAT depthFormat, unsigned int depthW, unsigned int depthH,
                     DXGI_FORMAT motionFormat, unsigned int motionW, unsigned int motionH)
{
    uint64_t hash = 0;
    for (const uint64_t v : { (uint64_t) colour, (uint64_t) depthFormat, (uint64_t) depthW, (uint64_t) depthH,
                              (uint64_t) motionFormat, (uint64_t) motionW, (uint64_t) motionH })
        hash = Sp::Mix(hash, v);
    return hash;
}

// Decides this frame. Returns whether the picture is packed. resetHistory is set when the packed picture is a different
// one from last frame's.
bool Begin(const Config& cfg, unsigned int width, unsigned int height, float scale, bool gridOrdinary,
           bool proxyBackend, uint64_t signature, bool& resetHistory)
{
    const Sp::Settings settings = Sp::ReadSettings(cfg);
    const Sp::Tracker::Frame frame =
        tracker.Begin(settings, width, height, scale, gridOrdinary, proxyBackend, signature);
    const Sp::Layout& next = tracker.layout();

    if (frame.resetHistory)
        resetHistory = true;

    Publish(next, frame.status);

    if (frame.changed)
    {
        if (frame.active)
            LOG_INFO("DLSS-NR compress screen edges: on, the model works on {}x{} instead of {}x{} ({:.0f}% of the "
                     "pixels), middle {:.0f}%x{:.0f}% of the frame",
                     next.modelW, next.modelH, next.ordinaryW, next.ordinaryH,
                     100.0 * next.modelW * next.modelH / (double) (next.ordinaryW * (double) next.ordinaryH),
                     100.0 * (next.centerBounds.right - next.centerBounds.left),
                     100.0 * (next.centerBounds.bottom - next.centerBounds.top));
        else if (settings.enabled)
            LOG_INFO("DLSS-NR compress screen edges: not applied ({})", Sp::Describe(frame.status));
        else
            LOG_INFO("DLSS-NR compress screen edges: off");
    }

    return frame.active;
}

// Compression turned itself off; NR carries on from the next frame without it.
void TurnOff(Sp::Status why)
{
    tracker.TurnOff(why);
    Publish(tracker.layout(), why);
    LOG_WARN("DLSS-NR compress screen edges turned itself off: {}; NR continues without it", Sp::Describe(why));
}

bool Make(ID3D12Resource*& slot, ID3D12Device* device, DXGI_FORMAT format, unsigned int w, unsigned int h)
{
    slot = CreateScratch(device, format, w, h);
    return slot != nullptr;
}

// Calibration can start after Prepare in this frame. Add only its ordinary pair without replacing live packed guides.
bool PrepareUnpack(ID3D12Device* device, const Sp::Layout& l)
{
    const bool ok = (proxy || Make(proxy, device, kFormat, l.ordinaryW, l.ordinaryH)) &&
                    (answer || Make(answer, device, kFormat, l.ordinaryW, l.ordinaryH));
    if (ok)
        madeUnpack = true;
    return ok;
}

// The textures for this layout, made again when the sizes change. False if one could not be made.
bool Prepare(ID3D12Device* device, const Sp::Layout& l, bool supersample, bool unpack, DlssNr::GuideExtent guides = {})
{
    if (!guides.width || !guides.height)
        guides = { l.modelW, l.modelH };
    const unsigned int want[8] = { l.modelW,  l.modelH,  l.ordinaryW,  l.ordinaryH,
                                   l.nativeW, l.nativeH, guides.width, guides.height };

    // Unpack textures already there satisfy a frame that does not need them (e.g. after exposure calibration ends),
    // and a frame that newly needs them only adds them: neither re-makes the packed colour/depth/motion.
    if (color != nullptr && std::equal(std::begin(want), std::end(want), std::begin(madeFor)) &&
        madeSuper == supersample)
        return madeUnpack || !unpack || PrepareUnpack(device, l);

    Release();

    bool ok = Make(color, device, kFormat, l.modelW, l.modelH) &&
              Make(depth, device, DXGI_FORMAT_R32_FLOAT, guides.width, guides.height) &&
              Make(motion, device, DXGI_FORMAT_R32G32_FLOAT, guides.width, guides.height);

    if (ok && unpack)
        ok = PrepareUnpack(device, l);

    if (ok && supersample)
        ok = Make(proxyNative, device, kFormat, l.nativeW, l.nativeH) &&
             Make(answerNative, device, kFormat, l.nativeW, l.nativeH);

    if (!ok)
    {
        Release();
        return false;
    }

    std::copy(std::begin(want), std::end(want), std::begin(madeFor));
    madeSuper = supersample;
    madeUnpack = unpack;
    if (guides.width != l.modelW || guides.height != l.modelH)
        LOG_INFO("Superior Scaling: render-resolution packed depth/MV {}x{} for NR colour {}x{} (native {}x{})",
                 guides.width, guides.height, l.modelW, l.modelH, l.nativeW, l.nativeH);
    return true;
}

// NR was shut down.
void Shutdown()
{
    for (ID3D12Resource** r : { &color, &depth, &motion, &proxy, &answer, &proxyNative, &answerNative })
    {
        if (*r != nullptr)
            (*r)->Release();
        *r = nullptr;
    }

    for (unsigned int& v : madeFor)
        v = 0;

    ReleaseDown();
    tracker.Reset();
    std::lock_guard<std::mutex> lock(publishedLock);
    published = {};
}
} // namespace EdgeCompression
