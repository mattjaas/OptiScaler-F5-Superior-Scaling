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
        std::ofstream out("artifacts/validation/native-center-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        for (unsigned layout = 0; layout < 3; ++layout)
            for (int curve : { -1, 0, 2, 4, 6 })
                for (int scale : { 65, 100 })
                {
                    DlssNr::Spatial::Settings settings;
                    settings.enabled = true;
                    DlssNr::Spatial::ApplyNamedLayout(settings, layout);
                    PackedStages f(r, scale, curve, &settings);
                    auto& s = f.after;
                    auto cache = CachedWarp(r, f.layout);
                    const std::string suffix = curve < 0 ? "sdr" : "hdr" + std::to_string(curve);
                    auto base = r.pipeline("artifacts/validation/native-center", "host_cached_packed_" + suffix);
                    auto candidate = r.pipeline(
                        "OptiScaler/shaders/dlssnr/superior/precompile",
                        std::string(scale == 100 ? "host_native_cached_packed_" : "host_cached_packed_") + suffix);
                    HostConstants h {};
                    h.Mode = 1;
                    h.Width = 3840;
                    h.Height = 2160;
                    h.WhitePoint = 1;
                    h.TransferStrength = h.ColourStrength = 1;
                    h.MaxRatio = 2;
                    h.ApplyModel = 1;
                    h.Passthrough = curve < 0;
                    h.ReversibleMode = std::max(0, curve);
                    h.Transfer = 1;
                    h.ModelWorkScale = scale / 100.f;
                    h.GuideWidth = 1;
                    h.SkinDetail = h.SkinColour = h.EnvironmentDetail = h.EnvironmentColour = 1;
                    const auto scales = DlssNr::SuperiorScaling::MakeFrequencyScales(3840, 2160, f.layout.ordinaryW,
                                                                                     f.layout.ordinaryH, s.w, s.h);
                    const auto gains = DlssNr::SuperiorScaling::BandGains(0, 2, false, scales, 1, 1);
                    h.MvScaleX = gains.high;
                    h.MvScaleY = gains.low;
                    s.resolveCb = f.cb(h);
                    f.table(104, f.packedProxy.Get(), s.residual.Get(), f.images.inputs[2].Get(), cache.Get(),
                            s.low.Get(), f.images.native.Get());
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
                    auto draw = [&](ID3D12PipelineState* p)
                    {
                        f.dispatch(p, s.resolveCb.Get(), 104, 3840, 2160);
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = f.images.native.Get();
                        r.list->ResourceBarrier(1, &b);
                    };
                    if (scale != 100)
                    {
                        r.begin();
                        f.roots();
                        draw(base.Get());
                        r.finish();
                        const auto old = ReadTexture(r, f.images.native.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        r.begin();
                        f.roots();
                        draw(candidate.Get());
                        r.finish();
                        if (old != ReadTexture(r, f.images.native.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                            throw std::runtime_error("Below-native centre path changed");
                    }
                    const auto name = "layout" + std::to_string(layout) + "_" + suffix;
                    MeasureCandidate(
                        f.images, out, name.c_str(), scale, [&] { draw(base.Get()); }, [&] { draw(candidate.Get()); });
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
