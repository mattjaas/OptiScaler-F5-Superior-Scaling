// Diagnostic carrier bypass. Separate entry point: frozen v59 shaders/ABI are unchanged.
#include "precompile/dlssnr.hlsl"
[numthreads(8, 8, 1)]
void RawResidualMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    const int3 p = int3(id.xy, 0);
    const float3 proxy = SanitizeFinite3(gSource.Load(p).rgb, 0.0);
    const float3 answer = SanitizeFinite3(gModel.Load(p).rgb, proxy);
    precise float3 edit = answer - proxy;
    gTarget[id.xy] = float4(SanitizeFinite3(edit, 0.0), 1.0);
}
