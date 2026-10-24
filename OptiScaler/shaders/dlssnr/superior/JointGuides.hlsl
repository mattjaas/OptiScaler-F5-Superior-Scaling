// Joint depth/MV reconstruction. Legacy point-depth PSOs remain available for A/B.
#define DLSSNR_DEPTH_QUERY_TYPE float2
#include "MinifiedMotion.hlsl"

float JointDepth(float2 nativePixel, out float2 query, out float2 gradient, out bool valid)
{
    const int2 lo=int2(gDepthRect.xy), hi=int2(gDepthRect.xy+gDepthRect.zw)-1;
    const float2 sample=gDepthRect.xy+(nativePixel/gNativeWorkSize.xy)*gDepthRect.zw;
    query=clamp(sample-0.5,float2(lo),float2(hi));
    const int2 p=clamp(int2(sample),lo,hi);
    const int2 a=clamp(p-int2(1,0),lo,hi), b=clamp(p+int2(1,0),lo,hi);
    const int2 c=clamp(p-int2(0,1),lo,hi), d=clamp(p+int2(0,1),lo,hi);
    const float z=gSource1.Load(int3(p,0)).x;
    const float za=gSource1.Load(int3(a,0)).x, zb=gSource1.Load(int3(b,0)).x;
    const float zc=gSource1.Load(int3(c,0)).x, zd=gSource1.Load(int3(d,0)).x;
    gradient=0.0;
    valid=isfinite(z);
    if (!valid) return 0.0;
    if (!isfinite(za)||!isfinite(zb)||!isfinite(zc)||!isfinite(zd)) return z;
    gradient=float2(MotionMinmod(z-za,zb-z),MotionMinmod(z-zc,zd-z));
    const float low=min(z,min(min(za,zb),min(zc,zd))), high=max(z,max(max(za,zb),max(zc,zd)));
    return clamp(z+dot(gradient,query-float2(p)),low,high);
}

#if defined(DLSSNR_JOINT_MINIFY)
// Preserve foreground coverage when several depth layers occupy one working pixel.
// On one local plane, transport depth to the query first so slope is not mistaken for an occluder.
bool JointReducedDepth(float2 workPixel, bool packed, float2 query, float2 gradient, inout float depth,
                       out int2 selected, out float2 selectedMotion)
{
    selectedMotion=0.0;
    selected=int2(floor(query+0.5));
    const float2 ratio=gDepthRect.zw/gNativeWorkSize.xy;
    const float2 a=(packed?PwUnpack(workPixel-0.5):(workPixel-0.5)*(gNativeWorkSize.xy/float2(gWidth,gHeight)))*ratio;
    const float2 b=(packed?PwUnpack(workPixel+0.5):(workPixel+0.5)*(gNativeWorkSize.xy/float2(gWidth,gHeight)))*ratio;
    if (all(b-a<=1.0001)) return false;
    const int2 lo=int2(gDepthRect.xy), hi=int2(gDepthRect.xy+gDepthRect.zw)-1;
    const int2 first=clamp(int2(floor(a))+lo,lo,hi), last=clamp(int2(ceil(b))-1+lo,first,hi);
    const int2 extent=last-first+1,taps=min(extent,8);
    float closest=depth;
    bool changed=false;
    [loop] for (int y=0;y<taps.y;++y)
    {
        [loop] for (int x=0;x<taps.x;++x)
        {
            const int2 p=first+min(int2((float2(x,y)+0.5)*float2(extent)/float2(taps)),extent-1);
            const float z=gSource1.Load(int3(p,0)).x;
            const float projected=z+dot(gradient,query-float2(p));
            const float tolerance=MotionDepthTolerance(projected,closest);
            const bool nearer=gUnused>0.5?projected>closest+tolerance:projected<closest-tolerance;
            if (isfinite(z)&&isfinite(projected)&&nearer)
            {
                // Do not introduce a foreground depth for which no nearby finite MV
                // represents that surface. Validate depth and motion as a pair.
                const float2 native=(float2(p)-gDepthRect.xy+0.5)*(gNativeWorkSize.xy/gDepthRect.zw);
                float2 q,g;
                bool finiteQuery;
                JointDepth(native,q,g,finiteQuery);
                const int2 ml=int2(gMotionRect.xy), mh=int2(gMotionRect.xy+gMotionRect.zw)-1;
                const int2 mp=clamp(int2(gMotionRect.xy+native*(gMotionRect.zw/gNativeWorkSize.xy)),ml,mh);
                bool represented=false;
                float2 pairMotion=0.0;
                [unroll] for (uint k=0;k<5;++k)
                {
                    const int2 delta=k==0?int2(0,0):k==1?int2(-1,0):k==2?int2(1,0):k==3?int2(0,-1):int2(0,1);
                    const int2 m=clamp(mp+delta,ml,mh);
                    const float2 dpos=gDepthRect.xy+((float2(m)-gMotionRect.xy+0.5)/gMotionRect.zw)*gDepthRect.zw-0.5;
                    const int2 base=int2(floor(dpos)); const float2 f=dpos-float2(base);
                    bool sameSurface=true;
                    [unroll] for (uint j=0;j<4;++j)
                    {
                        const int2 offset=int2(j&1u,j>>1u);
                        const float weight=(offset.x?f.x:1.0-f.x)*(offset.y?f.y:1.0-f.y);
                        const int2 d=clamp(base+offset,lo,hi);
                        const float mz=gSource1.Load(int3(d,0)).x;
                        const float predicted=mz+dot(g,float2(p-d));
                        // Only supporting depth texels may confirm which surface owns an MV.
                        if (weight>1e-4 && (!isfinite(mz)||abs(predicted-z)>MotionDepthTolerance(predicted,z))) sameSurface=false;
                    }
                    const float2 mv=gSource2.Load(int3(m,0)).xy;
                    if (!represented && sameSurface && all(isfinite(mv))) { represented=true; pairMotion=mv; }
                }
                if (represented) { closest=projected; depth=z; selected=p; selectedMotion=pairMotion; changed=true; }
            }
        }
    }
    return changed;
}
#endif


void StoreJointMotion(uint2 id, float2 nativePixel, float2 raw, bool packed)
{
    const float2 v=raw*gMotionScale;
    const float2 motion=packed?PwPack(nativePixel+v)-PwPack(nativePixel):raw;
    gTarget1[id]=all(isfinite(motion))?motion:0.0;
}
void JointGuides(uint3 id, bool packed)
{
    if (id.x>=gWidth||id.y>=gHeight) return;
    const float2 workPixel=float2(id.xy)+0.5;
    const float2 nativePixel=packed?PwUnpack(workPixel):workPixel*(gNativeWorkSize.xy/float2(gWidth,gHeight));
    float2 query,gradient;
    bool validDepth;
    const float depth=JointDepth(nativePixel,query,gradient,validDepth);
    const int2 lo=int2(gMotionRect.xy),hi=int2(gMotionRect.xy+gMotionRect.zw)-1;
    const int2 mp=clamp(int2(gMotionRect.xy+(nativePixel/gNativeWorkSize.xy)*gMotionRect.zw),lo,hi);
    const float2 raw=SuperiorSurfaceMotion(nativePixel,mp,query,validDepth?depth:asfloat(0x7fc00000u));
    gTarget0[id.xy]=validDepth&&isfinite(depth)?depth:0.0;
    StoreJointMotion(id.xy,nativePixel,raw,packed);
}
#if defined(DLSSNR_JOINT_MINIFY)
void CorrectJointGuides(uint3 thread, bool packed)
{
    const uint index=thread.x+thread.y*uint(gPadding0.y);
    const uint left=uint(gPadding1.x),right=uint(gPadding1.y),top=uint(gPadding1.z),bottom=uint(gPadding1.w);
    const uint topArea=top*gWidth,bottomArea=bottom*gWidth;
    const uint count=topArea+bottomArea+(gHeight-top-bottom)*(left+right);
    if (index>=count) return;
    uint2 id;
    if (index<topArea) id=uint2(index%gWidth,index/gWidth);
    else if (index<topArea+bottomArea)
    {
        const uint j=index-topArea; id=uint2(j%gWidth,gHeight-bottom+j/gWidth);
    }
    else
    {
        const uint j=index-topArea-bottomArea,row=j/(left+right),col=j%(left+right);
        id=uint2(col<left?col:gWidth-right+col-left,top+row);
    }
    const float2 workPixel=float2(id)+0.5;
    const float2 nativePixel=packed?PwUnpack(workPixel):workPixel*(gNativeWorkSize.xy/float2(gWidth,gHeight));
    float2 query,gradient;
    bool validDepth;
    float depth=JointDepth(nativePixel,query,gradient,validDepth);
    if (!validDepth) return;
    int2 selectedDepth;
    float2 raw;
    [branch] if (JointReducedDepth(workPixel,packed,query,gradient,depth,selectedDepth,raw))
    {
        gTarget0[id]=depth;
        StoreJointMotion(id,nativePixel,raw,packed);
        return;
    }
    const int2 lo=int2(gMotionRect.xy),hi=int2(gMotionRect.xy+gMotionRect.zw)-1;
    const int2 mp=clamp(int2(gMotionRect.xy+(nativePixel/gNativeWorkSize.xy)*gMotionRect.zw),lo,hi);
    const int2 dp=MotionDepthPixel(mp);
    const float z=gSource1.Load(int3(dp,0)).x;
    const float predicted=z+dot(gradient,query-float2(dp));
    [branch] if (isfinite(z)&&abs(predicted-depth)>MotionDepthTolerance(predicted,depth))
    {
        if (MotionMinifiedSurface(workPixel,packed,query,depth,gradient,raw)) StoreJointMotion(id,nativePixel,raw,packed);
    }
}
[numthreads(64,1,1)]
void SuperiorJointMinifiedGuidesMain(uint3 id:SV_DispatchThreadID) { CorrectJointGuides(id,true); }
[numthreads(64,1,1)]
void SuperiorUniformJointMinifiedGuidesMain(uint3 id:SV_DispatchThreadID) { CorrectJointGuides(id,false); }
#else
[numthreads(8,8,1)]
void SuperiorJointGuidesMain(uint3 id:SV_DispatchThreadID) { JointGuides(id,true); }
[numthreads(8,8,1)]
void SuperiorUniformJointGuidesMain(uint3 id:SV_DispatchThreadID) { JointGuides(id,false); }
#endif
