#pragma once
#include "packed_fixture.h"
#include "interpass_timing_fixture.h"
#include <functional>

constexpr const char* CandidateDirectory = "artifacts/validation/four-candidates";

// Record a whole ABBA block in one submission: warmup and samples stay resident on GPU,
// avoiding idle gaps between tiny command lists that otherwise let clocks oscillate.
inline void MeasureCandidate(LowFixture& f, std::ofstream& out, const char* name, int scale,
                             const std::function<void()>& base, const std::function<void()>& candidate,
                             bool screen = false)
{
    auto& r = f.r;
    const unsigned count = screen ? 24 : 48, blocks = screen ? 3 : 8;
    D3D12_QUERY_HEAP_DESC q {};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = count * 8;
    ComPtr<ID3D12QueryHeap> queries;
    check(r.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)));
    auto times = r.buffer(q.Count * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (unsigned block = 0; block < blocks; ++block)
    {
        r.begin();
        r.list->SetComputeRootSignature(r.root.Get());
        ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
        r.list->SetDescriptorHeaps(1, heaps);
        for (unsigned quartet = 0; quartet < count; ++quartet)
            for (unsigned pos = 0; pos < 4; ++pos)
            {
                bool changed = (pos == 1 || pos == 2) != (block % 2 != 0);
                unsigned index = quartet * 8 + pos * 2;
                r.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
                for (unsigned n = 0; n < 4; ++n)
                    (changed ? candidate : base)();
                r.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index + 1);
            }
        r.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, q.Count, times.Get(), 0);
        r.finish();
        UINT64* t;
        check(times->Map(0, nullptr, reinterpret_cast<void**>(&t)));
        for (unsigned quartet = 8; quartet < count; ++quartet)
            for (unsigned pos = 0; pos < 4; ++pos)
            {
                bool changed = (pos == 1 || pos == 2) != (block % 2 != 0);
                unsigned index = quartet * 8 + pos * 2;
                out << name << ',' << scale << ',' << (changed ? "candidate" : "base") << ',' << block << ','
                    << quartet - 8 << ',' << pos << ',' << double(t[index + 1] - t[index]) * 1000 / f.frequency / 4
                    << '\n';
            }
        times->Unmap(0, nullptr);
    }
    out.flush();
    std::cout << name << " P" << scale << " complete\n" << std::flush;
}

inline ComPtr<ID3D12Resource>
CachedWarp(Runner& r, const DlssNr::Spatial::Layout& layout,
           const std::string& shaderDirectory = "OptiScaler/shaders/dlssnr/superior/precompile")
{
    auto metadata = UploadWarp(r, layout);
    auto d = metadata->GetDesc();
    d.Width = std::max(11u, std::max(layout.nativeW, layout.nativeH));
    d.Height = 3;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto result = r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto pso = r.pipeline(shaderDirectory, "host_warp_cache");
    HostConstants c {};
    c.Width = layout.nativeW;
    c.Height = layout.nativeH;
    auto cb = r.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data;
    check(cb->Map(0, nullptr, &data));
    std::memcpy(data, &c, 256);
    cb->Unmap(0, nullptr);
    auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = d.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(metadata.Get(), &srv, { cpu.ptr + 3 * r.stride });
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = d.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    r.device->CreateUnorderedAccessView(result.Get(), nullptr, &uav, { cpu.ptr + 6 * r.stride });
    r.begin();
    r.list->SetComputeRootSignature(r.root.Get());
    ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
    r.list->SetDescriptorHeaps(1, heaps);
    r.list->SetComputeRootDescriptorTable(0, r.heap->GetGPUDescriptorHandleForHeapStart());
    r.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
    r.list->SetPipelineState(pso.Get());
    r.list->Dispatch((UINT(d.Width) + 7) / 8, 1, 1);
    r.barrier(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    r.finish();
    return result;
}

inline std::vector<uint32_t> ReadTexture(Runner& r, ID3D12Resource* image, D3D12_RESOURCE_STATES state)
{
    const auto d = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 bytes, rowBytes;
    r.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, &rowBytes, &bytes);
    auto readback = r.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    r.begin();
    r.barrier(image, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from {}, to {};
    from.pResource = image;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = fp;
    r.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    r.barrier(image, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    r.finish();
    char* data;
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)));
    std::vector<uint32_t> words(size_t(rowBytes) * d.Height / 4);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(words.data() + size_t(y) * rowBytes / 4, data + fp.Offset + y * fp.Footprint.RowPitch,
                    size_t(rowBytes));
    readback->Unmap(0, nullptr);
    return words;
}
