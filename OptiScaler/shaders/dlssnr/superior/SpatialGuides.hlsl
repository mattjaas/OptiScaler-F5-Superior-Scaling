// Superior's continuous motion field. The original Compress shaders remain unchanged.
#define SPATIAL_GUIDES 1
#define CSMain UnusedSpatialMain
#include "../precompile/dlssnr_spatial.hlsl"
#undef CSMain

float2 SuperiorMotion(float2 nativePixel, int2 nearestPixel)
{
    const int2 lo = int2(gMotionRect.xy);
    const int2 hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    const float2 nearest = gSource2.Load(int3(nearestPixel, 0)).xy;
    // Position on the SOURCE lattice, including its half-texel phase and valid origin.
    precise float2 position = nativePixel * (gMotionRect.zw / gNativeWorkSize.xy) - 0.5;
    const int2 base = int2(floor(position));
    const float2 f = position - float2(base);
    const float2 a = gSource2.Load(int3(clamp(base + lo, lo, hi), 0)).xy;
    const float2 b = gSource2.Load(int3(clamp(base + int2(1, 0) + lo, lo, hi), 0)).xy;
    const float2 c = gSource2.Load(int3(clamp(base + int2(0, 1) + lo, lo, hi), 0)).xy;
    const float2 d = gSource2.Load(int3(clamp(base + int2(1, 1) + lo, lo, hi), 0)).xy;
    // Preserve constant fields exactly; never mix non-finite vectors into a valid neighbor.
    if (all(a == nearest) && all(b == nearest) && all(c == nearest) && all(d == nearest))
        return nearest;
    if (!all(isfinite(a)) || !all(isfinite(b)) || !all(isfinite(c)) || !all(isfinite(d)))
        return all(isfinite(nearest)) ? nearest : 0.0;
    // Across a motion discontinuity retain the old point sample rather than mix different surfaces.
    const float2 span = max(max(abs(a - nearest), abs(b - nearest)),
                            max(abs(c - nearest), abs(d - nearest))) * abs(gMotionScale);
    if (any(span > 1.0))
        return nearest;
    precise float2 top = a + (b - a) * f.x;
    precise float2 bottom = c + (d - c) * f.x;
    return top + (bottom - top) * f.y;
}

[numthreads(8, 8, 1)]
void SuperiorSpatialGuidesMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    const float2 nativePixel = PwUnpack(float2(id.xy) + 0.5);
    const float2 uv = nativePixel / gNativeWorkSize.xy;
    const int2 depthLo = int2(gDepthRect.xy), motionLo = int2(gMotionRect.xy);
    const int2 depthHi = int2(gDepthRect.xy + gDepthRect.zw) - 1;
    const int2 motionHi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    const int2 dp = clamp(int2(gDepthRect.xy + uv * gDepthRect.zw), depthLo, depthHi);
    const int2 mp = clamp(int2(gMotionRect.xy + uv * gMotionRect.zw), motionLo, motionHi);
    const float2 v = SuperiorMotion(nativePixel, mp) * gMotionScale;
    gTarget0[id.xy] = gSource1.Load(int3(dp, 0)).x;
    gTarget1[id.xy] = PwPack(nativePixel + v) - PwPack(nativePixel);
}
