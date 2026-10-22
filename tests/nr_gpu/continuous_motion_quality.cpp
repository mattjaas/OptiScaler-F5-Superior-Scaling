#include "pch.h"
#include <limits>
#include <DirectXPackedVector.h>
#define main LegacySpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_spatial_guides_Shader.h"

int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    auto point = gpu.guides;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = gpu.rootSignature.Get();
    pd.CS = { host_spatial_guides_cso, sizeof(host_spatial_guides_cso) };
    ComPtr<ID3D12PipelineState> continuous;
    CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&continuous))));
    if (!continuous)
        return 2;
    unsigned cases = 0;
    size_t pixels = 0, affineInterior = 0, oldWrong = 0;
    double oldSquared = 0, newSquared = 0;
    float maxError = 0;
    for (auto size : { std::pair<UINT, UINT> { 320, 192 }, { 321, 193 } })
        for (unsigned density : { 1u, 2u, 3u })
            for (float scale : { .65f, 1.f })
                for (unsigned pattern = 0; pattern < 4; ++pattern)
                    for (bool half : { false, true })
                    {
                        const auto W = size.first, H = size.second;
                        Sp::Settings settings;
                        settings.enabled = true;
                        settings.offsetX = 2;
                        settings.shiftY = -1;
                        const auto l = Sp::Build(settings, W, H, scale, false);
                        CHECK(l.active);
                        const UINT mw = W / density, mh = H / density, mx = 7, my = 3;
                        const UINT aw = mw + 19, ah = mh + 11;
                        DlssNr::GuideRegions regions { { 1, 2, W / 2, H / 2 }, { mx, my, mw, mh } };
                        // Normalized, signed game motion with low-resolution reference metadata.
                        const auto constants = Sp::MakeGuideConstants(l, regions, { mw, mh }, -float(mw), float(mh));
                        const auto velocity = [&](float x, float y)
                        {
                            if (pattern == 0)
                                return std::pair<float, float> { 3.5f, -1.75f };
                            if (pattern == 2)
                                return std::pair<float, float> { x < W / 2.f ? 4.f : 32.f, -2.f };
                            return std::pair<float, float> { 2.f + x / 16.f - y / 32.f, -.5f + x / 32.f + y / 16.f };
                        };
                        std::vector<float> mv(size_t(aw) * ah * 2, 12345.f);
                        for (UINT y = 0; y < mh; ++y)
                            for (UINT x = 0; x < mw; ++x)
                            {
                                const auto v = velocity((x + .5f) * W / mw, (y + .5f) * H / mh);
                                const size_t i = (size_t(y + my) * aw + x + mx) * 2;
                                mv[i] = v.first / constants.motionScale[0];
                                mv[i + 1] = v.second / constants.motionScale[1];
                            }
                        if (pattern == 3)
                            mv[(size_t(my + mh / 2) * aw + mx + mw / 2) * 2] = std::numeric_limits<float>::quiet_NaN();
                        const UINT dw = W / 2 + 5, dh = H / 2 + 7;
                        std::vector<float> depth(size_t(dw) * dh);
                        for (size_t i = 0; i < depth.size(); ++i)
                            depth[i] = .1f + float(i) / depth.size();
                        auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, dw, dh, 4, depth.data());
                        std::vector<uint16_t> packed(mv.size());
                        for (size_t i = 0; i < mv.size(); ++i)
                            packed[i] = DirectX::PackedVector::XMConvertFloatToHalf(mv[i]);
                        auto mt = half ? gpu.Upload(DXGI_FORMAT_R16G16_FLOAT, aw, ah, 4, packed.data())
                                       : gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, mv.data());
                        auto run = [&](ID3D12PipelineState* p)
                        {
                            gpu.guides = p;
                            auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, l.modelW, l.modelH, true);
                            auto om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, l.modelW, l.modelH, true);
                            gpu.Dispatch(constants, dt.Get(), dt.Get(), mt.Get(), od.Get(), om.Get());
                            return std::make_pair(gpu.Read(od.Get(), 4), gpu.Read(om.Get(), 8));
                        };
                        const auto before = run(point.Get()), after = run(continuous.Get());
                        CHECK(before.first == after.first); // Depth must retain exact original point sampling.
                        const auto* a = reinterpret_cast<const float*>(before.second.data());
                        const auto* b = reinterpret_cast<const float*>(after.second.data());
                        if (pattern == 0)
                            for (size_t i = 0; i < before.second.size() / 4; ++i)
                                CHECK(std::abs(a[i] - b[i]) < .0001f); // FP32 endpoint reassociation only.
                        for (UINT y = 0; y < l.modelH; ++y)
                            for (UINT x = 0; x < l.modelW; ++x)
                            {
                                const size_t i = (size_t(y) * l.modelW + x) * 2;
                                CHECK(std::isfinite(b[i]) && std::isfinite(b[i + 1]));
                                if (pattern != 1 && pattern != 2)
                                    continue;
                                const float nx = pw::Unpack(x + .5f, l.warp.x), ny = pw::Unpack(y + .5f, l.warp.y);
                                const float px = std::clamp(nx, .5f * W / mw, (mw - .5f) * W / mw);
                                const float py = std::clamp(ny, .5f * H / mh, (mh - .5f) * H / mh);
                                auto v = velocity(px, py);
                                if (pattern == 2)
                                {
                                    const UINT sx = UINT(std::clamp(int(nx * mw / W), 0, int(mw) - 1));
                                    v = velocity((sx + .5f) * W / mw, py);
                                }
                                const float expected[2] { pw::Pack(nx + v.first, l.warp.x) - pw::Pack(nx, l.warp.x),
                                                          pw::Pack(ny + v.second, l.warp.y) - pw::Pack(ny, l.warp.y) };
                                for (unsigned c = 0; c < 2; ++c)
                                {
                                    const float e = std::abs(b[i + c] - expected[c]);
                                    maxError = std::max(maxError, e);
                                    CHECK(e <
                                          (half ? .02f
                                                : .0003f)); // Includes source half quantization, not FP16 arithmetic.
                                }
                                if (pattern == 1 && scale == 1.f && density > 1 && nx > l.centerBounds.left * W + 12 &&
                                    nx < l.centerBounds.right * W - 12 && ny > l.centerBounds.top * H + 12 &&
                                    ny < l.centerBounds.bottom * H - 12)
                                {
                                    const float dx = a[i] - expected[0], dy = a[i + 1] - expected[1];
                                    const float ex = b[i] - expected[0], ey = b[i + 1] - expected[1];
                                    oldSquared += dx * dx + dy * dy;
                                    newSquared += ex * ex + ey * ey;
                                    ++affineInterior;
                                    if (std::max(std::abs(dx), std::abs(dy)) > .001f)
                                        ++oldWrong;
                                }
                            }
                        pixels += size_t(l.modelW) * l.modelH;
                        ++cases;
                    }
    CHECK(affineInterior > 10000 && oldWrong > 10000);
    CHECK(newSquared < oldSquared * .05);
    std::printf("Continuous motion: %u cases, %zu pixels, max reference error %.9f px; affine center %zu pixels, "
                "point RMS %.9f -> continuous RMS %.9f px; discontinuities, nonfinite and padded origins checked\n",
                cases, pixels, maxError, affineInterior, std::sqrt(oldSquared / affineInterior),
                std::sqrt(newSquared / affineInterior));
    return fails ? 1 : 0;
}
