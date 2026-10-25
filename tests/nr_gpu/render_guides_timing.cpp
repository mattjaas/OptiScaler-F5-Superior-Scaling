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
        Runner r(true, 192);
        std::ofstream out("artifacts/validation/render-guides-timing.csv");
        out << std::setprecision(9) << "stage,scale,variant,block,quartet,position,gpu_ms\n";
        const std::string dir = "OptiScaler/shaders/dlssnr/superior/precompile";
        for (UINT renderW : { 1920u, 2560u })
            for (bool joint : { false, true })
                for (bool privateDlss : { false, true })
                {
                    const UINT renderH = renderW == 1920 ? 1080 : 1440;
                    PackedStages f(r, 100, 0);
                    const auto renderLayout = DlssNr::Spatial::RenderGuideLayout(f.layout, { renderW, renderH });
                    // Upload deterministic sloped depth and affine render-resolution motion outside timing.
                    auto upload = [&](DXGI_FORMAT format, bool isDepth)
                    {
                        auto image = f.texture(renderW, renderH, format);
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
                        UINT64 bytes;
                        const auto desc = image->GetDesc();
                        r.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
                        auto buffer = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                        uint8_t* data;
                        check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&data)));
                        for (UINT y = 0; y < renderH; ++y)
                            for (UINT x = 0; x < renderW; ++x)
                            {
                                float value[2] { .2f + x * .00002f + y * .00001f, 0 };
                                if (!isDepth)
                                {
                                    value[0] = 4.f + x * .001f;
                                    value[1] = -2.f + y * .001f;
                                }
                                std::memcpy(data + y * fp.Footprint.RowPitch + x * (isDepth ? 4 : 8), value,
                                            isDepth ? 4 : 8);
                            }
                        buffer->Unmap(0, nullptr);
                        r.begin();
                        r.barrier(image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
                        src.pResource = buffer.Get();
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
                    auto sourceDepth = upload(DXGI_FORMAT_R32_FLOAT, true);
                    auto sourceMotion = upload(DXGI_FORMAT_R32G32_FLOAT, false);
                    auto packed = r.pipeline(dir, joint ? "host_joint_guides" : "host_surface_guides");
                    auto correction =
                        r.pipeline(dir, joint ? "host_joint_minified_guides" : "host_minified_surface_guides");
                    auto uniform = r.pipeline(dir, joint ? "host_uniform_joint_guides" : "host_uniform_surface_guides");
                    ComPtr<ID3D12Resource> depth[2], motion[2], constants[2];
                    DlssNr::Spatial::MinifiedMotionDispatch plan[2];
                    for (unsigned variant = 0; variant < 2; ++variant)
                    {
                        const auto& layout = variant ? renderLayout : f.layout;
                        depth[variant] = f.texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32_FLOAT);
                        motion[variant] = f.texture(layout.modelW, layout.modelH, DXGI_FORMAT_R32G32_FLOAT);
                        const DlssNr::GuideRegions regions { { 0, 0, renderW, renderH }, { 0, 0, renderW, renderH } };
                        auto c = DlssNr::Spatial::MakeGuideConstants(layout, regions, { renderW, renderH }, 1, 1);
                        c.padding[0] = 1;
                        plan[variant] = joint ? DlssNr::Spatial::PrepareJointMinification(c)
                                              : DlssNr::Spatial::PrepareMinifiedMotion(c);
                        constants[variant] = f.cb(c);
                        f.table(112 + 8 * variant, sourceDepth.Get(), sourceDepth.Get(), sourceMotion.Get(), nullptr,
                                nullptr, depth[variant].Get(), motion[variant].Get());
                    }
                    auto privateDepth = f.texture(f.after.w, f.after.h, DXGI_FORMAT_R32_FLOAT);
                    auto privateMotion = f.texture(f.after.w, f.after.h, DXGI_FORMAT_R32G32_FLOAT);
                    auto l = f.layout;
                    l.warp.x = pw::BuildAxis(3840, 3840, l.modelW, 1, 0, 0);
                    l.warp.y = pw::BuildAxis(2160, 2160, l.modelH, 1, 0, 0);
                    const DlssNr::GuideRegions packedRegions { { 0, 0, renderLayout.modelW, renderLayout.modelH },
                                                               { 0, 0, renderLayout.modelW, renderLayout.modelH } };
                    auto uc = DlssNr::Spatial::MakeGuideConstants(l, packedRegions,
                                                                  { renderLayout.modelW, renderLayout.modelH }, 1, 1);
                    uc.mode = 104;
                    uc.padding[0] = 1;
                    auto uniformCb = f.cb(uc);
                    f.table(128, depth[1].Get(), depth[1].Get(), motion[1].Get(), nullptr, nullptr, privateDepth.Get(),
                            privateMotion.Get());
                    ComPtr<ID3D12Resource> carrierCb[2];
                    for (unsigned variant = 0; variant < 2; ++variant)
                    {
                        DlssNrConstants c {};
                        c.Mode = 39;
                        c.Width = f.after.w;
                        c.Height = f.after.h;
                        c.TransferStrength = float(f.after.w);
                        c.ColourStrength = float(f.after.h);
                        c.MvScaleX = variant ? float(f.after.w) / renderLayout.modelW : 1.f;
                        c.MvScaleY = variant ? float(f.after.h) / renderLayout.modelH : 1.f;
                        carrierCb[variant] = f.cb(c);
                        f.table(136 + variant * 8, f.packedProxy.Get(), f.packedAnswer.Get(),
                                variant ? privateMotion.Get() : motion[0].Get(), nullptr, nullptr,
                                f.after.carrier.Get(), f.after.temporalMotion.Get());
                    }
                    const auto draw = [&](unsigned variant)
                    {
                        const auto& layout = variant ? renderLayout : f.layout;
                        f.dispatch(packed.Get(), constants[variant].Get(), 112 + variant * 8, layout.modelW,
                                   layout.modelH);
                        if (plan[variant].pixels)
                        {
                            D3D12_RESOURCE_BARRIER b {};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            b.UAV.pResource = joint ? nullptr : motion[variant].Get();
                            r.list->ResourceBarrier(1, &b);
                            r.list->SetPipelineState(correction.Get());
                            r.list->Dispatch(plan[variant].groupsX, plan[variant].groupsY, 1);
                        }
                        if (!privateDlss)
                        {
                            D3D12_RESOURCE_BARRIER b {};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            r.list->ResourceBarrier(1, &b);
                            return;
                        }
                        for (auto* image : { depth[variant].Get(), motion[variant].Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        if (variant)
                        {
                            f.dispatch(uniform.Get(), uniformCb.Get(), 128, f.after.w, f.after.h);
                            for (auto* image : { privateDepth.Get(), privateMotion.Get() })
                                r.barrier(image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        }
                        f.dispatch(f.carrier.Get(), carrierCb[variant].Get(), 136 + variant * 8, f.after.w, f.after.h);
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        r.list->ResourceBarrier(1, &b);
                        if (variant)
                            for (auto* image : { privateDepth.Get(), privateMotion.Get() })
                                r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        for (auto* image : { depth[variant].Get(), motion[variant].Get() })
                            r.barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    };
                    const auto name = std::string(joint ? "joint" : "surface") +
                                      (privateDlss ? "-private-guides" : "-nr-only") + "-render" +
                                      std::to_string(renderH);
                    MeasureCandidate(
                        f.images, out, name.c_str(), 100, [&] { draw(0); }, [&] { draw(1); });
                }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
