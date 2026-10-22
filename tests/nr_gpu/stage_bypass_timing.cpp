#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "packed_stage_fixture.h"

int main()
{
    try
    {
        Runner r(true, 128);
        std::ofstream out("artifacts/validation/stage-bypass-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        for (int curve : { -1, 0 })
            for (int scale : { 65, 100 })
            {
                DlssNr::Spatial::Settings settings;
                settings.enabled = true;
                DlssNr::Spatial::ApplyNamedLayout(settings, 1);
                PackedStages f(r, scale, curve, &settings);
                auto& s = f.after;
                auto cache = CachedWarp(r, f.layout);
                const std::string suffix = curve < 0 ? "sdr" : "hdr0";
                auto base = r.pipeline(
                    dir, std::string(scale == 100 ? "host_native_cached_packed_" : "host_cached_packed_") + suffix);
                auto skip = r.pipeline(dir, std::string("host_skip_guided_packed_") + (curve < 0 ? "sdr" : "hdr"));
                HostConstants h {};
                h.Mode = 1;
                h.Width = 3840;
                h.Height = 2160;
                h.WhitePoint = 1;
                h.TransferStrength = h.ColourStrength = 1;
                h.MaxRatio = 2;
                h.ApplyModel = 1;
                h.Passthrough = curve < 0;
                h.ReversibleMode = std::max(curve, 0);
                h.Transfer = 1;
                h.ModelWorkScale = scale / 100.f;
                h.GuideWidth = 1;
                h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                const auto scales = DlssNr::SuperiorScaling::MakeFrequencyScales(3840, 2160, f.layout.ordinaryW,
                                                                                 f.layout.ordinaryH, s.w, s.h);
                const auto g = DlssNr::SuperiorScaling::BandGains(0, 2, false, scales, 1, 1);
                h.MvScaleX = g.high;
                h.MvScaleY = g.low;
                s.resolveCb = f.cb(h);
                f.table(104, f.packedProxy.Get(), s.residual.Get(), f.images.inputs[2].Get(), cache.Get(), s.low.Get(),
                        f.images.native.Get());
                r.begin();
                f.roots();
                f.dispatch(f.carrier.Get(), s.encodeCb.Get(), 72, s.w, s.h);
                r.barrier(s.carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                f.dispatch(f.carrier.Get(), s.decodeCb.Get(), 80, s.w, s.h);
                r.barrier(s.residual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                f.dispatch(f.low.Get(), s.lowCb.Get(), 88, (s.w + 7) / 8, (s.h + 7) / 8, 16);
                r.barrier(s.low.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                r.finish();
                const auto barrier = [&](ID3D12Resource* image)
                {
                    D3D12_RESOURCE_BARRIER b {};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    b.UAV.pResource = image;
                    r.list->ResourceBarrier(1, &b);
                };
                const auto draw = [&](ID3D12PipelineState* p)
                {
                    f.dispatch(p, s.resolveCb.Get(), 104, 3840, 2160);
                    barrier(f.images.native.Get());
                };
                const auto name = "skip-guided-balanced-" + suffix;
                MeasureCandidate(
                    f.images, out, name.c_str(), scale, [&] { draw(base.Get()); }, [&] { draw(skip.Get()); });
                if (curve == 0)
                {
                    DlssNrConstants c {};
                    c.Mode = 37;
                    c.Width = s.w;
                    c.Height = s.h;
                    auto encode = f.cb(c);
                    c.Mode = 40;
                    auto rawCb = f.cb(c);
                    auto raw = r.pipeline(dir, "host_residual_raw");
                    f.table(112, f.packedProxy.Get(), f.packedAnswer.Get(), nullptr, nullptr, nullptr,
                            s.residual.Get());
                    r.begin();
                    r.barrier(s.carrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    r.barrier(s.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    r.finish();
                    const auto roundtrip = [&]
                    {
                        f.dispatch(f.carrier.Get(), encode.Get(), 72, s.w, s.h);
                        r.barrier(s.carrier.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        f.dispatch(f.carrier.Get(), s.decodeCb.Get(), 80, s.w, s.h);
                        barrier(s.residual.Get());
                        r.barrier(s.carrier.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    };
                    const auto direct = [&]
                    {
                        f.dispatch(raw.Get(), rawCb.Get(), 112, s.w, s.h);
                        barrier(s.residual.Get());
                    };
                    MeasureCandidate(f.images, out, "skip-carrier-balanced", scale, roundtrip, direct);
                }
            }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
