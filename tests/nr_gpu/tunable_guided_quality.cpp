#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

namespace Sp = DlssNr::Spatial;
int main()
{
    try
    {
        Runner r(true);
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        unsigned cases = 0;
        size_t components = 0, changed = 0;
        double maximum = 0;
        for (bool packed : { false, true })
            for (bool native : { false, true })
                for (bool cache : { false, true })
                    for (bool half : { false, true })
                        for (unsigned curve : { 0u, 2u, 4u, 6u })
                            for (unsigned scenario = 0; scenario < 4; ++scenario)
                            {
                                if ((!packed && (native || cache)))
                                    continue;
                                const UINT W = 129, H = 97;
                                const float scale = native ? 1.f : .65f;
                                Sp::Settings settings;
                                settings.enabled = true;
                                const auto layout = Sp::Build(settings, W, H, scale, false);
                                auto warp = cache ? CachedWarp(r, layout) : UploadWarp(r, layout);
                                const bool sdr = scenario == 0;
                                std::string suffix = (native ? "native_" : "") + std::string(cache ? "cached_" : "") +
                                                     (packed ? "packed_" : "") + (sdr ? "sdr" : "hdr");
                                auto dynamic = r.pipeline(dir, "host_tunable_" + suffix);
                                const std::string legacy = native ? "host_" + suffix
                                                           : cache
                                                               ? std::string("host_cached_packed_") +
                                                                     (sdr ? "sdr" : "hdr" + std::to_string(curve))
                                                               : std::string("host_resolve_") +
                                                                     (packed ? "packed_" : "") + (sdr ? "sdr" : "hdr");
                                auto referenceName = legacy;
                                if (native && cache && !sdr)
                                    referenceName += std::to_string(curve);
                                if (sdr && curve != 0)
                                    referenceName = native   ? "host_native_packed_sdr"
                                                    : packed ? "host_resolve_packed_sdr"
                                                             : "host_resolve_sdr";
                                auto original = r.pipeline(dir, referenceName);
                                HostConstants h {};
                                h.Mode = 1;
                                h.Width = W;
                                h.Height = H;
                                h.WhitePoint = .4f;
                                h.ApplyModel = 1;
                                h.TransferStrength = h.ColourStrength = 1;
                                h.MaxRatio = 2;
                                h.Transfer = 11;
                                h.Passthrough = sdr;
                                h.ReversibleMode = curve;
                                h.PreExposure = 1.7f;
                                h.ExposureTrim = 1.2f;
                                h.ExposureBaseScale = 1.1f;
                                h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                                h.ModelWorkScale = scale;
                                h.GuideWidth = 0;
                                h.GuideHeight = 0;
                                h.MvScaleX = h.MvScaleY = 1;
                                h.UseGameExposure = scenario == 1;
                                h.UseExposureWhitePoint = scenario == 2;
                                h.InputEncoding = scenario == 3 ? 4u : 0u;
                                h.OriginalIsGameColour = 1;
                                auto run = [&](ID3D12PipelineState* pso, const HostConstants& c)
                                {
                                    DlssNrConstants bytes {};
                                    std::memcpy(&bytes, &c, 256);
                                    return r.run(pso, W, H, W, H, bytes, scenario, half, 8, layout.modelW,
                                                 layout.modelH, nullptr, 3, nullptr, 0, 0, 8,
                                                 packed ? warp.Get() : nullptr);
                                };
                                const auto a = run(original.Get(), h);
                                DlssNr::SuperiorScaling::SetGuided(h, .015f, 3.f);
                                const auto b = run(dynamic.Get(), h);
                                DlssNr::SuperiorScaling::SetGuided(h, .05f, .75f);
                                const auto c = run(dynamic.Get(), h);
                                for (size_t i = 0; i < a.size(); ++i)
                                    for (unsigned part = 0; part < (half ? 2u : 1u); ++part)
                                    {
                                        const float x =
                                            half ? TestHalf((a[i] >> (part * 16)) & 65535) : TestFloat(a[i]);
                                        const float y =
                                            half ? TestHalf((b[i] >> (part * 16)) & 65535) : TestFloat(b[i]);
                                        const float z =
                                            half ? TestHalf((c[i] >> (part * 16)) & 65535) : TestFloat(c[i]);
                                        if (!std::isfinite(x) || !std::isfinite(y))
                                        {
                                            if (a[i] != b[i])
                                                throw std::runtime_error("Default nonfinite mismatch");
                                            continue;
                                        }
                                        const double error = std::abs(double(x) - y);
                                        maximum = std::max(maximum, error);
                                        if (error > (half ? std::max(.0000611, std::abs(double(x)) * .001)
                                                          : 2e-5 * std::max(1.0, std::abs(double(x)))))
                                            throw std::runtime_error(
                                                "Tunable defaults differ: " + referenceName + " / " + suffix +
                                                " curve " + std::to_string(curve) + " scenario " +
                                                std::to_string(scenario) + " half " + std::to_string(half) +
                                                " values " + std::to_string(x) + " / " + std::to_string(y) + " error " +
                                                std::to_string(error));
                                        if (std::isfinite(z) && std::abs(z - y) > 1e-5f)
                                            ++changed;
                                        ++components;
                                    }
                                ++cases;
                            }
        if (changed < 100)
            throw std::runtime_error("Sigma parameters did not affect reconstruction");
        std::cout << "Tunable guided: " << cases << " cases / " << components << " components; default max " << maximum
                  << "; custom-changed " << changed
                  << "; SDR/HDR, game/automatic exposure, cached/direct/native, FP16/FP32\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
