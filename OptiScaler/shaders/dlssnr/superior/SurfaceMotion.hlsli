// A spatial, slope-limited reconstruction; no previous-frame resources or invented flow.
float MotionMinmod(float a, float b)
{
    return a > 0.0 && b > 0.0 ? min(a, b) : a < 0.0 && b < 0.0 ? max(a, b) : 0.0;
}
float2 MotionLimitedSlope(float2 a, float2 b)
{
    float2 centered = 0.5 * (a + b);
    float2 limited = float2(MotionMinmod(a.x, b.x), MotionMinmod(a.y, b.y));
    // Use one limiter for the vector: do not reconstruct its components from different branches.
    float2 ratio = float2(abs(centered.x) > 1e-30 ? abs(limited.x / centered.x) : 1.0,
                          abs(centered.y) > 1e-30 ? abs(limited.y / centered.y) : 1.0);
    return centered * min(ratio.x, ratio.y);
}
int2 MotionDepthPixel(int2 motionPixel)
{
    float2 uv = (float2(motionPixel) - gMotionRect.xy + 0.5) / gMotionRect.zw;
    return clamp(int2(gDepthRect.xy + uv * gDepthRect.zw), int2(gDepthRect.xy),
                 int2(gDepthRect.xy + gDepthRect.zw) - 1);
}
float MotionDepthTolerance(float a, float b)
{
    // Device depth stays device depth. The comparison is invariant under reversed Z.
    return max(1e-6, max(abs(a), abs(b)) * 3.814697265625e-6);
}
float2 SuperiorSurfaceMotion(float2 nativePixel, int2 nearestPixel, int2 queryDepthPixel, float queryDepth)
{
    const int2 lo = int2(gMotionRect.xy), hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    int2 p[5];
    p[0] = nearestPixel;
    p[1] = clamp(nearestPixel + int2(-1, 0), lo, hi);
    p[2] = clamp(nearestPixel + int2( 1, 0), lo, hi);
    p[3] = clamp(nearestPixel + int2(0, -1), lo, hi);
    p[4] = clamp(nearestPixel + int2(0,  1), lo, hi);
    float2 v[5];
    bool valid[5];
    [unroll] for (uint i = 0; i < 5; ++i)
    {
        v[i] = gSource2.Load(int3(p[i], 0)).xy;
        valid[i] = all(isfinite(v[i]));
    }
    if (valid[0] && all(v[0] == v[1]) && all(v[0] == v[2]) && all(v[0] == v[3]) && all(v[0] == v[4]))
        return v[0];

    bool compatible[5];
    [unroll] for (uint k = 0; k < 5; ++k) compatible[k] = valid[k];
    if (gPadding0.x > 0.5 && isfinite(queryDepth))
    {
        int2 dp[5];
        float z[5];
        bool finiteDepth = true;
        [unroll] for (uint j = 0; j < 5; ++j)
        {
            dp[j] = MotionDepthPixel(p[j]);
            z[j] = gSource1.Load(int3(dp[j], 0)).x;
            finiteDepth = finiteDepth && isfinite(z[j]);
        }
        if (finiteDepth)
        {
            // Fit the local depth plane using limited one-sided slopes. At a step, the
            // same-surface side limits the slope to zero instead of absorbing the jump.
            const float zx = MotionMinmod((z[0]-z[1]) / max(dp[0].x-dp[1].x, 1),
                                           (z[2]-z[0]) / max(dp[2].x-dp[0].x, 1));
            const float zy = MotionMinmod((z[0]-z[3]) / max(dp[0].y-dp[3].y, 1),
                                           (z[4]-z[0]) / max(dp[4].y-dp[0].y, 1));
            const float2 gradient = float2(zx, zy);
            const float prediction = z[0] + dot(gradient, float2(queryDepthPixel - dp[0]));
            if (!valid[0] || abs(prediction - queryDepth) > MotionDepthTolerance(prediction, queryDepth))
            {
                // The query can belong to another surface when depth has more samples than
                // motion. Select an existing depth-matching vector; never average the layers.
                uint best = 0;
                float error = valid[0] ? abs(z[0] - queryDepth) : 3.402823466e+38;
                [unroll] for (uint n = 1; n < 5; ++n)
                    if (valid[n] && abs(z[n] - queryDepth) < error)
                    {
                        error = abs(z[n] - queryDepth);
                        best = n;
                    }
                return valid[best] ? v[best] : 0.0;
            }
            [unroll] for (uint t = 1; t < 5; ++t)
            {
                const float expected = z[0] + dot(gradient, float2(dp[t] - dp[0]));
                compatible[t] = valid[t] && abs(expected - z[t]) <= MotionDepthTolerance(expected, z[t]);
            }
        }
        // Invalid depth is not evidence of another surface: retain the motion-only limiter.
    }
    if (!valid[0]) return 0.0;
    float2 sx = compatible[1] && compatible[2] ? MotionLimitedSlope(v[0]-v[1], v[2]-v[0]) : 0.0;
    float2 sy = compatible[3] && compatible[4] ? MotionLimitedSlope(v[0]-v[3], v[4]-v[0]) : 0.0;
    precise float2 position = nativePixel * (gMotionRect.zw / gNativeWorkSize.xy) - 0.5;
    float2 offset = position - float2(nearestPixel - lo);
    float2 result = v[0] + sx * offset.x + sy * offset.y;
    return all(isfinite(result)) ? result : v[0];
}
