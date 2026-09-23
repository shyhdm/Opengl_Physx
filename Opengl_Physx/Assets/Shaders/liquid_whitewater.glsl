uniform mat4 view,projection,inverseProjection;
uniform float spacing;
uniform vec2 resolution;
uniform sampler2D waterDepth,sceneDepth;
#ifdef VERTEX_SHADER
layout(location=0) in vec4 positionLife;
layout(location=1) in vec4 velocityType;
layout(location=2) in vec4 lifetimeAge;
out vec2 local;
flat out vec3 center;
flat out float radius,fade,type;
void main(){
    local=vec2((gl_VertexID&1)==0?-1:1,(gl_VertexID&2)==0?-1:1);
    type=velocityType.w;radius=spacing*(type<.5?.035:(type<1.5?.06:.045));
    radius*=mix(.8,1.2,lifetimeAge.z);
    center=(view*vec4(positionLife.xyz,1)).xyz;
    fade=smoothstep(0,.05,lifetimeAge.y)*smoothstep(0,.3,positionLife.w/max(lifetimeAge.x,.001));
    if(positionLife.w<=0 || -center.z<=radius*2){gl_Position=vec4(2,2,2,1);fade=0;return;}
    vec4 clip=projection*vec4(center,1);
    vec2 extent=vec2(projection[0][0],projection[1][1])*radius/(-center.z);
    vec2 minimum=1.6/resolution;float coverage=min(1,(extent.x*extent.y)/(minimum.x*minimum.y));fade*=coverage;
    gl_Position=vec4(clip.xy/clip.w+local*max(extent,minimum),clip.z/clip.w,1);
}
#endif
#ifdef FRAGMENT_SHADER
in vec2 local;flat in vec3 center;flat in float radius,fade,type;
layout(location=0) out vec4 color;
void main(){
    float rr=dot(local,local);if(rr>=1 || fade<=0)discard;
    vec2 uv=gl_FragCoord.xy/resolution;float chord=2*radius*sqrt(1-rr),depth=-center.z-chord*.5;
    vec4 opaque=inverseProjection*vec4(uv*2-1,texture(sceneDepth,uv).r*2-1,1);
    float solid=-opaque.z/opaque.w;if(depth>=solid)discard;
    float water=texture(waterDepth,uv).r;
    float submersion=water>0?max(0,depth-water):0;
    float edge=1-smoothstep(max(0.0,1-fwidth(rr)*1.5),1.0,rr);
    float alpha=(1-exp(-chord*24/max(spacing,.001)))*fade*edge;
    alpha*=exp(-submersion/max(spacing*4,.001))*smoothstep(0,radius,solid-depth);
    if(alpha<.002)discard;
    vec3 tint=mix(vec3(.96,.98,1),vec3(.55,.78,.85),1-exp(-submersion/max(spacing*3,.001)));
    color=vec4(tint*alpha,alpha);
}
#endif
