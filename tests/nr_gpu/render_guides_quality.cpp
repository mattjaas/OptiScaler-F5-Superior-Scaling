#include "pch.h"
#define main LegacySpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/DlssNr_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_spatial_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/dlssnr_superior_carrier_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_joint_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_joint_guides_Shader.h"

int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    const void* blobs[] { dlssnr_spatial_guides_cso,
                          host_spatial_guides_cso,
                          host_surface_guides_cso,
                          host_joint_guides_cso,
                          DlssNr_cso,
                          host_uniform_guides_cso,
                          host_uniform_surface_guides_cso,
                          host_uniform_joint_guides_cso,
                          dlssnr_superior_carrier_cso };
    const size_t sizes[] { sizeof(dlssnr_spatial_guides_cso),
                           sizeof(host_spatial_guides_cso),
                           sizeof(host_surface_guides_cso),
                           sizeof(host_joint_guides_cso),
                           sizeof(DlssNr_cso),
                           sizeof(host_uniform_guides_cso),
                           sizeof(host_uniform_surface_guides_cso),
                           sizeof(host_uniform_joint_guides_cso),
                           sizeof(dlssnr_superior_carrier_cso) };
    ComPtr<ID3D12PipelineState> pso[9];
    for (unsigned i = 0; i < 9; ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        d.pRootSignature = gpu.rootSignature.Get();
        d.CS = { blobs[i], sizes[i] };
        CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso[i]))));
    }
    size_t nrPixels = 0, privateComponents = 0;
    for (bool custom : { false, true })
        for (bool reversed : { false, true })
            for (unsigned method = 0; method < 4; ++method)
                for (UINT renderW : { 160u, 213u })
                {
                    constexpr UINT W = 320, H = 192;
                    const UINT renderH = renderW == 160 ? 96 : 127;
                    Sp::Settings settings;
                    settings.enabled = true;
                    if (custom)
                    {
                        settings.offsetX = 2;
                        settings.shiftY = -1;
                    }
                    const auto colour = Sp::Build(settings, W, H, 1.f, false);
                    const auto layout = Sp::RenderGuideLayout(colour, { renderW, renderH });
                    const UINT gw = layout.modelW, gh = layout.modelH;
                    // Nonzero source origins verify that valid render subrects, not allocations, are used.
                    constexpr UINT bx = 3, by = 5;
                    const UINT aw = renderW + 7, ah = renderH + 9;
                    std::vector<float> depth(size_t(aw) * ah, 12345), mv(size_t(aw) * ah * 2, 12345);
                    for (UINT y = 0; y < renderH; ++y)
                        for (UINT x = 0; x < renderW; ++x)
                        {
                            const auto i = size_t(y + by) * aw + x + bx;
                            depth[i] = reversed ? .7f : .3f;
                            mv[i * 2] = -2.f;
                            mv[i * 2 + 1] = 1.f;
                        }
                    auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, aw, ah, 4, depth.data());
                    auto mt = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, mv.data());
                    const DlssNr::GuideRegions regions { { bx, by, renderW, renderH }, { bx, by, renderW, renderH } };
                    auto c = Sp::MakeGuideConstants(layout, regions, { renderW, renderH }, -1, 1);
                    c.padding[0] = 1;
                    c.unused = reversed ? 1.f : 0.f;
                    auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, gw, gh, true);
                    auto om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, gw, gh, true);
                    gpu.guides = pso[method];
                    gpu.Dispatch(c, dt.Get(), dt.Get(), mt.Get(), od.Get(), om.Get());
                    const auto dz = gpu.Read(od.Get(), 4), vectors = gpu.Read(om.Get(), 8);
                    const auto* z = reinterpret_cast<const float*>(dz.data());
                    const auto* v = reinterpret_cast<const float*>(vectors.data());
                    for (UINT y = 0; y < gh; ++y)
                        for (UINT x = 0; x < gw; ++x)
                        {
                            const auto i = size_t(y) * gw + x;
                            const float nx = pw::Unpack(x + .5f, layout.warp.x),
                                        ny = pw::Unpack(y + .5f, layout.warp.y);
                            const float ex =
                                pw::Pack(nx - 2.f * c.motionScale[0], layout.warp.x) - pw::Pack(nx, layout.warp.x);
                            const float ey =
                                pw::Pack(ny + c.motionScale[1], layout.warp.y) - pw::Pack(ny, layout.warp.y);
                            CHECK(std::abs(v[i * 2] - ex) < .0003f && std::abs(v[i * 2 + 1] - ey) < .0003f);
                            CHECK(z[i] == (reversed ? .7f : .3f));
                            ++nrPixels;
                        }
                    // Mirror private DLAA's uniform reconstruction of the packed NR guides.
                    auto sd = gpu.Upload(DXGI_FORMAT_R32_FLOAT, gw, gh, 4, dz.data());
                    auto sm = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, gw, gh, 8, vectors.data());
                    Sp::Layout uniform {};
                    uniform.nativeW = W;
                    uniform.nativeH = H;
                    uniform.modelW = colour.modelW;
                    uniform.modelH = colour.modelH;
                    uniform.warp.x = pw::BuildAxis(W, W, colour.modelW, 1, 0, 0);
                    uniform.warp.y = pw::BuildAxis(H, H, colour.modelH, 1, 0, 0);
                    const DlssNr::GuideRegions packed { { 0, 0, gw, gh }, { 0, 0, gw, gh } };
                    c = Sp::MakeGuideConstants(uniform, packed, { gw, gh }, 1, 1);
                    c.mode = 104;
                    c.padding[0] = 1;
                    if (method == 0)
                    {
                        HostConstants point {};
                        point.Mode = 10;
                        point.Width = colour.modelW;
                        point.Height = colour.modelH;
                        point.GuideWidth = gw;
                        point.GuideHeight = gh;
                        point.TransferStrength = float(gw);
                        point.ColourStrength = float(gh);
                        point.MvScaleX = point.MvScaleY = 1;
                        std::memcpy(&c, &point, 256);
                    }
                    auto pd = gpu.Texture(DXGI_FORMAT_R32_FLOAT, colour.modelW, colour.modelH, true);
                    auto pm = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, colour.modelW, colour.modelH, true);
                    gpu.colour = pso[method + 4];
                    gpu.Dispatch(c, sd.Get(), method ? sd.Get() : sm.Get(), sm.Get(), pd.Get(), pm.Get());
                    const auto raw = gpu.Read(pm.Get(), 8), privateDepth = gpu.Read(pd.Get(), 4);
                    const auto* pz = reinterpret_cast<const float*>(privateDepth.data());
                    for (size_t i = 0; i < size_t(colour.modelW) * colour.modelH; ++i)
                        CHECK(pz[i] == (reversed ? .7f : .3f));
                    auto shared = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, colour.modelW, colour.modelH, 8, raw.data());
                    std::vector<uint16_t> neutral(size_t(colour.modelW) * colour.modelH * 4, ToHalf(.5f));
                    auto proxy =
                        gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, colour.modelW, colour.modelH, 8, neutral.data());
                    for (bool hold : { false, true })
                    {
                        HostConstants carrier {};
                        carrier.Mode = 39;
                        carrier.Width = colour.modelW;
                        carrier.Height = colour.modelH;
                        carrier.TransferStrength = float(colour.modelW);
                        carrier.ColourStrength = float(colour.modelH);
                        carrier.MvScaleX = hold ? 0 : float(colour.modelW) / gw;
                        carrier.MvScaleY = hold ? 0 : float(colour.modelH) / gh;
                        std::memcpy(&c, &carrier, 256);
                        auto oc = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, colour.modelW, colour.modelH, true);
                        auto ov = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, colour.modelW, colour.modelH, true);
                        gpu.colour = pso[8];
                        gpu.Dispatch(c, proxy.Get(), proxy.Get(), shared.Get(), oc.Get(), ov.Get());
                        const auto result = gpu.Read(ov.Get(), 8);
                        const auto* actual = reinterpret_cast<const float*>(result.data());
                        const auto* expected = reinterpret_cast<const float*>(raw.data());
                        for (size_t i = 0; i < size_t(colour.modelW) * colour.modelH * 2; ++i)
                            CHECK(std::abs(actual[i] - expected[i] * (i % 2 ? carrier.MvScaleY : carrier.MvScaleX)) <
                                  .000001f);
                        privateComponents += size_t(colour.modelW) * colour.modelH * 2;
                    }
                }
    std::printf(
        "Render-resolution guides PASS: %zu NR pixels, %zu private MV components; point/bilinear/surface/joint, "
        "odd render sizes, offset/shift, source subrects, reversed Z, private unit conversion and hold\n",
        nrPixels, privateComponents);
    return fails ? 1 : 0;
}
