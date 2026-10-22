#include "pch.h"
#include "superior_prepare_fixture.h"

int main()
{
    try
    {
        Runner r(true, 32);
        auto carrier = r.pipeline("OptiScaler/shaders/dlssnr/superior/precompile", "dlssnr_superior_carrier");
        auto reference = r.pipeline("artifacts/validation/native-center", "carrier_bypass_reference");
        unsigned cases = 0;
        size_t words = 0;
        for (auto size : { std::pair<UINT, UINT> { 1, 1 }, { 17, 9 }, { 193, 129 } })
            for (bool half : { false, true })
                for (int input = 0; input < 4; ++input)
                {
                    PrepareFixture f(r, size.first, size.second, size.first, size.second, size.first, size.second,
                                     { 0, 0, size.first, size.second }, { 0, 0, size.first, size.second }, 1, 1, input,
                                     half);
                    // Encode to the existing FP16/FP32 carrier, then decode that exact texture without NGX.
                    f.table(0, f.proxy.Get(), f.answer.Get(), f.proxy.Get(), f.oldCarrier.Get(), f.oldCarrier.Get());
                    f.table(8, f.oldCarrier.Get(), f.oldCarrier.Get(), f.oldCarrier.Get(), f.newCarrier.Get(),
                            f.newCarrier.Get());
                    f.table(16, f.proxy.Get(), f.answer.Get(), f.proxy.Get(), f.oldCarrier.Get(), f.oldCarrier.Get());
                    DlssNrConstants c {};
                    c.Width = size.first;
                    c.Height = size.second;
                    c.Mode = 38;
                    auto decode = f.cb(c);
                    c.DebugView = half ? 1u : 0u;
                    auto expected = f.cb(c);
                    r.begin();
                    f.roots();
                    f.dispatch(carrier.Get(), f.carrierCb.Get(), 0);
                    r.barrier(f.oldCarrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    f.dispatch(carrier.Get(), decode.Get(), 8);
                    r.barrier(f.newCarrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    auto actualRead = f.read(f.newCarrier.Get());
                    r.barrier(f.oldCarrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    f.dispatch(reference.Get(), expected.Get(), 16);
                    r.barrier(f.oldCarrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    auto expectedRead = f.read(f.oldCarrier.Get());
                    r.finish();
                    const auto actual = f.words(actualRead), wanted = f.words(expectedRead);
                    if (actual != wanted)
                        throw std::runtime_error("No-DLSS carrier roundtrip differs from independent reference");
                    ++cases;
                    words += actual.size();
                }
        std::cout << "No-DLSS carrier roundtrip PASS: " << cases << " cases, " << words
                  << " words; FP16/FP32 storage, [-1,2] clipping, signed HDR/nonfinite inputs and odd borders\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
