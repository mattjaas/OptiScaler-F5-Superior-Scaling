// v59's radius-one estimator, folded into janblade's existing final composition.
// t0: working proxy, t1: decoded signed DLSS residual (or raw NR with GuideHeight=1), t5: Low64 field.
// No exposure, colour encoding or tone-mapping logic belongs in this include.
Texture2D<float4> gSuperiorLow : register(t5);
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
#include "WarpMap.hlsli"
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
static uint2 gSuperiorPixel;
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
static float4 gNativeAxisX, gNativeAxisY;
#endif
#endif
float2 SuperiorPackedUv(float2 uv)
{
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
    gNativeAxisX = gMotion.Load(int3(gSuperiorPixel.x, 1, 0));
    gNativeAxisY = gMotion.Load(int3(gSuperiorPixel.y, 2, 0));
    return float2(gNativeAxisX.x, gNativeAxisY.x);
#else
    return float2(gMotion.Load(int3(gSuperiorPixel.x, 1, 0)).x,
                  gMotion.Load(int3(gSuperiorPixel.y, 2, 0)).x);
#endif
#else
    return SuperiorArithmeticPackedUv(uv);
#endif
}
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
float SuperiorNativeGuidedWeight(float2 nativePixel)
{
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
    return 1.0 - (1.0 - gNativeAxisX.y) * (1.0 - gNativeAxisY.y);
#else
    const float2 delta = nativePixel - gBandCenter.xy;
    const float2 span = float2(delta.x >= 0.0 ? gSidePos.x : gSideNeg.x,
                               delta.y >= 0.0 ? gSidePos.y : gSideNeg.y);
    const float2 center = float2(delta.x >= 0.0 ? gSidePos.z : gSideNeg.z,
                                 delta.y >= 0.0 ? gSidePos.w : gSideNeg.w);
    const float2 work = float2(delta.x >= 0.0 ? gSideWorkPos.x : gSideWorkNeg.x,
                               delta.y >= 0.0 ? gSideWorkPos.y : gSideWorkNeg.y);
    // Start blending four native pixels before a footprint touches compression.
    // Every compressed footprint therefore uses the full guided filter; the inner centre stays direct.
    const float2 distance = abs(delta) - center * span + 4.5;
    const float2 weight = float2(work.x >= 1.0 || center.x >= 1.0 ? 0.0 : saturate(distance.x * 0.25),
                                 work.y >= 1.0 || center.y >= 1.0 ? 0.0 : saturate(distance.y * 0.25));
    const float2 smoothWeight = weight * weight * (3.0 - 2.0 * weight);
    // A smooth union also avoids a diagonal max(x,y) crease at the centre rectangle's corners.
    return 1.0 - (1.0 - smoothWeight.x) * (1.0 - smoothWeight.y);
#endif
}
#endif
#endif
float3 SuperiorShapeEdit(float3 edit, float2 uvq)
{
    return gGuideWidth != 0u
        ? gMvScaleX * edit + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb
        : edit;
}
float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)
{
    // Non-temporal fallback (e.g. an above-native ordinary grid with reduced packed NR).
    // Packed temporal reconstruction instead uses the direct warped query below.
    // Non-finite NR or proxy values (an Inf from a pass) must not reach the frame. SanitizeFinite3 returns finite
    // values unchanged, so finite results are bit-identical.
    if (gGuideHeight != 0u)
        return SuperiorShapeEdit(SanitizeFinite3(gModel.SampleLevel(gLinear, uvq, 0).rgb -
                                                 gSource.SampleLevel(gLinear, uvq, 0).rgb, 0.0), uvq);
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
    float guidedWeight = 1.0;
#endif
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    // Map the native query directly to the packed lattice; no bilinear intermediate images.
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
    const float2 nativePixel = uvq * float2(gWidth, gHeight);
#endif
    uvq = SuperiorPackedUv(uvq);
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
    const bool nativeIdentity = gNativeAxisX.w == 1.0 && gNativeAxisY.w == 1.0;
#else
    const bool nativeIdentity = all(gWorkScale.xy == 1.0);
#endif
    [branch] if (gModelWorkScale == 1.0 && nativeIdentity)
    {
        guidedWeight = SuperiorNativeGuidedWeight(nativePixel);
        [branch] if (guidedWeight == 0.0)
        {
            // The native packer moves this region by whole texels. Keep the stabilized NR edit,
            // frequency shaping and composition; bypass only the nine-tap guided reconstruction.
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
            const int2 p = clamp(int2(gNativeAxisX.z, gNativeAxisY.z), int2(0, 0), int2(srcW, srcH) - 1);
#else
            const int2 shift = int2(round(gBandCenter.zw - gBandCenter.xy));
            const int2 p = clamp(int2(nativePixel) + shift, int2(0, 0), int2(srcW, srcH) - 1);
#endif
            const float3 direct = gCompareMode == 1u ? gModel.SampleLevel(gLinear, uvq, 0).rgb
                                                    : gModel.Load(int3(p, 0)).rgb;
            return SuperiorShapeEdit(SanitizeFinite3(direct, 0.0), uvq);
        }
    }
#endif
#endif
    const float3 fallback = SanitizeFinite3(gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb, 0.0);
    const float rangeSigma = max(abs(0.015), 1e-5);
    const float spatialSigma = max(abs(3.0), 1e-4);
    const float invRange2 = 1.0 / (rangeSigma * rangeSigma);
    const float invSpatial2 = 1.0 / (spatialSigma * spatialSigma);
    const float gaussianExp2 = 0.7213475204444817;
    const float2 sourcePos = uvq * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(sourcePos + 0.5));
    float3 weighted = 0.0;
    float weightSum = 0.0;
#if defined(DLSSNR_SUPERIOR_HOST_SDR)
    [loop]
#else
    [unroll]
#endif
    for (int oy = -1; oy <= 1; ++oy)
    {
#if defined(DLSSNR_SUPERIOR_HOST_SDR)
        [loop]
#else
        [unroll]
#endif
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 p = clamp(base + int2(ox, oy), int2(0, 0), int2((int)srcW - 1, (int)srcH - 1));
            // A non-finite proxy tap gets ~zero range weight, a non-finite residual tap contributes nothing.
            const float3 proxy = SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, 1e4);
            const float3 residual = SanitizeFinite3(gModel.Load(int3(p, 0)).rgb, 0.0);
            const float3 colourDelta = proxy - nativeGuide;
            const float rangeDistance2 = dot(colourDelta, colourDelta) * (1.0 / 3.0);
            const float2 spatialDelta = float2(p) - sourcePos;
            const float spatialDistance2 = dot(spatialDelta, spatialDelta);
            const float wRange = exp2(-gaussianExp2 * rangeDistance2 * invRange2);
            const float wSpatial = exp2(-gaussianExp2 * spatialDistance2 * invSpatial2);
            const float w = wRange * wSpatial;
            weighted += residual * w;
            weightSum += w;
        }
    }
    float3 edit = weightSum > 1e-8 ? weighted / weightSum : fallback;
#if defined(DLSSNR_SUPERIOR_NATIVE_CENTER)
    [branch] if (guidedWeight < 1.0)
        edit = lerp(fallback, edit, guidedWeight);
#endif
    return SuperiorShapeEdit(edit, uvq);
}
