#include "pch.h"
#include <limits>
#include <DirectXPackedVector.h>
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

int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    ComPtr<ID3D12PipelineState> carrierPso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC carrierDesc {};
    carrierDesc.pRootSignature = gpu.rootSignature.Get();
    carrierDesc.CS = { dlssnr_superior_carrier_cso, sizeof(dlssnr_superior_carrier_cso) };
    CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&carrierDesc, IID_PPV_ARGS(&carrierPso))));
    if (!carrierPso)
        return 2;
    std::vector<uint16_t> neutral(8 * 8 * 4, DirectX::PackedVector::XMConvertFloatToHalf(.5f));
    auto proxy = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 8, 8, neutral.data());
    size_t privateComponents = 0;
    ComPtr<ID3D12PipelineState> pso[2][3];
    pso[1][0] = gpu.guides;
    const void* bytes[5] { DlssNr_cso, host_uniform_guides_cso, host_uniform_surface_guides_cso,
                           host_spatial_guides_cso, host_surface_guides_cso };
    const size_t sizes[5] { sizeof(DlssNr_cso), sizeof(host_uniform_guides_cso),
                            sizeof(host_uniform_surface_guides_cso), sizeof(host_spatial_guides_cso),
                            sizeof(host_surface_guides_cso) };
    for (unsigned i = 0; i < 5; ++i)
    {
        auto& out = i < 3 ? pso[0][i] : pso[1][i - 2];
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        d.pRootSignature = gpu.rootSignature.Get();
        d.CS = { bytes[i], sizes[i] };
        CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out))));
        if (!out)
            return 2;
    }
    unsigned cases = 0;
    size_t pixels = 0, blendedOld = 0, blendedNew = 0, revealed = 0;
    double revealError[3] {};
    double error[5][3] {};
    size_t count[5] {};
    for (auto dim : { std::pair<UINT, UINT> { 320, 192 }, { 321, 193 } })
        for (float scale : { .65f, 1.f, 1.25f })
            for (bool packed : { false, true })
                for (bool half : { false, true })
                    for (bool reversed : { false, true })
                        for (unsigned scene = 0; scene < 12; ++scene)
                        {
                            const UINT W = dim.first, H = dim.second, mw = scene == 7 ? W : W / 2,
                                       mh = scene == 7 ? H : H / 2;
                            const UINT mx = 5, my = 3, aw = mw + 17, ah = mh + 9, dx = 3, dy = 7, dw = W + 11,
                                       dh = H + 13;
                            Sp::Settings settings;
                            settings.enabled = packed;
                            settings.offsetX = 2;
                            settings.shiftY = -1;
                            auto layout = Sp::Build(settings, W, H, scale, false);
                            if (!packed)
                            {
                                layout.modelW = layout.ordinaryW;
                                layout.modelH = layout.ordinaryH;
                                layout.warp.x = pw::BuildAxis(W, W, layout.modelW, 1, 0, 0);
                                layout.warp.y = pw::BuildAxis(H, H, layout.modelH, 1, 0, 0);
                            }
                            const UINT ow = layout.modelW, oh = layout.modelH;
                            DlssNr::GuideRegions regions { { dx, dy, W, H }, { mx, my, mw, mh } };
                            auto cb = Sp::MakeGuideConstants(layout, regions, { mw, mh }, -1.f, 1.f);
                            cb.mode = packed ? 101u : 104u;
                            cb.padding[0] = scene == 6 ? 0.f : 1.f;
                            auto foreground = [&](float x, float y)
                            {
                                if (scene >= 10)
                                    return x >= W * .5f + .3f + (scene - 10) * 4 &&
                                           x < W * .5f + 9.7f + (scene - 10) * 4;
                                if (scene == 4)
                                    return x >= W * .5f + .3f && x < W * .5f + 3.7f;
                                if (scene == 5)
                                    return x > W * .3f + .7f * y + .3f;
                                return x > W * .5f + .3f;
                            };
                            auto velocity = [&](float x, float y)
                            {
                                if (scene == 0 || scene == 7)
                                    return std::pair<float, float> { 3.5f, -1.75f };
                                if (scene == 1)
                                    return std::pair<float, float> { 2 + x * .75f - y * .375f,
                                                                     -.5f + x * .375f + y * .75f };
                                if (scene == 8)
                                    return std::pair<float, float> { 2 + x / 16.f - y / 32.f,
                                                                     -.5f + x / 32.f + y / 16.f };
                                if (scene == 9)
                                    return std::pair<float, float> { 2 + 2 * std::sin(x * .04f),
                                                                     -1 + 1.5f * std::cos(y * .05f) };
                                if (scene == 2 || scene == 6)
                                    return std::pair<float, float> { x >= W * .5f ? 4.f : 3.5f, -2.f };
                                return std::pair<float, float> { foreground(x, y) ? (scene >= 10 ? -4.f : 12.f) : 0.f,
                                                                 0.f };
                            };
                            auto deviceDepth = [&](float x, float y)
                            {
                                float z = scene == 1 || scene == 8                    ? .2f + x * .0001f + y * .0002f
                                          : (scene >= 3 && scene <= 5) || scene >= 10 ? (foreground(x, y) ? .6f : .2f)
                                                                                      : .3f;
                                return reversed ? 1.f - z : z;
                            };
                            std::vector<float> motion(size_t(aw) * ah * 2, 12345), depth(size_t(dw) * dh, 12345);
                            for (UINT y = 0; y < mh; ++y)
                                for (UINT x = 0; x < mw; ++x)
                                {
                                    auto v = velocity((x + .5f) * W / mw, (y + .5f) * H / mh);
                                    const auto i = (size_t(y + my) * aw + x + mx) * 2;
                                    motion[i] = v.first / cb.motionScale[0];
                                    motion[i + 1] = v.second / cb.motionScale[1];
                                }
                            for (UINT y = 0; y < H; ++y)
                                for (UINT x = 0; x < W; ++x)
                                    depth[size_t(y + dy) * dw + x + dx] = deviceDepth(x + .5f, y + .5f);
                            if (scene == 7)
                                motion[(size_t(my + mh / 2) * aw + mx + mw / 2) * 2] =
                                    std::numeric_limits<float>::quiet_NaN();
                            std::vector<uint16_t> hmotion(motion.size());
                            for (size_t i = 0; i < motion.size(); ++i)
                                hmotion[i] = DirectX::PackedVector::XMConvertFloatToHalf(motion[i]);
                            auto mt = half ? gpu.Upload(DXGI_FORMAT_R16G16_FLOAT, aw, ah, 4, hmotion.data())
                                           : gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, motion.data());
                            auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, dw, dh, 4, depth.data());
                            std::vector<uint8_t> output[3], outDepth[3];
                            for (unsigned method = 0; method < 3; ++method)
                            {
                                auto c = cb;
                                if (!packed && method == 0)
                                {
                                    HostConstants raw {};
                                    raw.Mode = 10;
                                    raw.Width = ow;
                                    raw.Height = oh;
                                    raw.GuideWidth = W;
                                    raw.GuideHeight = H;
                                    raw.DebugView = dx;
                                    raw.CompareMode = dy;
                                    raw.TransferStrength = float(mw);
                                    raw.ColourStrength = float(mh);
                                    raw.CompareSwap = mx;
                                    raw.Transfer = my;
                                    raw.MvScaleX = raw.MvScaleY = 1;
                                    std::memcpy(&c, &raw, 256);
                                }
                                auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, ow, oh, true),
                                     om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, ow, oh, true);
                                gpu.guides = pso[packed][method];
                                gpu.colour = pso[packed][method];
                                gpu.Dispatch(c, dt.Get(), !packed && method == 0 ? mt.Get() : dt.Get(), mt.Get(),
                                             od.Get(), om.Get());
                                output[method] = gpu.Read(om.Get(), 8);
                                outDepth[method] = gpu.Read(od.Get(), 4);
                            }
                            if (!packed)
                            {
                                // Private SR consumes the selected raw NR field, with one unit conversion.
                                auto shared = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, ow, oh, 8, output[2].data());
                                for (bool hold : { false, true })
                                {
                                    HostConstants c {};
                                    c.Mode = 39;
                                    c.Width = ow;
                                    c.Height = oh;
                                    c.TransferStrength = float(ow);
                                    c.ColourStrength = float(oh);
                                    c.MvScaleX = hold ? 0 : cb.motionScale[0] * ow / W;
                                    c.MvScaleY = hold ? 0 : cb.motionScale[1] * oh / H;
                                    Sp::Constants storage {};
                                    std::memcpy(&storage, &c, 256);
                                    auto oc = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ow, oh, true);
                                    auto om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, ow, oh, true);
                                    gpu.colour = carrierPso;
                                    gpu.Dispatch(storage, proxy.Get(), proxy.Get(), shared.Get(), oc.Get(), om.Get());
                                    const auto actual = gpu.Read(om.Get(), 8);
                                    const auto* got = reinterpret_cast<const float*>(actual.data());
                                    const auto* raw = reinterpret_cast<const float*>(output[2].data());
                                    for (size_t i = 0; i < size_t(ow) * oh * 2; ++i)
                                    {
                                        const float expected = raw[i] * (i % 2 ? c.MvScaleY : c.MvScaleX);
                                        CHECK(std::abs(got[i] - expected) < 1e-6f * std::max(1.f, std::abs(expected)));
                                    }
                                    privateComponents += size_t(ow) * oh * 2;
                                }
                            }
                            for (UINT y = 0; y < oh; ++y)
                                for (UINT x = 0; x < ow; ++x)
                                {
                                    const auto i = (size_t(y) * ow + x) * 2;
                                    const float nx = packed ? pw::Unpack(x + .5f, layout.warp.x) : (x + .5f) * W / ow;
                                    const float ny = packed ? pw::Unpack(y + .5f, layout.warp.y) : (y + .5f) * H / oh;
                                    const int sx = std::clamp(int(nx * mw / W), 0, int(mw) - 1),
                                              sy = std::clamp(int(ny * mh / H), 0, int(mh) - 1);
                                    const float* field[3];
                                    float value[3][2];
                                    for (unsigned m = 0; m < 3; ++m)
                                    {
                                        field[m] = reinterpret_cast<const float*>(output[m].data());
                                        const float scaleX = packed ? 1.f : cb.motionScale[0] * ow / W;
                                        const float scaleY = packed ? 1.f : cb.motionScale[1] * oh / H;
                                        value[m][0] = field[m][i] * scaleX;
                                        value[m][1] = field[m][i + 1] * scaleY;
                                    }
                                    CHECK(std::isfinite(value[2][0]) && std::isfinite(value[2][1]));
                                    // No invention at a subpixel step, even where its magnitude is below the old guard.
                                    if (scene == 2 || scene == 6)
                                    {
                                        const auto v = velocity((sx + .5f) * W / mw, (sy + .5f) * H / mh);
                                        const float expected =
                                            packed ? pw::Pack(nx + v.first, layout.warp.x) - pw::Pack(nx, layout.warp.x)
                                                   : v.first * ow / W;
                                        if (std::abs(value[1][0] - expected) > .02f)
                                            ++blendedOld;
                                        if (std::abs(value[2][0] - expected) > .02f)
                                            ++blendedNew;
                                    }
                                    if (scene == 7)
                                        continue;
                                    const bool interior = nx > 12 && ny > 12 && nx < W - 12 && ny < H - 12;
                                    if (!interior)
                                        continue;
                                    unsigned group = scene == 1                                  ? 0
                                                     : (scene >= 3 && scene <= 5) || scene >= 10 ? 1
                                                     : scene == 8                                ? 3
                                                     : scene == 9                                ? 4
                                                                                                 : 2;
                                    auto truth = velocity(std::clamp(nx, .5f * W / mw, (mw - .5f) * W / mw),
                                                          std::clamp(ny, .5f * H / mh, (mh - .5f) * H / mh));
                                    if (group == 1)
                                        truth = velocity(std::floor(nx) + .5f, std::floor(ny) + .5f);
                                    if (scene == 2 || scene == 6)
                                        truth = velocity((sx + .5f) * W / mw, (sy + .5f) * H / mh);
                                    const float ex =
                                        packed ? pw::Pack(nx + truth.first, layout.warp.x) - pw::Pack(nx, layout.warp.x)
                                               : truth.first * ow / W;
                                    const float ey = packed ? pw::Pack(ny + truth.second, layout.warp.y) -
                                                                  pw::Pack(ny, layout.warp.y)
                                                            : truth.second * oh / H;
                                    for (unsigned m = 0; m < 3; ++m)
                                        error[group][m] +=
                                            std::pow(value[m][0] - ex, 2) + std::pow(value[m][1] - ey, 2);
                                    if (scene == 11 && std::floor(nx) + .5f >= W * .5f + .3f &&
                                        std::floor(nx) + .5f < W * .5f + 4.3f)
                                    {
                                        ++revealed;
                                        for (unsigned m = 0; m < 3; ++m)
                                            revealError[m] +=
                                                std::pow(value[m][0] - ex, 2) + std::pow(value[m][1] - ey, 2);
                                    }
                                    ++count[group];
                                }
                            pixels += size_t(ow) * oh;
                            ++cases;
                        }
    CHECK(blendedOld > 100 && blendedNew == 0);
    CHECK(error[0][2] < error[0][0] * .1);  // Steep affine gradients should no longer trigger a fixed 1-pixel cutoff.
    CHECK(error[1][2] < error[1][0] * .75); // Fewer wrong-layer selections with higher-resolution query depth.
    CHECK(revealed > 1000 && revealError[2] < revealError[0] * .5);
    for (unsigned g = 0; g < 5; ++g)
        std::printf("Surface group %u: %zu samples, point/bilinear/limited RMS %.9f / %.9f / %.9f px\n", g, count[g],
                    std::sqrt(error[g][0] / count[g]), std::sqrt(error[g][1] / count[g]),
                    std::sqrt(error[g][2] / count[g]));
    std::printf(
        "Surface motion: %u cases / %zu pixels; small-step mixed vectors old %zu, new %zu; reversed-Z, offsets, "
        "FP16/FP32, no-depth, 65/100/125%%, packed and uniform\n",
        cases, pixels, blendedOld, blendedNew);
    std::printf("Revealed background: %zu current-frame queries; point/bilinear/limited RMS %.9f / %.9f / %.9f px\n",
                revealed, std::sqrt(revealError[0] / revealed), std::sqrt(revealError[1] / revealed),
                std::sqrt(revealError[2] / revealed));
    std::printf("Shared NR/private-DLSS motion: %zu components, single unit conversion and Hold checked\n",
                privateComponents);
    return fails ? 1 : 0;
}
