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
        Runner r(true, 128);
        std::ofstream out("artifacts/validation/continuous-motion-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        auto continuous = r.pipeline("OptiScaler/shaders/dlssnr/superior/precompile", "host_spatial_guides");
        for (int pattern = 0; pattern < 3; ++pattern)
            for (int scale : { 65, 100 })
            {
                PackedStages f(r, scale, 0);
                constexpr UINT w = 1920, h = 1080;
                auto desc = f.packedMotion->GetDesc();
                desc.Width = w;
                desc.Height = h;
                desc.Format = DXGI_FORMAT_R16G16_FLOAT;
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                auto mv = r.resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                UINT64 bytes;
                r.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
                auto upload = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                char* data;
                check(upload->Map(0, nullptr, reinterpret_cast<void**>(&data)));
                for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < w; ++x)
                    {
                        const float vx = pattern == 0 ? 8.f : 8.f + x * .01f + (pattern == 2 && x > w / 2 ? 40.f : 0.f);
                        const float vy = pattern == 0 ? -2.f : -2.f + y * .005f;
                        const uint16_t v[2] { DirectX::PackedVector::XMConvertFloatToHalf(vx / -3840.f),
                                              DirectX::PackedVector::XMConvertFloatToHalf(vy / 2160.f) };
                        std::memcpy(data + y * fp.Footprint.RowPitch + x * 4, v, 4);
                    }
                upload->Unmap(0, nullptr);
                r.begin();
                D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
                src.pResource = upload.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                dst.pResource = mv.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                r.barrier(mv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (auto* image : { f.packedDepth.Get(), f.packedMotion.Get() })
                    r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                r.finish();
                const DlssNr::GuideRegions regions { { 0, 0, 3840, 2160 }, { 0, 0, w, h } };
                const auto cb =
                    f.cb(DlssNr::Spatial::MakeGuideConstants(f.layout, regions, { w, h }, -float(w), float(h)));
                f.table(112, f.images.inputs[0].Get(), f.images.inputs[2].Get(), mv.Get(), nullptr, nullptr,
                        f.packedDepth.Get(), f.packedMotion.Get());
                const auto draw = [&](ID3D12PipelineState* p)
                {
                    f.dispatch(p, cb.Get(), 112, f.layout.modelW, f.layout.modelH);
                    D3D12_RESOURCE_BARRIER barriers[2] {};
                    for (unsigned i = 0; i < 2; ++i)
                    {
                        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[i].UAV.pResource = i ? f.packedMotion.Get() : f.packedDepth.Get();
                    }
                    r.list->ResourceBarrier(2, barriers);
                };
                const char* names[] { "motion-constant", "motion-affine", "motion-discontinuity" };
                MeasureCandidate(
                    f.images, out, names[pattern], scale, [&] { draw(f.guides.Get()); },
                    [&] { draw(continuous.Get()); });
            }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
