#include "pch.h"
#include <DirectXPackedVector.h>
#include <limits>
#define main LegacySpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_minified_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_minified_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_joint_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_joint_minified_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_joint_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_joint_minified_guides_Shader.h"

int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    ComPtr<ID3D12PipelineState> old[2], fix[2], joint[2][2];
    auto make = [&](const void* bytes, size_t size, ComPtr<ID3D12PipelineState>& result)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        d.pRootSignature = gpu.rootSignature.Get();
        d.CS = { bytes, size };
        CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&result))));
    };
#define PSO(name, dst) make(name##_cso, sizeof(name##_cso), dst)
    PSO(host_surface_guides, old[1]);
    PSO(host_uniform_surface_guides, old[0]);
    PSO(host_minified_surface_guides, fix[1]);
    PSO(host_uniform_minified_surface_guides, fix[0]);
    PSO(host_joint_guides, joint[1][0]);
    PSO(host_joint_minified_guides, joint[1][1]);
    PSO(host_uniform_joint_guides, joint[0][0]);
    PSO(host_uniform_joint_minified_guides, joint[0][1]);
#undef PSO
    size_t pixels = 0, planeCount = 0, covered = 0, layerCount = 0, changedToForeground = 0, mismatch[2] {};
    double planeDepthError[2] {}, planeMotionError[2] {};
    unsigned cases = 0, twoDimensionalPlans = 0;
    for (auto dim : { std::pair<UINT, UINT> { 128, 96 }, { 129, 97 } })
        for (float scale : { .25f, .65f, 1.f, 1.25f })
            for (bool packed : { false, true })
                for (auto density : { std::pair<float, float> { .5f, .5f }, { 1.f, 1.f }, { 1.f, .5f } })
                    for (bool reversed : { false, true })
                        for (bool half : { false, true })
                            for (unsigned scene = 0; scene < 7; ++scene)
                            {
                                const UINT W = dim.first, H = dim.second, dw = UINT(W * density.first),
                                           dh = UINT(H * density.first), mw = UINT(W * density.second),
                                           mh = UINT(H * density.second);
                                const UINT dx = 3, dy = 7, da = dw + 11, db = dh + 13, mx = 5, my = 3, ma = mw + 17,
                                           mb = mh + 9;
                                Sp::Settings settings;
                                settings.enabled = packed;
                                settings.offsetX = 2;
                                settings.shiftY = -1;
                                auto layout = Sp::Build(settings, W, H, scale, false);
                                if (packed && !layout.active)
                                    continue;
                                if (!packed)
                                {
                                    layout.modelW = layout.ordinaryW;
                                    layout.modelH = layout.ordinaryH;
                                    layout.warp.x = pw::BuildAxis(W, W, layout.modelW, 1, 0, 0);
                                    layout.warp.y = pw::BuildAxis(H, H, layout.modelH, 1, 0, 0);
                                }
                                const UINT ow = layout.modelW, oh = layout.modelH;
                                const DlssNr::GuideRegions regions { { dx, dy, dw, dh }, { mx, my, mw, mh } };
                                auto cb = Sp::MakeGuideConstants(layout, regions, { mw, mh }, -1, 1);
                                cb.mode = packed ? 101 : 104;
                                cb.padding[0] = scene == 6 ? 0.f : 1.f;
                                cb.unused = reversed ? 1.f : 0.f;
                                auto foreground = [&](float x, float y)
                                {
                                    if (scene == 3)
                                        return x > W * .5f + .3f && x < W * .5f + 3.7f;
                                    if (scene == 4)
                                        return x + y > W * .5f + H * .5f + .3f;
                                    return x > W * .5f + .3f;
                                };
                                auto zAt = [&](float x, float y)
                                {
                                    const float z = scene == 1                 ? .2f + x * .00015f + y * .0001f
                                                    : scene >= 2 && scene <= 4 ? (foreground(x, y) ? .2f : .7f)
                                                                               : .3f;
                                    return reversed ? 1.f - z : z;
                                };
                                auto vAt = [&](float x, float y)
                                {
                                    if (scene == 1)
                                        return std::pair<float, float> { 2 + x * .0625f - y * .03125f,
                                                                         -1 + x * .03125f + y * .0625f };
                                    if (scene >= 2 && scene <= 4)
                                        return std::pair<float, float> { foreground(x, y) ? 12.f : -2.f,
                                                                         foreground(x, y) ? -1.f : 1.f };
                                    return std::pair<float, float> { 4.f, -2.f };
                                };
                                std::vector<float> depth(size_t(da) * db, 12345), motion(size_t(ma) * mb * 2, 12345);
                                for (UINT y = 0; y < dh; ++y)
                                    for (UINT x = 0; x < dw; ++x)
                                        depth[size_t(y + dy) * da + x + dx] =
                                            zAt((x + .5f) * W / dw, (y + .5f) * H / dh);
                                for (UINT y = 0; y < mh; ++y)
                                    for (UINT x = 0; x < mw; ++x)
                                    {
                                        const auto v = vAt((x + .5f) * W / mw, (y + .5f) * H / mh);
                                        const auto i = (size_t(y + my) * ma + x + mx) * 2;
                                        motion[i] = v.first / cb.motionScale[0];
                                        motion[i + 1] = v.second / cb.motionScale[1];
                                    }
                                if (scene == 5)
                                {
                                    depth[size_t(dy + dh / 2) * da + dx + dw / 2] = NAN;
                                    motion[(size_t(my + mh / 2) * ma + mx + mw / 2) * 2] = INFINITY;
                                }
                                std::vector<uint16_t> hm(motion.size());
                                for (size_t i = 0; i < motion.size(); ++i)
                                    hm[i] = DirectX::PackedVector::XMConvertFloatToHalf(motion[i]);
                                auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, da, db, 4, depth.data());
                                auto mt = half ? gpu.Upload(DXGI_FORMAT_R16G16_FLOAT, ma, mb, 4, hm.data())
                                               : gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, ma, mb, 8, motion.data());
                                std::vector<uint8_t> zs[2], vs[2];
                                for (unsigned version = 0; version < 2; ++version)
                                {
                                    auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, ow, oh, true),
                                         om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, ow, oh, true);
                                    auto selected = version && cb.padding[0] > .5f ? joint[packed][0] : old[packed];
                                    gpu.guides = selected;
                                    gpu.colour = selected;
                                    gpu.Dispatch(cb, dt.Get(), dt.Get(), mt.Get(), od.Get(), om.Get());
                                    if (!version || cb.padding[0] > .5f)
                                    {
                                        auto c = cb;
                                        auto plan =
                                            version ? Sp::PrepareJointMinification(c) : Sp::PrepareMinifiedMotion(c);
                                        if (plan.pixels)
                                        {
                                            if (version && W == 129 && scene == 2)
                                            {
                                                plan.groupsX = std::min(plan.groupsX, 3u);
                                                plan.groupsY =
                                                    (plan.pixels + plan.groupsX * 64 - 1) / (plan.groupsX * 64);
                                                c.padding[1] = float(plan.groupsX * 64);
                                                ++twoDimensionalPlans;
                                            }
                                            void* data;
                                            CHECK(SUCCEEDED(gpu.constants->Map(0, nullptr, &data)));
                                            std::memcpy(data, &c, sizeof(c));
                                            gpu.constants->Unmap(0, nullptr);
                                            gpu.Begin();
                                            D3D12_RESOURCE_BARRIER b {};
                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                                            b.UAV.pResource = version ? nullptr : om.Get();
                                            gpu.list->ResourceBarrier(1, &b);
                                            ID3D12DescriptorHeap* heaps[] = { gpu.heap.Get() };
                                            gpu.list->SetDescriptorHeaps(1, heaps);
                                            gpu.list->SetComputeRootSignature(gpu.rootSignature.Get());
                                            gpu.list->SetComputeRootDescriptorTable(
                                                0, gpu.heap->GetGPUDescriptorHandleForHeapStart());
                                            gpu.list->SetPipelineState(version ? joint[packed][1].Get()
                                                                               : fix[packed].Get());
                                            gpu.list->Dispatch(plan.groupsX, plan.groupsY, 1);
                                            gpu.End();
                                        }
                                    }
                                    zs[version] = gpu.Read(od.Get(), 4);
                                    vs[version] = gpu.Read(om.Get(), 8);
                                }
                                if (scene == 6)
                                    CHECK(zs[0] == zs[1] && vs[0] == vs[1]);
                                const auto* z0 = reinterpret_cast<const float*>(zs[0].data());
                                const auto* z1 = reinterpret_cast<const float*>(zs[1].data());
                                const auto* v0 = reinterpret_cast<const float*>(vs[0].data());
                                const auto* v1 = reinterpret_cast<const float*>(vs[1].data());
                                for (UINT y = 0; y < oh; ++y)
                                    for (UINT x = 0; x < ow; ++x)
                                    {
                                        const size_t i = size_t(y) * ow + x;
                                        const float nx = packed ? pw::Unpack(x + .5f, layout.warp.x)
                                                                : (x + .5f) * W / ow,
                                                    ny = packed ? pw::Unpack(y + .5f, layout.warp.y)
                                                                : (y + .5f) * H / oh;
                                        CHECK(std::isfinite(z1[i]) && std::isfinite(v1[i * 2]) &&
                                              std::isfinite(v1[i * 2 + 1]));
                                        auto work = [&](std::pair<float, float> v)
                                        {
                                            return packed ? std::pair<float, float> { pw::Pack(nx + v.first,
                                                                                               layout.warp.x) -
                                                                                          pw::Pack(nx, layout.warp.x),
                                                                                      pw::Pack(ny + v.second,
                                                                                               layout.warp.y) -
                                                                                          pw::Pack(ny, layout.warp.y) }
                                                          : std::pair<float, float> { v.first * ow / W,
                                                                                      v.second * oh / H };
                                        };
                                        const float sx = packed ? 1 : cb.motionScale[0] * ow / W,
                                                    sy = packed ? 1 : cb.motionScale[1] * oh / H;
                                        if (scene == 1 && nx > 8 && ny > 8 && nx < W - 8 && ny < H - 8)
                                        {
                                            const float expected = zAt(nx, ny);
                                            const auto mv = work(vAt(nx, ny));
                                            planeDepthError[0] += std::pow(z0[i] - expected, 2);
                                            planeDepthError[1] += std::pow(z1[i] - expected, 2);
                                            planeMotionError[0] += std::pow(v0[i * 2] * sx - mv.first, 2) +
                                                                   std::pow(v0[i * 2 + 1] * sy - mv.second, 2);
                                            planeMotionError[1] += std::pow(v1[i * 2] * sx - mv.first, 2) +
                                                                   std::pow(v1[i * 2 + 1] * sy - mv.second, 2);
                                            ++planeCount;
                                        }
                                        if (scene >= 2 && scene <= 4)
                                        {
                                            const bool fg0 = reversed ? z0[i] > .5f : z0[i] < .5f,
                                                       fg1 = reversed ? z1[i] > .5f : z1[i] < .5f;
                                            CHECK(!fg0 || fg1);
                                            if (!fg0 && fg1)
                                                ++changedToForeground;
                                            for (unsigned version = 0; version < 2; ++version)
                                            {
                                                const bool fg = version ? fg1 : fg0;
                                                const auto expected = work({ fg ? 12.f : -2.f, fg ? -1.f : 1.f });
                                                const auto* v = version ? v1 : v0;
                                                if (std::abs(v[i * 2] * sx - expected.first) > .004f ||
                                                    std::abs(v[i * 2 + 1] * sy - expected.second) > .004f)
                                                    ++mismatch[version];
                                            }
                                            ++layerCount;
                                            if (dw == mw && dh == mh)
                                            {
                                                const auto expected = work({ fg1 ? 12.f : -2.f, fg1 ? -1.f : 1.f });
                                                CHECK(std::abs(v1[i * 2] * sx - expected.first) < .004f &&
                                                      std::abs(v1[i * 2 + 1] * sy - expected.second) < .004f);
                                                ++covered;
                                            }
                                        }
                                    }
                                pixels += size_t(ow) * oh;
                                ++cases;
                            }
    CHECK(planeDepthError[1] < planeDepthError[0] * .05 && changedToForeground > 100 && covered > 1000);
    CHECK(mismatch[1] <= mismatch[0] && twoDimensionalPlans > 10);
    std::printf("Joint 2D correction dispatches: %u\n", twoDimensionalPlans);
    std::printf("Joint depth/MV: %u cases, %zu pixels; FP16/FP32, reversed Z, padded/independent regions, "
                "uniform/packed 25/65/100/125%%, invalid/missing depth\n",
                cases, pixels);
    std::printf("Plane depth RMS %.12f -> %.12f; working MV RMS %.9f -> %.9f (%zu pixels)\n",
                std::sqrt(planeDepthError[0] / planeCount), std::sqrt(planeDepthError[1] / planeCount),
                std::sqrt(planeMotionError[0] / planeCount), std::sqrt(planeMotionError[1] / planeCount), planeCount);
    std::printf(
        "Depth/MV layer mismatches %zu -> %zu of %zu; same-grid pair checks %zu; recovered foreground coverage %zu\n",
        mismatch[0], mismatch[1], layerCount, covered, changedToForeground);
    return fails ? 1 : 0;
}
