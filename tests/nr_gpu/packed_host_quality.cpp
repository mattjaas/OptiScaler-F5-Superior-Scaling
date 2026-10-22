#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "superior_input_fixture.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

int main()
{
    try
    {
        Runner r(true);
        const std::string candidate = "OptiScaler/shaders/dlssnr/superior/precompile";
        const std::string reference = "artifacts/validation/host-shaders";
        auto hdr = r.pipeline(candidate, "host_resolve_packed_hdr"),
             sdr = r.pipeline(candidate, "host_resolve_packed_sdr");
        auto nativeHdr = r.pipeline(candidate, "host_native_packed_hdr"),
             nativeSdr = r.pipeline(candidate, "host_native_packed_sdr");
        std::array<ComPtr<ID3D12PipelineState>, 5> cached;
        unsigned profile = 0;
        for (const char* name : { "sdr", "hdr0", "hdr2", "hdr4", "hdr6" })
            cached[profile++] = r.pipeline(candidate, std::string("host_native_cached_packed_") + name);
        auto rhdr = r.pipeline(reference, "reference_packed_hdr"), rsdr = r.pipeline(reference, "reference_packed_sdr");
        unsigned cases = 0;
        size_t components = 0;
        double maxError = 0;
        double maxFloatError = 0;
        unsigned maxHalfUlps = 0;
        for (auto dims : { std::pair<UINT, UINT> { 193, 129 }, { 385, 217 } })
            for (float scale : { .5f, .65f, 1.f })
                for (int preset = 0; preset < 6; ++preset)
                {
                    DlssNr::Spatial::Settings settings;
                    settings.enabled = true;
                    if (preset < 3)
                        DlssNr::Spatial::ApplyNamedLayout(settings, preset);
                    else if (preset == 3)
                    {
                        settings.centerX = 65;
                        settings.workX = 85;
                        settings.centerY = 70;
                        settings.workY = 95;
                        settings.offsetX = 7;
                        settings.offsetY = -5;
                        settings.shiftX = -2;
                        settings.shiftY = 1;
                    }
                    else if (preset == 4)
                        settings.workY = 100;
                    else
                        settings.workX = 100;
                    const auto layout = DlssNr::Spatial::Build(settings, dims.first, dims.second, scale, false);
                    if (!layout.active)
                        throw std::runtime_error("Inactive fixture layout: " + std::to_string(dims.first) + " scale " +
                                                 std::to_string(scale) + " preset " + std::to_string(preset) +
                                                 " status " + std::to_string(int(layout.status)));
                    auto warp = UploadWarp(r, layout);
                    auto cachedWarp = scale == 1.f ? CachedWarp(r, layout) : ComPtr<ID3D12Resource> {};
                    for (bool half : { false, true })
                        for (unsigned curve = 0; curve < 8; ++curve)
                            for (unsigned scenario = 0; scenario < 8; ++scenario)
                            {
                                HostConstants h {};
                                h.Mode = 1;
                                h.Width = dims.first;
                                h.Height = dims.second;
                                h.WhitePoint = .4f;
                                h.ApplyModel = scenario == 4 ? 0u : 1u;
                                h.TransferStrength = 1;
                                h.ColourStrength = 1;
                                h.MaxRatio = 2;
                                h.Transfer = 1;
                                h.ReversibleMode = curve;
                                h.PreExposure = 1.7f;
                                h.ExposureTrim = 1.2f;
                                h.DebugView = scenario >= 4 ? scenario - 4 : 0;
                                h.InputEncoding = scenario == 4 ? 3 : scenario == 5 ? 4 : 0;
                                h.Passthrough = scenario == 0 || (scenario >= 4 && curve % 2 == 0);
                                h.OriginalIsGameColour = 1;
                                h.UseGameExposure = scenario == 1;
                                h.UseExposureWhitePoint = scenario == 2;
                                h.ExposureTrimAnchorCount = scenario == 2 ? 2 : 0;
                                h.ExposureTrimAnchorExposure0 = .1f;
                                h.ExposureTrimAnchorTrim0 = .8f;
                                h.ExposureTrimAnchorExposure1 = 10;
                                h.ExposureTrimAnchorTrim1 = 1.6f;
                                h.CompareMode = scenario == 3 ? 1 : 0;
                                h.CompareZoom = 2;
                                h.CompareSplit = .37f;
                                h.SkinProtection = scenario == 3;
                                h.SkinDetail = .3f;
                                h.SkinColour = .4f;
                                h.EnvironmentDetail = h.EnvironmentColour = 1;
                                h.ModelWorkScale = scale;
                                h.GuideWidth = 1;
                                const auto scales = DlssNr::SuperiorScaling::MakeFrequencyScales(
                                    dims.first, dims.second, layout.ordinaryW, layout.ordinaryH, layout.modelW,
                                    layout.modelH);
                                h.MvScaleX = DlssNr::SuperiorScaling::EffectiveBand(.8f, scales.high, 1, 1);
                                h.MvScaleY = DlssNr::SuperiorScaling::EffectiveBand(.661f, scales.low, 1, 1);
                                DlssNrConstants bytes {};
                                std::memcpy(&bytes, &h, 256);
                                auto run = [&](ID3D12PipelineState* pso, bool useCache = false)
                                {
                                    return r.run(pso, dims.first, dims.second, dims.first, dims.second, bytes, scenario,
                                                 half, 8, layout.modelW, layout.modelH, nullptr, 3, nullptr, 0, 0, 8,
                                                 useCache ? cachedWarp.Get() : warp.Get(),
                                                 half ? InputFixture::halfBits : nullptr);
                                };
                                const auto a = run(h.Passthrough ? rsdr.Get() : rhdr.Get());
                                std::vector<std::pair<ID3D12PipelineState*, bool>> paths {
                                    { scale == 1.f ? (h.Passthrough ? nativeSdr.Get() : nativeHdr.Get())
                                                   : (h.Passthrough ? sdr.Get() : hdr.Get()),
                                      false }
                                };
                                const int normal = DlssNr::SuperiorScaling::NormalResolveVariant(
                                    false, true, h.Passthrough, h.ReversibleMode, h.DebugView, h.CompareMode,
                                    h.ApplyModel);
                                if (scale == 1.f && normal >= 5)
                                    paths.push_back({ cached[normal - 5].Get(), true });
                                for (auto path : paths)
                                {
                                    const auto b = run(path.first, path.second);
                                    for (size_t i = 0; i < a.size(); ++i)
                                        for (unsigned part = 0; part < (half ? 2u : 1u); ++part)
                                        {
                                            const float x =
                                                half ? TestHalf((a[i] >> (part * 16)) & 65535) : TestFloat(a[i]);
                                            const float y =
                                                half ? TestHalf((b[i] >> (part * 16)) & 65535) : TestFloat(b[i]);
                                            if (!std::isfinite(x) || !std::isfinite(y))
                                            {
                                                if (a[i] != b[i])
                                                    throw std::runtime_error("Nonfinite mismatch");
                                                continue;
                                            }
                                            const double error = std::abs(double(x) - y);
                                            maxError = std::max(maxError, error);
                                            if (!half)
                                                maxFloatError = std::max(maxFloatError, error);
                                            // The native identity branch intentionally changes the estimator; splitting
                                            // shaping from the independent reference can also change final FP rounding.
                                            const bool nativeEdited =
                                                scale == 1.f && h.DebugView == 0 && h.ApplyModel != 0;
                                            const double tolerance =
                                                nativeEdited ? (half ? std::max(0.0000611, std::abs(double(x)) * .001)
                                                                     : 2e-6 * std::max(1.0, std::abs(double(x))))
                                                             : 0;
                                            if (half && nativeEdited)
                                            {
                                                const auto ordered = [](unsigned bits)
                                                { return bits & 32768 ? 65535 - bits : bits + 32768; };
                                                const auto ax = ordered((a[i] >> (part * 16)) & 65535),
                                                           bx = ordered((b[i] >> (part * 16)) & 65535);
                                                const auto ulps = ax > bx ? ax - bx : bx - ax;
                                                maxHalfUlps = std::max(maxHalfUlps, ulps);
                                                // Near black, tiny FP32 cancellation differences can span several
                                                // half subnormal ULPs. The absolute/relative bound below is
                                                // authoritative.
                                            }
                                            if (error > tolerance || (tolerance == 0 && a[i] != b[i]))
                                                throw std::runtime_error(
                                                    "Packed guided mismatch: " + std::to_string(error) + " curve " +
                                                    std::to_string(curve) + " size " + std::to_string(dims.first) +
                                                    " scale " + std::to_string(scale) + " layout " +
                                                    std::to_string(preset) + " half " + std::to_string(half) +
                                                    " scenario " + std::to_string(scenario) + " word " +
                                                    std::to_string(i) + " values " + std::to_string(x) + " / " +
                                                    std::to_string(y));
                                            ++components;
                                        }
                                    ++cases;
                                }
                            }
                }
        std::cout << "Packed guided host reference PASS: " << cases << " cases, " << components
                  << " components, max absolute error " << maxError << ", FP32 max " << maxFloatError
                  << ", max FP16 ULPs " << maxHalfUlps << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
