#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        const std::string refs = "artifacts/validation/host-shaders";
        size_t words = 0;
        unsigned cases = 0;
        for (bool packed : { false, true })
            for (bool half : { false, true })
                for (float scale : { .65f, 1.f })
                    for (unsigned curve : { 0u, 2u, 4u, 6u })
                        for (bool sdr : { false, true })
                        {
                            const UINT w = 65, h = 37;
                            DlssNr::Spatial::Settings settings;
                            settings.enabled = true;
                            settings.offsetX = 7;
                            settings.shiftY = -1;
                            const auto l = DlssNr::Spatial::Build(settings, w, h, scale, false);
                            auto metadata = UploadWarp(r, l);
                            HostConstants c {};
                            c.Mode = 1;
                            c.Width = w;
                            c.Height = h;
                            c.WhitePoint = .4f;
                            c.ApplyModel = 1;
                            c.TransferStrength = c.ColourStrength = 1;
                            c.MaxRatio = 2;
                            c.Transfer = 1;
                            c.Passthrough = sdr;
                            c.ReversibleMode = sdr ? 0u : curve;
                            c.ModelWorkScale = scale;
                            c.GuideWidth = 1;
                            c.MvScaleX = .8f;
                            c.MvScaleY = .66f;
                            c.SkinDetail = c.SkinColour = c.EnvironmentDetail = c.EnvironmentColour = 1;
                            DlssNrConstants bytes {};
                            std::memcpy(&bytes, &c, 256);
                            const auto checkPath = [&](const std::string& candidate, const std::string& reference)
                            {
                                auto a = r.pipeline(dir, candidate), b = r.pipeline(refs, reference);
                                const UINT sw = packed ? l.modelW : l.ordinaryW, sh = packed ? l.modelH : l.ordinaryH;
                                const auto run = [&](ID3D12PipelineState* p)
                                {
                                    return r.run(p, w, h, w, h, bytes, 0, half, 8, sw, sh, nullptr, 3, nullptr, 0, 0, 8,
                                                 packed ? metadata.Get() : nullptr);
                                };
                                const auto actual = run(a.Get()), expected = run(b.Get());
                                if (actual != expected)
                                    throw std::runtime_error("Stage bypass differs from independent reference: " +
                                                             candidate);
                                ++cases;
                                words += actual.size();
                            };
                            const std::string suffix = (packed ? "packed_" : "") + std::string(sdr ? "sdr" : "hdr");
                            checkPath("host_skip_guided_" + suffix, "reference_skip_guided_" + suffix);
                            if (packed)
                                checkPath("host_resolve_" + suffix, "reference_full_" + suffix);
                        }
        auto raw = r.pipeline(dir, "host_residual_raw");
        auto rawRef = r.pipeline("artifacts/validation/native-center", "raw_residual_reference");
        bool largeResidual = false;
        for (auto size : { std::pair<UINT, UINT> { 1, 1 }, { 17, 9 }, { 65, 37 } })
            for (bool half : { false, true })
                for (int input = 0; input < 4; ++input)
                {
                    DlssNrConstants c {};
                    c.Mode = 40;
                    c.Width = size.first;
                    c.Height = size.second;
                    const auto a = r.run(raw.Get(), c.Width, c.Height, c.Width, c.Height, c, input, half);
                    const auto b = r.run(rawRef.Get(), c.Width, c.Height, c.Width, c.Height, c, input, half);
                    if (a != b)
                        throw std::runtime_error("Carrier bypass is not direct sanitized N-P");
                    if (!half)
                        for (size_t i = 0; i < a.size(); ++i)
                            if (i % 4 != 3)
                            {
                                float v;
                                std::memcpy(&v, &a[i], 4);
                                if (!std::isfinite(v))
                                    throw std::runtime_error("Nonfinite raw residual");
                                largeResidual |= std::abs(v) > 1.5f;
                            }
                    ++cases;
                    words += a.size();
                }
        if (!largeResidual)
            throw std::runtime_error("Missing out-of-carrier-range coverage");
        std::cout << "Superior stage GPU bypass PASS: " << cases << " cases / " << words
                  << " words; skip-guided, force-full-guided, raw carrier bypass, HDR/SDR, FP16/FP32\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
