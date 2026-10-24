#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "packed_stage_fixture.h"
#include <DirectXPackedVector.h>

int main(int argc, char** argv)
{
    try
    {
        Runner r(true, 160);
        std::ofstream out("artifacts/validation/minified-motion-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        for (bool packed : { false, true })
            for (int pattern = 0; pattern < 3; ++pattern)
                for (int configuredScale : { 25, 40, 65, 100 })
                {
                    if (argc > 1 && (configuredScale != 100 || !packed))
                        continue;
                    const int scale = packed && configuredScale == 25 ? 28 : configuredScale;
                    if (!packed && scale == 100)
                        continue; // Native NR does not enter a guide-resample path.
                    DlssNr::Spatial::Settings fixtureSettings;
                    fixtureSettings.enabled = true;
                    if (!packed && scale == 25)
                    {
                        // The production minimum packed extent is 25% per axis.
                        // Initialize ordinary 25% colour via an almost-identity
                        // fixture pack; its setup dispatches are outside timing.
                        fixtureSettings.workX = fixtureSettings.workY = 99.99f;
                    }
                    PackedStages f(r, scale, 0, &fixtureSettings);
                    if (!f.layout.active)
                        throw std::runtime_error("Timing requires an active production warp");
                    auto& s = packed ? f.after : f.before;
                    if (packed)
                    {
                        s.proxy = f.packedProxy;
                        s.answer = f.packedAnswer;
                        s.depth = f.packedDepth;
                        s.motion = f.packedMotion;
                    }
                    auto shader0 = r.pipeline("artifacts/validation/motion-minify-base",
                                              packed ? "host_surface_guides" : "host_uniform_surface_guides");
                    auto shader2 = r.pipeline(dir, packed ? "host_surface_guides" : "host_uniform_surface_guides");
                    auto correction = r.pipeline(dir, packed ? "host_minified_surface_guides"
                                                             : "host_uniform_minified_surface_guides");
                    auto uploadTexture = [&](UINT w, UINT h, DXGI_FORMAT format, unsigned texelBytes, bool isDepth)
                    {
                        auto d = f.packedMotion->GetDesc();
                        d.Width = w;
                        d.Height = h;
                        d.Format = format;
                        d.Flags = D3D12_RESOURCE_FLAG_NONE;
                        auto image = r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                        UINT64 bytes;
                        r.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
                        auto upload = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        char* data;
                        check(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)));
                        for (UINT y = 0; y < h; ++y)
                            for (UINT x = 0; x < w; ++x)
                            {
                                const float nx = (x + .5f) * 3840 / w, ny = (y + .5f) * 2160 / h;
                                if (isDepth)
                                {
                                    const float z =
                                        pattern == 2
                                            ? .2f +
                                                  ((std::fmod(nx, 8.f) < 2.9f && std::fmod(ny, 8.f) < 2.9f) ? .4f : 0.f)
                                        : pattern ? .2f + (nx > 1920.8f ? .4f : 0.f)
                                                  : .2f + nx * .00002f + ny * .00001f;
                                    std::memcpy(data + y * fp.Footprint.RowPitch + x * texelBytes, &z, 4);
                                }
                                else
                                {
                                    const float vx =
                                        pattern == 2
                                            ? ((std::fmod(nx, 8.f) < 2.9f && std::fmod(ny, 8.f) < 2.9f) ? 12.f : -2.f)
                                        : pattern ? (nx > 1920.8f ? 12.f : 0.f)
                                                  : 8.f + nx * .01f;
                                    const float vy = pattern ? 0.f : -2.f + ny * .005f;
                                    const uint16_t v[2] { DirectX::PackedVector::XMConvertFloatToHalf(vx / -2.f),
                                                          DirectX::PackedVector::XMConvertFloatToHalf(vy / 2.f) };
                                    std::memcpy(data + y * fp.Footprint.RowPitch + x * texelBytes, v, 4);
                                }
                            }
                        upload->Unmap(0, nullptr);
                        r.begin();
                        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
                        src.pResource = upload.Get();
                        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        src.PlacedFootprint = fp;
                        dst.pResource = image.Get();
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        r.barrier(image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        r.finish();
                        return image;
                    };
                    auto motion = uploadTexture(1920, 1080, DXGI_FORMAT_R16G16_FLOAT, 4, false);
                    auto depth = uploadTexture(3840, 2160, DXGI_FORMAT_R32_FLOAT, 4, true);
                    auto layout = f.layout;
                    if (!packed)
                    {
                        layout.modelW = s.w;
                        layout.modelH = s.h;
                        layout.warp.x = pw::BuildAxis(3840, 3840, s.w, 1, 0, 0);
                        layout.warp.y = pw::BuildAxis(2160, 2160, s.h, 1, 0, 0);
                    }
                    const DlssNr::GuideRegions regions { { 0, 0, 3840, 2160 }, { 0, 0, 1920, 1080 } };
                    auto gc = DlssNr::Spatial::MakeGuideConstants(layout, regions, { 1920, 1080 }, -1, 1);
                    gc.mode = packed ? 101 : 104;
                    gc.padding[0] = 1;
                    const auto correctionDispatch = DlssNr::Spatial::PrepareMinifiedMotion(gc);
                    auto guide = f.cb(gc);
                    f.table(112, depth.Get(), depth.Get(), motion.Get(), nullptr, nullptr, s.depth.Get(),
                            s.motion.Get());
                    DlssNrConstants carrier {};
                    carrier.Mode = 39;
                    carrier.Width = s.w;
                    carrier.Height = s.h;
                    carrier.TransferStrength = float(packed ? s.w : 1920);
                    carrier.ColourStrength = float(packed ? s.h : 1080);
                    carrier.MvScaleX = packed ? 1.f : -float(s.w) / 1920;
                    carrier.MvScaleY = packed ? 1.f : float(s.h) / 1080;
                    carrier.TransferStrength = float(s.w);
                    carrier.ColourStrength = float(s.h);
                    auto resampledCarrier = f.cb(carrier);
                    f.table(136, s.proxy.Get(), s.answer.Get(), s.motion.Get(), nullptr, nullptr, s.carrier.Get(),
                            s.temporalMotion.Get());
                    r.begin();
                    f.roots();
                    if (packed)
                        for (auto* image : { s.depth.Get(), s.motion.Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    else
                    {
                        f.dispatch(f.spatial.Get(), f.unpackCb.Get(), 24, s.w, s.h);
                        for (auto* image : { s.proxy.Get(), s.answer.Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }
                    r.finish();
                    const auto draw = [&](unsigned method)
                    {
                        f.dispatch(method == 0 ? shader0.Get() : shader2.Get(), guide.Get(), 112, s.w, s.h);
                        if (method && correctionDispatch.pixels)
                        {
                            D3D12_RESOURCE_BARRIER barrier {};
                            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            barrier.UAV.pResource = s.motion.Get();
                            r.list->ResourceBarrier(1, &barrier);
                            r.list->SetPipelineState(correction.Get());
                            r.list->Dispatch(correctionDispatch.groupsX, correctionDispatch.groupsY, 1);
                        }
                        for (auto* image : { s.depth.Get(), s.motion.Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        f.dispatch(f.carrier.Get(), resampledCarrier.Get(), 136, s.w, s.h);
                        for (auto* image : { s.carrier.Get(), s.temporalMotion.Get() })
                        {
                            D3D12_RESOURCE_BARRIER b {};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            b.UAV.pResource = image;
                            r.list->ResourceBarrier(1, &b);
                        }
                        for (auto* image : { s.depth.Get(), s.motion.Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    };
                    const std::string name = std::string(packed ? "packed" : "uniform") + (pattern == 2 ? "-islands"
                                                                                           : pattern    ? "-edge"
                                                                                                        : "-affine");
                    MeasureCandidate(
                        f.images, out, name.c_str(), scale, [&] { draw(0); }, [&] { draw(2); });
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
