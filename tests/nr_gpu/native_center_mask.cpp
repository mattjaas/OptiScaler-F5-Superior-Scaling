#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"

float AxisRamp(float pixel, const pw::Axis& axis)
{
    const int side = pixel < axis.bandCenter ? 0 : 1;
    if (axis.work[side] >= 1.f || axis.center[side] >= 1.f)
        return 0.f;
    return std::clamp((std::abs(pixel - axis.bandCenter) - axis.halfBand + 4.5f) / 4.f, 0.f, 1.f);
}
int main()
{
    try
    {
        Runner r(true);
        auto shader = r.pipeline("artifacts/validation/native-center", "native_mask");
        size_t direct = 0, transition = 0, full = 0, compressed = 0;
        size_t cachedCoordinates = 0;
        for (auto dims : { std::pair<UINT, UINT> { 193, 129 }, { 385, 217 } })
            for (float scale : { .5f, .65f, 1.f, 1.5f })
                for (unsigned preset = 0; preset < 6; ++preset)
                {
                    DlssNr::Spatial::Settings s;
                    s.enabled = true;
                    if (preset < 3)
                        DlssNr::Spatial::ApplyNamedLayout(s, preset);
                    if (preset == 3)
                    {
                        s.offsetX = 7;
                        s.offsetY = -5;
                        s.shiftX = -2;
                        s.shiftY = 1;
                    }
                    if (preset == 4)
                        s.workY = 100;
                    if (preset == 5)
                        s.workX = 100;
                    const auto l = DlssNr::Spatial::Build(s, dims.first, dims.second, scale, false);
                    if (!l.active)
                        throw std::runtime_error("Inactive native mask fixture");
                    auto warp = UploadWarp(r, l);
                    auto oldCache = CachedWarp(r, l, "artifacts/validation/native-center");
                    auto newCache = CachedWarp(r, l);
                    const auto a = ReadTexture(r, oldCache.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    const auto b = ReadTexture(r, newCache.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    const auto stride = std::max(11u, std::max(dims.first, dims.second)) * 4u;
                    for (unsigned i = 0; i < 44; ++i)
                        if (a[i] != b[i])
                            throw std::runtime_error("Warp metadata changed");
                    for (unsigned axis = 0; axis < 2; ++axis)
                        for (unsigned i = 0; i < (axis ? dims.second : dims.first); ++i)
                        {
                            if (a[(axis + 1) * stride + i * 4] != b[(axis + 1) * stride + i * 4])
                                throw std::runtime_error("Existing cached UV channel changed");
                            ++cachedCoordinates;
                        }
                    HostConstants h {};
                    h.Width = dims.first;
                    h.Height = dims.second;
                    h.ModelWorkScale = scale;
                    DlssNrConstants bytes {};
                    std::memcpy(&bytes, &h, 256);
                    auto output = r.run(shader.Get(), dims.first, dims.second, dims.first, dims.second, bytes, 0, false,
                                        8, l.modelW, l.modelH, nullptr, 0, nullptr, 0, 0, 8, warp.Get());
                    for (UINT y = 0; y < dims.second; ++y)
                        for (UINT x = 0; x < dims.first; ++x)
                        {
                            const auto i = (size_t(y) * dims.first + x) * 4;
                            const float actual = TestFloat(output[i]);
                            float expected = 1;
                            if (scale == 1.f)
                            {
                                const float a = AxisRamp(x + .5f, l.warp.x), b = AxisRamp(y + .5f, l.warp.y);
                                const float wx = a * a * (3 - 2 * a), wy = b * b * (3 - 2 * b);
                                expected = 1 - (1 - wx) * (1 - wy);
                            }
                            if (std::abs(actual - expected) > 3e-5f)
                                throw std::runtime_error("Native mask weight mismatch");
                            if (scale != 1.f && actual != 1.f)
                                throw std::runtime_error("Non-native reconstruction changed");
                            if (actual == 0.f)
                            {
                                ++direct;
                                const int px = int(x) + int(std::round(l.warp.x.workCenter - l.warp.x.bandCenter));
                                const int py = int(y) + int(std::round(l.warp.y.workCenter - l.warp.y.bandCenter));
                                if (TestFloat(output[i + 1]) != px || TestFloat(output[i + 2]) != py || px < 0 ||
                                    py < 0 || px >= int(l.modelW) || py >= int(l.modelH))
                                    throw std::runtime_error("Identity texel mismatch");
                            }
                            else if (actual == 1.f)
                                ++full;
                            else
                                ++transition;
                            // Any native pixel footprint touching a compressed side must already be fully guided.
                            if (scale == 1.f)
                            {
                                auto touches = [](float p, const pw::Axis& a)
                                {
                                    const int side = p < a.bandCenter ? 0 : 1;
                                    return a.work[side] < 1.f && a.center[side] < 1.f &&
                                           std::abs(p - a.bandCenter) + .5f >= a.halfBand;
                                };
                                if (touches(x + .5f, l.warp.x) || touches(y + .5f, l.warp.y))
                                {
                                    ++compressed;
                                    if (actual != 1.f)
                                        throw std::runtime_error("Compression begins before full guided weight");
                                }
                            }
                        }
                }
        // Timed 4K layouts must retain the old UV values too; auxiliary channels may differ.
        for (float scale : { .65f, 1.f })
            for (unsigned preset = 0; preset < 3; ++preset)
            {
                DlssNr::Spatial::Settings settings;
                settings.enabled = true;
                DlssNr::Spatial::ApplyNamedLayout(settings, preset);
                const auto l = DlssNr::Spatial::Build(settings, 3840, 2160, scale, false);
                auto oldCache = CachedWarp(r, l, "artifacts/validation/native-center"), newCache = CachedWarp(r, l);
                const auto a = ReadTexture(r, oldCache.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const auto b = ReadTexture(r, newCache.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (unsigned axis = 0; axis < 2; ++axis)
                    for (unsigned i = 0; i < (axis ? 2160u : 3840u); ++i)
                    {
                        if (a[(axis + 1) * 3840u * 4 + i * 4] != b[(axis + 1) * 3840u * 4 + i * 4])
                            throw std::runtime_error("4K cached UV channel changed");
                        ++cachedCoordinates;
                    }
            }
        if (!direct || !transition || !compressed)
            throw std::runtime_error("Missing mask coverage");
        std::cout << "Native centre mask PASS: direct " << direct << ", transition " << transition << ", full " << full
                  << ", all " << compressed << " compressed footprints fully guided; " << cachedCoordinates
                  << " cached UVs bit-exact; shifted/one-axis/odd rasters\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
