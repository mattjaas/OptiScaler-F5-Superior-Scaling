#include "pch.h"
// Exercise the production hook wrappers with two fake plugins and real DXGI queue metadata.
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include "../../OptiScaler/dlssnr/DlssNr_StreamlinePicture.h"
#include "../../OptiScaler/framegen/dlssg/DlssgTelemetry.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_FinishedReady.h"
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D call failed");
}
static void Expect(bool ok, const char* why)
{
    if (!ok)
        throw std::runtime_error(why);
}
struct Setting
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Setting DlssNrEnabled, DlssNrFinishedPicture;
    struct Mode
    {
        unsigned value = 0;
        unsigned value_or_default() const { return value; }
    };
    Mode DlssNrFinishedGameFgMode;
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
enum class GameQuirk
{
    Kcd2NrBeforeFg
};
enum class FGNvngxReplacement
{
    None,
    Other
};
struct Quirks
{
    bool enabled = false;
    bool operator[](GameQuirk) const { return enabled; }
};
namespace sl
{
enum class DLSSGMode
{
    eOff,
    eOn
};
}
struct State
{
    uint64_t frameCount = 100, dlssgLastEvaluateFrame = UINT64_MAX;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    bool isShuttingDown = false;
    Quirks gameQuirks;
    FGNvngxReplacement activeFgNvngx = FGNvngxReplacement::None;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
namespace Util
{
bool CheckForRealObject(const char*, IUnknown*, IUnknown**) { return false; }
} // namespace Util
#define LOG_INFO(...) ((void) 0)
static ID3D12Resource* expectedPicture;
static ID3D12CommandQueue* expectedQueue;
static unsigned edits = 0, passthroughEdits = 0;
static bool expectPq = false;
// DlssNrFeature_Dx12.h's private-data key; avoid pulling NGX/application headers into the harness.
static constexpr GUID colorSpaceKey = {
    0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 }
};
namespace DlssNr
{
void ApplyToStreamlinePicture(IDXGISwapChain* swapchain, ID3D12Resource* picture, ID3D12CommandQueue* queue)
{
    Expect(picture == expectedPicture, "Mixed local and game plugin buffers");
    Expect(queue == expectedQueue, "Used the presentation queue instead of the render queue");
    if (expectPq)
    {
        DXGI_COLOR_SPACE_TYPE space {};
        UINT size = sizeof(space);
        Check(swapchain->GetPrivateData(colorSpaceKey, &size, &space));
        Expect(space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, "Lost PQ metadata at handoff");
        Expect(picture->GetDesc().Format == DXGI_FORMAT_R10G10B10A2_UNORM, "Lost 10-bit application buffer");
    }
    ++edits;
}
void ApplyToStreamlinePassthrough(IDXGISwapChain*, ID3D12CommandQueue* queue)
{
    Expect(queue == expectedQueue, "Passthrough used the asynchronous presentation queue");
    ++edits;
    ++passthroughEdits;
}
} // namespace DlssNr
// The runner copies the production body unchanged, replacing only application includes with these seams.
#include "StreamlinePicture.cpp"

template <unsigned Runtime> struct Plugin
{
    inline static ComPtr<IDXGISwapChain1> chain;
    inline static ComPtr<ID3D12Resource> picture;
    inline static unsigned calls = 0, expectedEdits = 0;
    inline static bool handled = true, bufferHandled = true;
    static UINT Index(IDXGISwapChain*, bool& skip)
    {
        skip = handled;
        return Runtime;
    }
    static HRESULT Buffer(IDXGISwapChain*, UINT index, REFIID iid, void** output, bool& skip)
    {
        Expect(index == Runtime, "Called another plugin's index hook");
        skip = handled && bufferHandled;
        return picture->QueryInterface(iid, output);
    }
    static HRESULT Present(IDXGISwapChain*, UINT, UINT, bool&)
    {
        Expect(edits == expectedEdits, "NR did not run before FG consumed the picture");
        ++calls;
        return S_OK;
    }
    static HRESULT Present1(IDXGISwapChain* sc, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS*, bool& skip)
    {
        return Present(sc, interval, flags, skip);
    }
    static HRESULT Create(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain** out, bool& skip)
    {
        skip = handled;
        return chain.CopyTo(out);
    }
    static HRESULT CreateHwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1** out, bool& skip)
    {
        skip = handled;
        return chain.CopyTo(out);
    }
    static void* Get(const char* name)
    {
        if (!strcmp(name, "slHookPresent"))
            return &Present;
        if (!strcmp(name, "slHookPresent1"))
            return &Present1;
        if (!strcmp(name, "slHookCreateSwapChain"))
            return &Create;
        if (!strcmp(name, "slHookCreateSwapChainForHwnd"))
            return &CreateHwnd;
        if (!strcmp(name, "slHookGetCurrentBackBufferIndex"))
            return &Index;
        if (!strcmp(name, "slHookGetBuffer"))
            return &Buffer;
        return nullptr;
    }
};
int main()
try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q {};
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    expectedQueue = queue.Get();
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = desc.Height = 8;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Check(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &Plugin<0>::chain));
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    Check(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &Plugin<1>::chain));
    Check(Plugin<0>::chain->GetBuffer(0, IID_PPV_ARGS(&Plugin<0>::picture)));
    Check(Plugin<1>::chain->GetBuffer(1, IID_PPV_ARGS(&Plugin<1>::picture)));

    using namespace DlssNr::StreamlinePicture;
    Expect(!Wrap("slHookPresent", Plugin<1>::Get, true), "Local wrapping enabled without KCD2 quirk");
    State::Instance().gameQuirks.enabled = true;
    State::Instance().activeFgNvngx = FGNvngxReplacement::Other;
    Expect(!Wrap("slHookPresent", Plugin<1>::Get, true), "Local wrapping enabled for replacement FG");
    State::Instance().activeFgNvngx = FGNvngxReplacement::None;
    Expect(!Wrap("slHookCreateSwapChain", Plugin<0>::Get), "Native legacy path changed outside KCD2");
    auto nativePresent = reinterpret_cast<decltype(&Plugin<0>::Present)>(Wrap("slHookPresent", Plugin<0>::Get));
    auto localPresent = reinterpret_cast<decltype(&Plugin<1>::Present1)>(Wrap("slHookPresent1", Plugin<1>::Get, true));
    auto localPresentLegacy =
        reinterpret_cast<decltype(&Plugin<1>::Present)>(Wrap("slHookPresent", Plugin<1>::Get, true));
    auto nativeCreate =
        reinterpret_cast<decltype(&Plugin<0>::CreateHwnd)>(Wrap("slHookCreateSwapChainForHwnd", Plugin<0>::Get));
    auto localCreate =
        reinterpret_cast<decltype(&Plugin<1>::Create)>(Wrap("slHookCreateSwapChain", Plugin<1>::Get, true));
    Expect(nativePresent && localPresent && localPresentLegacy && nativeCreate && localCreate,
           "Missing presentation/creation wrapper");
    bool skip = false;
    ComPtr<IDXGISwapChain1> nativeChain;
    Check(nativeCreate(nullptr, queue.Get(), nullptr, nullptr, nullptr, nullptr, &nativeChain, skip));
    ComPtr<IDXGISwapChain> localChain;
    skip = false;
    Check(localCreate(nullptr, queue.Get(), nullptr, &localChain, skip));
    Expect(RenderQueue(nativeChain.Get()).Get() == queue.Get(), "Hwnd create lost the render queue");
    Expect(RenderQueue(localChain.Get()).Get() == queue.Get(), "Legacy create lost the render queue");
    const auto pq = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    Check(localChain->SetPrivateData(colorSpaceKey, sizeof(pq), &pq));
    // Wrappers/queue metadata are installed in either mode; only the selected route may edit.
    skip = false;
    Plugin<0>::expectedEdits = edits;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(edits == 0, "Janblade A/B mode unexpectedly used the pre-FG handoff");
    Config::Instance()->DlssNrFinishedGameFgMode.value = 1;
    State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOn;
    State::Instance().dlssgLastEvaluateFrame = State::Instance().frameCount;
    for (unsigned frame = 0; frame < 6; ++frame)
    {
        expectPq = false;
        skip = false;
        expectedPicture = Plugin<0>::picture.Get();
        Plugin<0>::expectedEdits = edits + 1;
        Check(nativePresent(nativeChain.Get(), 0, 0, skip));
        expectPq = true;
        skip = false;
        expectedPicture = Plugin<1>::picture.Get();
        Plugin<1>::expectedEdits = edits + 1;
        if (frame % 2)
            Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
        else
            Check(localPresentLegacy(localChain.Get(), 0, 0, skip));
    }
    Plugin<1>::expectedEdits = edits;
    skip = false;
    Check(localPresent(localChain.Get(), 0, DXGI_PRESENT_TEST, nullptr, skip));
    skip = true;
    Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
    skip = false;
    Config::Instance()->DlssNrEnabled.value = false;
    Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
    Config::Instance()->DlssNrEnabled.value = true;
    Config::Instance()->DlssNrFinishedPicture.value = false;
    Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
    Config::Instance()->DlssNrFinishedPicture.value = true;
    Plugin<1>::handled = false;
    Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
    Plugin<1>::handled = true;
    State::Instance().isShuttingDown = true;
    Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
    State::Instance().isShuttingDown = false;
    Config::Instance()->DlssNrFinishedGameFgMode.value = 0;
    Plugin<0>::expectedEdits = edits;
    skip = false;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(Plugin<0>::calls == 8 && Plugin<1>::calls == 12, "Lost or duplicated an original present");
    // A registered chain must recover when FG stops and the plugin declines its app buffers.
    // The successful Off setting takes precedence over a recent evaluate from the last FG frame.
    Config::Instance()->DlssNrFinishedGameFgMode.value = 1;
    State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOff;
    DlssgTelemetry::RecordSetOptions(1, 1, false, 0);
    Plugin<0>::handled = false;
    Plugin<0>::expectedEdits = edits + 1;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(passthroughEdits == 1, "FG Off left NR blocked by stale registration");
    Expect(RenderQueue(nativeChain.Get()).Get() == queue.Get(), "FG Off discarded render-queue registration");
    // Start without any FG options/evaluates, as some games load the plugin but leave FG unused.
    DlssgTelemetry::g_counts.optionsSeen.store(false);
    State::Instance().dlssgLastEvaluateFrame = UINT64_MAX;
    Plugin<0>::expectedEdits = edits + 1;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(passthroughEdits == 2, "NR did not run when FG had never been enabled");
    // Off/On cycles, native and local runtimes. A missing buffer while FG is On must not touch a
    // generated display buffer. Successful app handoff must never also invoke passthrough.
    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOn;
        DlssgTelemetry::RecordSetOptions(1, 1, true, 0);
        Plugin<0>::expectedEdits = edits;
        Check(nativePresent(nativeChain.Get(), 0, 0, skip));
        Plugin<0>::handled = true;
        expectPq = false;
        expectedPicture = Plugin<0>::picture.Get();
        Plugin<0>::expectedEdits = edits + 1;
        Check(nativePresent(nativeChain.Get(), 0, 0, skip));
        Expect(passthroughEdits == 2 + cycle, "App handoff or running FG also invoked passthrough");
        State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOff;
        DlssgTelemetry::RecordSetOptions(1, 1, false, 0);
        Plugin<1>::handled = false;
        Plugin<1>::expectedEdits = edits + 1;
        Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
        Expect(passthroughEdits == 3 + cycle, "Local FG Off did not recover NR");
        Plugin<1>::handled = true;
        expectPq = true;
        expectedPicture = Plugin<1>::picture.Get();
        Plugin<1>::expectedEdits = edits + 1;
        Check(localPresent(localChain.Get(), 0, 0, nullptr, skip));
        Expect(passthroughEdits == 3 + cycle, "FG Off with a valid app buffer ran NR twice");
        Plugin<0>::handled = false;
    }
    // Do not infer Off from a failed options call or a still-recent evaluate without options.
    DlssgTelemetry::RecordSetOptions(1, 1, false, 1);
    Plugin<0>::expectedEdits = edits;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    DlssgTelemetry::g_counts.optionsSeen.store(false);
    State::Instance().dlssgLastEvaluateFrame = State::Instance().frameCount;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    DlssgTelemetry::RecordSetOptions(1, 1, false, 0);
    State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOn;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    State::Instance().dlssgLastSetMode = sl::DLSSGMode::eOff;
    // Some plugins handle the index but decline GetBuffer after FG is disabled.
    Plugin<0>::handled = true;
    Plugin<0>::bufferHandled = false;
    Plugin<0>::expectedEdits = edits + 1;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(passthroughEdits == 6, "Declined GetBuffer did not recover NR");
    // A missing queue must leave the ordinary lower route alone, never compose before Present.
    Check(nativeChain->SetPrivateDataInterface(renderQueueKey, nullptr));
    Plugin<0>::expectedEdits = edits;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(passthroughEdits == 6, "Passthrough ran without a registered render queue");
    Check(nativeChain->SetPrivateDataInterface(renderQueueKey, queue.Get()));
    Plugin<0>::handled = false;
    // Test/skip/disabled/shutdown guards apply to passthrough too.
    Check(nativePresent(nativeChain.Get(), 0, DXGI_PRESENT_TEST, skip));
    skip = true;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    skip = false;
    Config::Instance()->DlssNrEnabled.value = false;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Config::Instance()->DlssNrEnabled.value = true;
    Config::Instance()->DlssNrFinishedPicture.value = false;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Config::Instance()->DlssNrFinishedPicture.value = true;
    State::Instance().isShuttingDown = true;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    State::Instance().isShuttingDown = false;
    Config::Instance()->DlssNrFinishedGameFgMode.value = 0;
    Check(nativePresent(nativeChain.Get(), 0, 0, skip));
    Expect(passthroughEdits == 6, "A passthrough guard allowed an extra edit");
    puts("PASS: FG Off recovery, no-FG startup, repeated Off/On, native/local separation, no duplicate edits");
    puts("PASS: KCD2-only real FG gate; local legacy + native Hwnd registration; separate plugin buffers; "
         "PQ/10-bit metadata; edit before FG; live A/B selection; skip/disabled/shutdown guards");
    return 0;
}
catch (const std::exception& error)
{
    puts(error.what());
    return 1;
}
