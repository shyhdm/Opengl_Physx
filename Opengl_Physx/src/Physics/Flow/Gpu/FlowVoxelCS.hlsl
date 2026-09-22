#include "NvFlowShader.hlsli"
struct FlowVoxelParams
{
    float4x4 viewInverse;
    float4x4 projectionInverse;
    float4 minimum;
    float4 maximum;
    float4 cellSize;
    uint4 viewport;
    NvFlowSparseLevelParams level;
};
ConstantBuffer<FlowVoxelParams> params;
StructuredBuffer<uint> table;
Texture3D<float4> density;
Texture2D<float> sceneDepth;
Texture2D<float4> sceneColor;
RWTexture2D<float4> result;

float3 worldPoint(float2 uv, float depth)
{
    float4 p = mul(float4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0), params.projectionInverse);
    p = mul(p, params.viewInverse);
    return p.xyz / p.w;
}
float3 fireColor(float heat)
{
    float3 low = lerp(float3(1,.035,.005),float3(1,.32,.015),smoothstep(.05,.4,heat));
    float3 high = lerp(float3(1,.92,.32),float3(1,1,.92),smoothstep(.7,1,heat));
    return lerp(low,high,smoothstep(.35,.78,heat));
}
[numthreads(8,8,1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel=dispatchThreadID.xy;
    if(any(pixel>=params.viewport.xy)) return;
    float4 background=sceneColor[pixel];
    result[pixel]=background;
    float2 uv=(float2(pixel)+.5)/float2(params.viewport.xy);
    float3 origin=mul(float4(0,0,0,1),params.viewInverse).xyz;
    float3 direction=normalize(worldPoint(uv,1)-origin);
    float3 safeDirection=float3(abs(direction.x)<1e-8?1e-8:direction.x,
        abs(direction.y)<1e-8?1e-8:direction.y,abs(direction.z)<1e-8?1e-8:direction.z);
    float3 first=(params.minimum.xyz-origin)/safeDirection;
    float3 second=(params.maximum.xyz-origin)/safeDirection;
    float3 nearPoint=min(first,second),farPoint=max(first,second);
    float entry=max(max(nearPoint.x,nearPoint.y),max(nearPoint.z,0));
    float exit=min(min(farPoint.x,farPoint.y),farPoint.z);
    exit=min(exit,max(dot(worldPoint(uv,sceneDepth[pixel])-origin,direction),0));
    if(exit<=entry) return;
    float3 spacing=params.cellSize.xyz;
    int3 cell=int3(floor((origin+direction*(entry+min(spacing.x,min(spacing.y,spacing.z))*1e-4))/spacing));
    int3 advance=int3(sign(direction));
    float3 interval=abs(spacing/safeDirection);
    float3 nextHit=((float3(cell)+step(0.0,direction))*spacing-origin)/safeDirection;
    if(abs(direction.x)<1e-8) nextHit.x=interval.x=1e30;
    if(abs(direction.y)<1e-8) nextHit.y=interval.y=1e30;
    if(abs(direction.z)<1e-8) nextHit.z=interval.z=1e30;
    float3 normal=-direction;
    uint limit=uint(dot(ceil((params.maximum.xyz-params.minimum.xyz)/spacing),float3(1,1,1)))+6;
    for(uint i=0;i<limit && entry<exit;++i)
    {
        float boundary=min(nextHit.x,min(nextHit.y,nextHit.z));
        int4 real=NvFlowGlobalVirtualToReal(table,params.level,int4(cell,int(params.viewport.z)));
        float4 value=real.w!=0?density.Load(int4(real.xyz,0)):float4(0,0,0,0);
        if(min(boundary,exit)>entry+1e-7 && (value.w>.02 || value.x>.10))
        {
            float heat=1-exp(-max(value.x,0)*.35);
            float3 color=lerp(float3(.22,.22,.22),fireColor(heat),smoothstep(0,.25,heat));
            float lighting=.55+.45*max(dot(normal,normalize(float3(.4,.8,.6))),0);
            result[pixel]=float4(color*lighting,1);
            return;
        }
        entry=boundary;
        if(nextHit.x<=boundary) {cell.x+=advance.x;nextHit.x+=interval.x;normal=float3(-advance.x,0,0);}
        if(nextHit.y<=boundary) {cell.y+=advance.y;nextHit.y+=interval.y;normal=float3(0,-advance.y,0);}
        if(nextHit.z<=boundary) {cell.z+=advance.z;nextHit.z+=interval.z;normal=float3(0,0,-advance.z);}
    }
}
