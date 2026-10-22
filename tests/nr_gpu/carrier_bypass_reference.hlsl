// Independent no-DLSS roundtrip specification, including clipping and intermediate carrier storage precision.
cbuffer Params : register(b0)
{
    uint mode, unused, width, height;
    float unused0, unused1;
    uint halfCarrier;
};
Texture2D<float4> proxy : register(t0);
Texture2D<float4> answer : register(t1);
RWTexture2D<float4> result : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 p : SV_DispatchThreadID)
{
    if (p.x >= width || p.y >= height) return;
    float3 a = proxy.Load(int3(p.xy,0)).rgb;
    float3 b = answer.Load(int3(p.xy,0)).rgb;
    for (uint c=0;c<3;++c)
    {
        if (!isfinite(a[c])) a[c]=0;
        if (!isfinite(b[c])) b[c]=a[c];
    }
    precise float3 delta = b-a;
    precise float3 encoded = 0.5+delta;
    float3 carrier = clamp(encoded, -1.0, 2.0);
    if (halfCarrier != 0u) carrier = f16tof32(f32tof16(carrier));
    result[p.xy] = float4(carrier-0.5,1);
}
