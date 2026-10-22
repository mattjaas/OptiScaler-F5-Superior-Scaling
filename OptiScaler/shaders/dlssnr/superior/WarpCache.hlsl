// Runs once per generation/layout; all entries use the consumer's exact FP32 evaluation.
cbuffer Params : register(b0)
{
    uint gMode; float gUnused; uint gWidth, gHeight;
};
Texture2D<float4> gMotion : register(t3);
RWTexture2D<float4> gTarget : register(u0);
#include "WarpMap.hlsli"
float4 NativeAxisEntry(uint index, uint axis)
{
    const float extent = axis == 0u ? float(gWidth) : float(gHeight);
    const float query = (float(index) + 0.5) / extent;
    const float2 mapped = SuperiorArithmeticPackedUv(axis == 0u ? float2(query, 0) : float2(0, query));
    const float pixel = query * extent;
    const float4 centers = gBandCenter;
    const float delta = pixel - centers[axis];
    const bool positive = delta >= 0.0;
    const float4 side = gMotion.Load(int3(positive ? 6 : 5, 0, 0));
    const float4 work = gMotion.Load(int3(positive ? 8 : 7, 0, 0));
    const float distance = abs(delta) - side[axis + 2] * side[axis] + 4.5;
    const float t = work[axis] >= 1.0 || side[axis + 2] >= 1.0 ? 0.0 : saturate(distance * 0.25);
    const float smooth = t * t * (3.0 - 2.0 * t);
    const int direct = int(pixel) + int(round(centers[axis + 2] - centers[axis]));
    // R remains the exact UV used by existing PSOs. Spare channels cache native-mask work once per layout.
    return float4(mapped[axis], smooth, float(direct), gWorkScale[axis]);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.y == 0u && id.x < 11u)
        gTarget[id.xy] = gMotion.Load(int3(id.xy, 0));
    if (id.y == 1u && id.x < gWidth)
        gTarget[id.xy] = NativeAxisEntry(id.x, 0u);
    if (id.y == 2u && id.x < gHeight)
        gTarget[id.xy] = NativeAxisEntry(id.x, 1u);
}
