#include "pch.h"
#define DlssNrConstants HostConstants
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#undef DlssNrConstants
#include "four_candidates_fixture.h"
#include "packed_stage_fixture.h"
#include <DirectXPackedVector.h>

int main()
{
    try
    {
        Runner r(true, 160);
        std::ofstream out("artifacts/validation/surface-motion-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        for (bool packed : { false, true })
            for (int pattern = 0; pattern < 2; ++pattern)
                for (int scale : { 65, 100 })
                {
                    if (!packed && scale == 100)
                        continue; // Native NR does not enter a guide-resample path.
                    PackedStages f(r, scale, 0);
                    auto& s = packed ? f.after : f.before;
                    if (packed)
                    {
                        s.proxy = f.packedProxy;
                        s.answer = f.packedAnswer;
                        s.depth = f.packedDepth;
                        s.motion = f.packedMotion;
                    }
                    auto shader0 = packed ? f.guides : f.standard;
                    auto shader1 = r.pipeline(dir, packed ? "host_spatial_guides" : "host_uniform_guides");
                    auto shader2 = r.pipeline(dir, packed ? "host_surface_guides" : "host_uniform_surface_guides");
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
                                        pattern ? .2f + (nx > 1920.8f ? .4f : 0.f) : .2f + nx * .00002f + ny * .00001f;
                                    std::memcpy(data + y * fp.Footprint.RowPitch + x * texelBytes, &z, 4);
                                }
                                else
                                {
                                    const float vx = pattern ? (nx > 1920.8f ? 12.f : 0.f) : 8.f + nx * .01f;
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
                    auto guide = f.cb(gc);
                    HostConstants point {};
                    point.Mode = 10;
                    point.Width = s.w;
                    point.Height = s.h;
                    point.GuideWidth = 3840;
                    point.GuideHeight = 2160;
                    point.TransferStrength = 1920;
                    point.ColourStrength = 1080;
                    point.MvScaleX = point.MvScaleY = 1;
                    auto pointCb = f.cb(point);
                    f.table(112, depth.Get(), depth.Get(), motion.Get(), nullptr, nullptr, s.depth.Get(),
                            s.motion.Get());
                    f.table(120, depth.Get(), motion.Get(), motion.Get(), nullptr, nullptr, s.depth.Get(),
                            s.motion.Get());
                    DlssNrConstants carrier {};
                    carrier.Mode = 39;
                    carrier.Width = s.w;
                    carrier.Height = s.h;
                    carrier.TransferStrength = float(packed ? s.w : 1920);
                    carrier.ColourStrength = float(packed ? s.h : 1080);
                    carrier.MvScaleX = packed ? 1.f : -float(s.w) / 1920;
                    carrier.MvScaleY = packed ? 1.f : float(s.h) / 1080;
                    auto pointCarrier = f.cb(carrier);
                    carrier.TransferStrength = float(s.w);
                    carrier.ColourStrength = float(s.h);
                    auto resampledCarrier = f.cb(carrier);
                    f.table(128, s.proxy.Get(), s.answer.Get(), packed ? s.motion.Get() : motion.Get(), nullptr,
                            nullptr, s.carrier.Get(), s.temporalMotion.Get());
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
                        f.dispatch(method == 0   ? shader0.Get()
                                   : method == 1 ? shader1.Get()
                                                 : shader2.Get(),
                                   !packed && method == 0 ? pointCb.Get() : guide.Get(),
                                   !packed && method == 0 ? 120 : 112, s.w, s.h);
                        for (auto* image : { s.depth.Get(), s.motion.Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        f.dispatch(f.carrier.Get(), method == 0 ? pointCarrier.Get() : resampledCarrier.Get(),
                                   method == 0 ? 128 : 136, s.w, s.h);
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
                    for (unsigned base = 0; base < 2; ++base)
                    {
                        const std::string name = std::string(packed ? "packed" : "uniform") +
                                                 (pattern ? "-edge" : "-affine") +
                                                 (base ? "-vs-bilinear" : "-vs-point");
                        MeasureCandidate(
                            f.images, out, name.c_str(), scale, [&] { draw(base); }, [&] { draw(2); });
                    }
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
