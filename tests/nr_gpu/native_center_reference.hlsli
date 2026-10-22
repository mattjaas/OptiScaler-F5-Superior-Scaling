// Independent scalar identity-region specification. No production estimator/geometry include.
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
float ReferenceNativeAxisWeight(float pixel, uint axis)
{
    const float origin = gMotion.Load(int3(4, 0, 0))[axis];
    const float delta = pixel - origin;
    const bool right = delta >= 0.0;
    const float4 side = gMotion.Load(int3(right ? 6 : 5, 0, 0));
    const float4 packed = gMotion.Load(int3(right ? 8 : 7, 0, 0));
    if (packed[axis] >= 1.0 || side[axis + 2] >= 1.0)
        return 0.0;
    const float halfBand = side[axis] * side[axis + 2];
    return saturate((abs(delta) - halfBand + 4.5) * 0.25);
}
float3 ReferenceShapeNative(float3 edit, float2 packedUv)
{
    if (gGuideWidth == 0u)
        return edit;
    return gMvScaleX * edit + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, packedUv, 0).rgb;
}
#endif
float3 SuperiorGuidedEdit(float2 uv, float3 nativeGuide)
{
#if defined(DLSSNR_SUPERIOR_SKIP_GUIDED)
    float2 query = uv;
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    query = PackedReferenceUv(uv);
#endif
    const float3 edit = gModel.SampleLevel(gLinear, saturate(query), 0).rgb;
    return gGuideWidth != 0u ? gMvScaleX * edit + (gMvScaleY-gMvScaleX) *
        gSuperiorLow.SampleLevel(gLinear, query, 0).rgb : edit;
#elif defined(DLSSNR_SUPERIOR_FORCE_FULL_GUIDED)
    return ReferenceFullGuidedEdit(uv, nativeGuide, false);
#else
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    if (gGuideHeight == 0u)
    {
        const float2 packedUv = PackedReferenceUv(uv);
        float weight = 1.0;
        const float2 pixel = uv * float2(gWidth, gHeight);
        const float4 scale = gMotion.Load(int3(10, 0, 0));
        if (gModelWorkScale == 1.0 && scale.x == 1.0 && scale.y == 1.0)
        {
            const float x = ReferenceNativeAxisWeight(pixel.x, 0), y = ReferenceNativeAxisWeight(pixel.y, 1);
            const float wx=x*x*(3.0-2.0*x), wy=y*y*(3.0-2.0*y);
            weight = 1.0-(1.0-wx)*(1.0-wy);
        }
        if (weight == 0.0)
        {
            uint w,h;
            gSource.GetDimensions(w,h);
            const float4 center = gMotion.Load(int3(4, 0, 0));
            const int2 translation = int2(round(center.zw - center.xy));
            const int2 p = min(max(int2(pixel) + translation, int2(0,0)), int2(w-1,h-1));
            const float3 direct = gCompareMode == 1u ? gModel.SampleLevel(gLinear, packedUv, 0).rgb
                                                    : gModel.Load(int3(p,0)).rgb;
            return ReferenceShapeNative(direct, packedUv);
        }
        float3 raw = ReferenceFullGuidedEdit(uv, nativeGuide, true);
        if (weight < 1.0)
            raw = lerp(gModel.SampleLevel(gLinear, saturate(packedUv), 0).rgb, raw, weight);
        return ReferenceShapeNative(raw, packedUv);
    }
#endif
    return ReferenceFullGuidedEdit(uv, nativeGuide, false);
#endif
}
