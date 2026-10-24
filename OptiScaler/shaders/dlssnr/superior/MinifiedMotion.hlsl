// Minification-only surface correction, after the unchanged Depth-aware pass.
#define SPATIAL_GUIDES 1
#define CSMain UnusedSpatialMain
#include "../precompile/dlssnr_spatial.hlsl"
#undef CSMain
#include "SurfaceMotion.hlsli"

// Exact pixel edges, not a frame-wide compression ratio or a center derivative.
// MV and depth remain tied to the same point-sampled query surface. In particular,
// averaging all vectors in this footprint would mix foreground and background.
bool MotionMinifiedBounds(float2 workPixel, bool packed, out int2 first, out int2 last)
{
    const float2 scale = gMotionRect.zw / gNativeWorkSize.xy;
    const float2 a = (packed ? PwUnpack(workPixel - 0.5)
        : (workPixel - 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight))) * scale;
    const float2 b = (packed ? PwUnpack(workPixel + 0.5)
        : (workPixel + 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight))) * scale;
    const int2 lo = int2(gMotionRect.xy), hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    first = clamp(int2(floor(a)) + lo, lo, hi);
    last = clamp(int2(ceil(b)) - 1 + lo, first, hi);
    // Rounding the nonlinear map must not turn an effectively 1:1 region into a minifier.
    return any(b - a > 1.0001);
}

bool MotionMinifiedSurface(float2 workPixel, bool packed, int2 queryDepthPixel, float queryDepth,
                           float2 gradient, out float2 selected)
{
    int2 first, last;
    selected = 0.0;
    if (!MotionMinifiedBounds(workPixel, packed, first, last)) return false;

    // Search the complete covered lattice up to 8 texels per axis. Above that,
    // bounded stratification spans the entire footprint instead of clipping a
    // fixed neighborhood around its center (no unbounded per-pixel GPU loop).
    const int2 extent = last - first + 1;
    const int2 taps = min(extent, 8);
    bool found = false;
    float bestDistance = 3.402823466e+38;
    [loop] for (int y = 0; y < taps.y; ++y)
    {
        [loop] for (int x = 0; x < taps.x; ++x)
        {
            const int2 p = first + min(int2((float2(x, y) + 0.5) * float2(extent) / float2(taps)), extent - 1);
            const int2 dp = MotionDepthPixel(p);
            const float z = gSource1.Load(int3(dp, 0)).x;
            // De-trend sloping device depth, preserving reversed-Z symmetry.
            const float predicted = z + dot(gradient, float2(queryDepthPixel - dp));
            if (!isfinite(z) || abs(predicted - queryDepth) > MotionDepthTolerance(predicted, queryDepth)) continue;
            const float2 v = gSource2.Load(int3(p, 0)).xy;
            if (!all(isfinite(v))) continue;
            const float2 delta = float2(dp - queryDepthPixel) / (gDepthRect.zw / gMotionRect.zw);
            const float distance = dot(delta, delta);
            if (!found || distance < bestDistance)
            {
                found = true;
                bestDistance = distance;
                selected = v;
            }
        }
    }
    return found;
}




void CorrectMinifiedMotion(uint3 thread, bool packed)
{
    const uint index = thread.x + thread.y * uint(gPadding0.y);
    const uint left = uint(gPadding1.x), right = uint(gPadding1.y);
    const uint top = uint(gPadding1.z), bottom = uint(gPadding1.w);
    const uint topArea = top * gWidth, bottomArea = bottom * gWidth;
    const uint middleHeight = gHeight - top - bottom;
    const uint sideArea = middleHeight * (left + right);
    if (index >= topArea + bottomArea + sideArea) return;
    uint2 id;
    if (index < topArea) id = uint2(index % gWidth, index / gWidth);
    else if (index < topArea + bottomArea)
    {
        const uint j = index - topArea;
        id = uint2(j % gWidth, gHeight - bottom + j / gWidth);
    }
    else
    {
        const uint j = index - topArea - bottomArea, row = j / (left + right), col = j % (left + right);
        id = uint2(col < left ? col : gWidth - right + col - left, top + row);
    }
    const float2 workPixel = float2(id) + 0.5;
    const float2 nativePixel = packed ? PwUnpack(workPixel)
        : workPixel * (gNativeWorkSize.xy / float2(gWidth,gHeight));
    const float2 uv = nativePixel / gNativeWorkSize.xy;
    const int2 lo = int2(gMotionRect.xy), hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    const int2 mp = clamp(int2(gMotionRect.xy + uv * gMotionRect.zw), lo, hi);
    const int2 dp = clamp(int2(gDepthRect.xy + uv * gDepthRect.zw), int2(gDepthRect.xy),
                          int2(gDepthRect.xy + gDepthRect.zw) - 1);
    const float queryDepth = gSource1.Load(int3(dp,0)).x;
    const int2 center = MotionDepthPixel(mp);
    const float z = gSource1.Load(int3(center,0)).x;
    if (!isfinite(queryDepth) || !isfinite(z) || abs(z-queryDepth) <= MotionDepthTolerance(z,queryDepth)) return;
    const int2 a = MotionDepthPixel(clamp(mp-int2(1,0),lo,hi));
    const int2 b = MotionDepthPixel(clamp(mp+int2(1,0),lo,hi));
    const int2 c = MotionDepthPixel(clamp(mp-int2(0,1),lo,hi));
    const int2 d = MotionDepthPixel(clamp(mp+int2(0,1),lo,hi));
    const float za = gSource1.Load(int3(a,0)).x, zb = gSource1.Load(int3(b,0)).x;
    const float zc = gSource1.Load(int3(c,0)).x, zd = gSource1.Load(int3(d,0)).x;
    if (!isfinite(za) || !isfinite(zb) || !isfinite(zc) || !isfinite(zd)) return;
    const float2 gradient = float2(
        MotionMinmod((z-za)/max(center.x-a.x,1),(zb-z)/max(b.x-center.x,1)),
        MotionMinmod((z-zc)/max(center.y-c.y,1),(zd-z)/max(d.y-center.y,1)));
    const float prediction = z + dot(gradient,float2(dp-center));
    [branch] if (abs(prediction-queryDepth) > MotionDepthTolerance(prediction,queryDepth))
    {
        float2 raw;
        if (MotionMinifiedSurface(workPixel,packed,dp,queryDepth,gradient,raw))
        {
            const float2 v = raw * gMotionScale;
            const float2 motion = packed ? PwPack(nativePixel+v)-PwPack(nativePixel) : raw;
            gTarget1[id] = all(isfinite(motion)) ? motion : 0.0;
        }
    }
}
[numthreads(64,1,1)]
void SuperiorMinifiedGuidesMain(uint3 id : SV_DispatchThreadID) { CorrectMinifiedMotion(id,true); }
[numthreads(64,1,1)]
void SuperiorUniformMinifiedGuidesMain(uint3 id : SV_DispatchThreadID) { CorrectMinifiedMotion(id,false); }
