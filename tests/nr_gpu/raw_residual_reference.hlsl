// Independent subtraction/sanitization specification, no production include.
cbuffer Params : register(b0) { uint mode, unused, width, height; };
Texture2D<float4> source : register(t0);
Texture2D<float4> model : register(t1);
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void CSMain(uint3 p : SV_DispatchThreadID)
{
    if (p.x >= width || p.y >= height) return;
    float3 a=source.Load(int3(p.xy,0)).rgb, b=model.Load(int3(p.xy,0)).rgb;
    for(uint c=0;c<3;++c)
    {
        if(!isfinite(a[c]))a[c]=0;
        if(!isfinite(b[c]))b[c]=a[c];
    }
    precise float3 result=b-a;
    for(uint i=0;i<3;++i)if(!isfinite(result[i]))result[i]=0;
    output[p.xy]=float4(result,1);
}
