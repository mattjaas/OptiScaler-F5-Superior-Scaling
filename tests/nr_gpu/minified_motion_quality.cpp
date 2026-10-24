#include "pch.h"
#include <limits>
#include <fstream>
#include <DirectXPackedVector.h>
#define main LegacySpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_minified_surface_guides_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_uniform_minified_surface_guides_Shader.h"

int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    ComPtr<ID3D12PipelineState> current[2], previous[2], correction[2];
    for (unsigned packed = 0; packed < 2; ++packed)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
        d.pRootSignature = gpu.rootSignature.Get();
        d.CS = packed
                   ? D3D12_SHADER_BYTECODE { host_surface_guides_cso, sizeof(host_surface_guides_cso) }
                   : D3D12_SHADER_BYTECODE { host_uniform_surface_guides_cso, sizeof(host_uniform_surface_guides_cso) };
        CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&current[packed]))));
        d.CS = packed ? D3D12_SHADER_BYTECODE { host_minified_surface_guides_cso,
                                                sizeof(host_minified_surface_guides_cso) }
                      : D3D12_SHADER_BYTECODE { host_uniform_minified_surface_guides_cso,
                                                sizeof(host_uniform_minified_surface_guides_cso) };
        CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&correction[packed]))));
        std::ifstream file(std::string("artifacts/validation/motion-minify-base/") +
                               (packed ? "host_surface_guides_Shader.cso" : "host_uniform_surface_guides_Shader.cso"),
                           std::ios::binary);
        previous[packed] = current[packed];
        if (file)
        {
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            d.CS = { bytes.data(), bytes.size() };
            previous[packed].Reset();
            CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&previous[packed]))));
        }
    }
    size_t pixels = 0, unchanged = 0, recoverable = 0, corrected = 0, regressed = 0, mixed = 0, layerQueries = 0;
    double oldLayerError = 0, newLayerError = 0, affineError = 0;
    unsigned cases = 0, twoDimensionalPlans = 0;
    for (auto dim : { std::pair<UINT, UINT> { 128, 96 }, { 129, 97 } })
        for (float resolution : { .25f, .4f, .5f, 1.f, 1.25f })
            for (auto density :
                 { std::pair<float, float> { .5f, .5f }, { 1.f, 1.f }, { 2.f, 2.f }, { .5f, 2.f }, { 2.f, .5f } })
                for (bool packed : { false, true })
                    for (bool half : { false, true })
                        for (bool reversed : { false, true })
                            for (unsigned scene = 0; scene < 9; ++scene)
                            {
                                const UINT W = dim.first, H = dim.second, mw = UINT(W * density.first),
                                           mh = UINT(H * density.second);
                                const UINT mx = 5, my = 3, aw = mw + 17, ah = mh + 9;
                                const UINT dx = 3, dy = 7, dw = W * 2, dh = H * 2, da = dw + 11, db = dh + 13;
                                Sp::Settings settings;
                                settings.enabled = packed;
                                settings.offsetX = 2;
                                settings.shiftY = -1;
                                auto layout = Sp::Build(settings, W, H, resolution, false);
                                if (packed && !layout.active)
                                    continue; // Production never dispatches an inactive warp.
                                if (!packed)
                                {
                                    layout.modelW = layout.ordinaryW;
                                    layout.modelH = layout.ordinaryH;
                                    layout.warp.x = pw::BuildAxis(W, W, layout.modelW, 1, 0, 0);
                                    layout.warp.y = pw::BuildAxis(H, H, layout.modelH, 1, 0, 0);
                                }
                                const UINT ow = layout.modelW, oh = layout.modelH;
                                DlssNr::GuideRegions regions { { dx, dy, dw, dh }, { mx, my, mw, mh } };
                                auto cb = Sp::MakeGuideConstants(layout, regions, { mw, mh }, -1.f, 1.f);
                                cb.mode = packed ? 101u : 104u;
                                cb.padding[0] = scene == 7 ? 0.f : 1.f;
                                auto foreground = [&](float x, float y)
                                {
                                    if (scene == 4)
                                        return std::fmod(x + y, 12.f) < 2.8f;
                                    return std::fmod(x, 8.f) < 2.9f && std::fmod(y, 8.f) < 2.9f;
                                };
                                auto velocity = [&](float x, float y)
                                {
                                    if (scene == 0 || scene == 6)
                                        return std::pair<float, float> { 4.f, -2.f };
                                    if (scene == 1)
                                        return std::pair<float, float> { 2.f + x * .125f - y * .0625f,
                                                                         -1.f + x * .0625f + y * .125f };
                                    if (scene >= 5)
                                        return std::pair<float, float> { x < W * .5f ? 3.5f : 4.f, -2.f };
                                    return std::pair<float, float> { foreground(x, y) ? 12.f : -2.f, 0.f };
                                };
                                auto deviceDepth = [&](float x, float y)
                                {
                                    if (scene == 8)
                                        return std::numeric_limits<float>::quiet_NaN();
                                    float z = scene >= 2 && scene <= 4 ? (foreground(x, y) ? .6f : .2f) : .3f;
                                    if (scene == 1 || scene == 3)
                                        z += x * .0001f + y * .0002f;
                                    return reversed ? 1.f - z : z;
                                };
                                std::vector<float> mv(size_t(aw) * ah * 2, 12345.f), depth(size_t(da) * db, 12345.f);
                                for (UINT y = 0; y < mh; ++y)
                                    for (UINT x = 0; x < mw; ++x)
                                    {
                                        const auto v = velocity((x + .5f) * W / mw, (y + .5f) * H / mh);
                                        const size_t i = (size_t(y + my) * aw + x + mx) * 2;
                                        mv[i] = v.first / cb.motionScale[0];
                                        mv[i + 1] = v.second / cb.motionScale[1];
                                    }
                                if (scene == 6)
                                {
                                    mv[(size_t(my + mh / 2) * aw + mx + mw / 2) * 2] =
                                        std::numeric_limits<float>::quiet_NaN();
                                    mv[(size_t(my + mh / 2) * aw + mx + mw / 2 + 1) * 2] =
                                        std::numeric_limits<float>::infinity();
                                }
                                for (UINT y = 0; y < dh; ++y)
                                    for (UINT x = 0; x < dw; ++x)
                                        depth[size_t(y + dy) * da + x + dx] =
                                            deviceDepth((x + .5f) * W / dw, (y + .5f) * H / dh);
                                std::vector<uint16_t> hv(mv.size());
                                for (size_t i = 0; i < mv.size(); ++i)
                                    hv[i] = DirectX::PackedVector::XMConvertFloatToHalf(mv[i]);
                                auto mt = half ? gpu.Upload(DXGI_FORMAT_R16G16_FLOAT, aw, ah, 4, hv.data())
                                               : gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, mv.data());
                                auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, da, db, 4, depth.data());
                                std::vector<uint8_t> output[2], outDepth[2];
                                for (unsigned version = 0; version < 2; ++version)
                                {
                                    if (version == 0 && !previous[packed])
                                        continue;
                                    auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, ow, oh, true),
                                         om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, ow, oh, true);
                                    gpu.guides = version ? current[packed] : previous[packed];
                                    gpu.colour = gpu.guides;
                                    gpu.Dispatch(cb, dt.Get(), dt.Get(), mt.Get(), od.Get(), om.Get());
                                    if (version)
                                    {
                                        auto corrected = cb;
                                        auto dispatch = Sp::PrepareMinifiedMotion(corrected);
                                        if (dispatch.pixels)
                                        {
                                            if (W == 129 && scene == 2)
                                            {
                                                // Exercise the same 2D work-list indexing used above 65,535 groups,
                                                // with a small X cap so no giant test textures are needed.
                                                dispatch.groupsX = std::min(dispatch.groupsX, 3u);
                                                dispatch.groupsY = (dispatch.pixels + dispatch.groupsX * 64 - 1) /
                                                                   (dispatch.groupsX * 64);
                                                corrected.padding[1] = float(dispatch.groupsX * 64);
                                                ++twoDimensionalPlans;
                                            }
                                            void* data = nullptr;
                                            CHECK(SUCCEEDED(gpu.constants->Map(0, nullptr, &data)));
                                            std::memcpy(data, &corrected, sizeof(corrected));
                                            gpu.constants->Unmap(0, nullptr);
                                            gpu.Begin();
                                            D3D12_RESOURCE_BARRIER barrier {};
                                            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                                            barrier.UAV.pResource = om.Get();
                                            gpu.list->ResourceBarrier(1, &barrier);
                                            ID3D12DescriptorHeap* heaps[] = { gpu.heap.Get() };
                                            gpu.list->SetDescriptorHeaps(1, heaps);
                                            gpu.list->SetComputeRootSignature(gpu.rootSignature.Get());
                                            gpu.list->SetComputeRootDescriptorTable(
                                                0, gpu.heap->GetGPUDescriptorHandleForHeapStart());
                                            gpu.list->SetPipelineState(correction[packed].Get());
                                            gpu.list->Dispatch(dispatch.groupsX, dispatch.groupsY, 1);
                                            gpu.End();
                                        }
                                    }
                                    output[version] = gpu.Read(om.Get(), 8);
                                    outDepth[version] = gpu.Read(od.Get(), 4);
                                }
                                const auto* now = reinterpret_cast<const float*>(output[1].data());
                                const auto* old =
                                    output[0].empty() ? nullptr : reinterpret_cast<const float*>(output[0].data());
                                if (old)
                                    CHECK(outDepth[0] == outDepth[1]);
                                for (UINT y = 0; y < oh; ++y)
                                    for (UINT x = 0; x < ow; ++x)
                                    {
                                        const size_t i = (size_t(y) * ow + x) * 2;
                                        const float nx =
                                            packed ? pw::Unpack(x + .5f, layout.warp.x) : (x + .5f) * W / ow;
                                        const float ny =
                                            packed ? pw::Unpack(y + .5f, layout.warp.y) : (y + .5f) * H / oh;
                                        const float left =
                                            packed ? pw::Unpack(float(x), layout.warp.x) : float(x) * W / ow;
                                        const float right =
                                            packed ? pw::Unpack(float(x + 1), layout.warp.x) : float(x + 1) * W / ow;
                                        const float top =
                                            packed ? pw::Unpack(float(y), layout.warp.y) : float(y) * H / oh;
                                        const float bottom =
                                            packed ? pw::Unpack(float(y + 1), layout.warp.y) : float(y + 1) * H / oh;
                                        const bool minified =
                                            (right - left) * mw / W > 1.0001f || (bottom - top) * mh / H > 1.0001f;
                                        const auto px = std::clamp(int(nx * dw / W), 0, int(dw) - 1),
                                                   py = std::clamp(int(ny * dh / H), 0, int(dh) - 1);
                                        const float sx = packed ? 1.f : cb.motionScale[0] * ow / W,
                                                    sy = packed ? 1.f : cb.motionScale[1] * oh / H;
                                        const float gotx = now[i] * sx, goty = now[i + 1] * sy;
                                        CHECK(std::isfinite(gotx) && std::isfinite(goty));
                                        const float queryZ =
                                            reinterpret_cast<const float*>(outDepth[1].data())[size_t(y) * ow + x];
                                        bool sourceDepth = (scene == 7 || scene == 8) && queryZ == 0.f;
                                        for (int yy = std::max(0, py - 1); yy <= std::min(int(dh) - 1, py + 1); ++yy)
                                            for (int xx = std::max(0, px - 1); xx <= std::min(int(dw) - 1, px + 1);
                                                 ++xx)
                                                sourceDepth |= queryZ == depth[size_t(yy + dy) * da + xx + dx];
                                        CHECK(sourceDepth);
                                        if (old && (!minified || scene == 7))
                                        {
                                            CHECK(std::memcmp(now + i, old + i, 8) == 0);
                                            ++unchanged;
                                        }
                                        auto working = [&](float vx, float vy)
                                        {
                                            return packed ? std::pair<float, float> { pw::Pack(nx + vx, layout.warp.x) -
                                                                                          pw::Pack(nx, layout.warp.x),
                                                                                      pw::Pack(ny + vy, layout.warp.y) -
                                                                                          pw::Pack(ny, layout.warp.y) }
                                                          : std::pair<float, float> { vx * ow / W, vy * oh / H };
                                        };
                                        if (scene == 0)
                                        {
                                            const float vx = half ? DirectX::PackedVector::XMConvertHalfToFloat(
                                                                        DirectX::PackedVector::XMConvertFloatToHalf(
                                                                            4.f / cb.motionScale[0])) *
                                                                        cb.motionScale[0]
                                                                  : 4.f;
                                            const float vy = half ? DirectX::PackedVector::XMConvertHalfToFloat(
                                                                        DirectX::PackedVector::XMConvertFloatToHalf(
                                                                            -2.f / cb.motionScale[1])) *
                                                                        cb.motionScale[1]
                                                                  : -2.f;
                                            const auto expected = working(vx, vy);
                                            CHECK(std::abs(gotx - expected.first) < .002f &&
                                                  std::abs(goty - expected.second) < .002f);
                                        }
                                        if (scene >= 2 && scene <= 4)
                                        {
                                            const auto a = working(12.f, 0.f), b = working(-2.f, 0.f);
                                            if (std::min(std::abs(gotx - a.first), std::abs(gotx - b.first)) > .002f ||
                                                std::abs(goty) > .002f)
                                                ++mixed;
                                            if (scene == 2 && minified)
                                            {
                                                const bool fg = reversed ? queryZ < .5f : queryZ > .5f;
                                                const float expected = fg ? a.first : b.first;
                                                newLayerError += std::pow(gotx - expected, 2);
                                                ++layerQueries;
                                                if (old)
                                                {
                                                    oldLayerError += std::pow(old[i] * sx - expected, 2);
                                                    if (std::abs(old[i] * sx - expected) > .01f &&
                                                        std::abs(gotx - expected) < .002f)
                                                        ++corrected;
                                                    if (std::abs(old[i] * sx - expected) < .002f &&
                                                        std::abs(gotx - expected) > .01f)
                                                        ++regressed;
                                                }
                                                // Independent coverage oracle: a correct-layer source center must
                                                // exist.
                                                bool covered = false;
                                                const int lx = std::max(0, int(std::floor(left * mw / W))),
                                                          hx =
                                                              std::min(int(mw) - 1, int(std::ceil(right * mw / W)) - 1);
                                                const int ly = std::max(0, int(std::floor(top * mh / H))),
                                                          hy = std::min(int(mh) - 1,
                                                                        int(std::ceil(bottom * mh / H)) - 1);
                                                for (int yy = ly; yy <= hy; ++yy)
                                                    for (int xx = lx; xx <= hx; ++xx)
                                                        covered |=
                                                            foreground((xx + .5f) * W / mw, (yy + .5f) * H / mh) == fg;
                                                if (covered && hx - lx < 8 && hy - ly < 8)
                                                {
                                                    ++recoverable;
                                                    CHECK(std::abs(gotx - expected) < .002f);
                                                }
                                            }
                                        }
                                        if (scene == 1 && !half && nx > 8 && ny > 8 && nx < W - 8 && ny < H - 8)
                                        {
                                            const auto v = velocity(nx, ny), expected = working(v.first, v.second);
                                            affineError = std::max(affineError,
                                                                   double(std::max(std::abs(gotx - expected.first),
                                                                                   std::abs(goty - expected.second))));
                                        }
                                    }
                                pixels += size_t(ow) * oh;
                                ++cases;
                            }
    CHECK(mixed == 0 && recoverable > 1000 && twoDimensionalPlans > 10);
    std::printf("2D minifier work-list dispatches: %u\n", twoDimensionalPlans);
    if (previous[0] && previous[1])
        CHECK(corrected > 1000 && regressed == 0 && newLayerError < oldLayerError);
    std::printf("Regressed depth-layer choices: %zu\n", regressed);
    CHECK(affineError < .003);
    std::printf(
        "Minified motion: %u cases, %zu pixels, %zu recoverable queries; mixed vectors %zu; affine max %.9f px\n",
        cases, pixels, recoverable, mixed, affineError);
    if (previous[0] && previous[1])
        std::printf("Against 2b2eb39f: %zu bit-identical non-minified/no-depth queries; %zu corrected layer choices; "
                    "layer RMS %.9f -> %.9f px (%zu queries)\n",
                    unchanged, corrected, std::sqrt(oldLayerError / layerQueries),
                    std::sqrt(newLayerError / layerQueries), layerQueries);
    return fails ? 1 : 0;
}
