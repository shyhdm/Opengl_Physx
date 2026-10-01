uniform vec3 interiorPosition;
uniform float interiorPower;
uniform vec3 sunDirection;
uniform mat4 view,projection,inverseProjection,inverseView;
uniform float spacing,foamOpacity;
uniform vec2 resolution;
uniform sampler2D waterDepth,sceneDepth,sceneColor;
uniform vec3 backgroundColor;
uniform samplerCube environmentMap;
#ifdef VERTEX_SHADER
layout(location=0) in vec4 positionLife;
layout(location=1) in vec4 velocityType;
layout(location=2) in vec4 lifetimeAge;
out vec2 local;
flat out vec3 center;
flat out float radius,fade,type;
flat out vec2 along,across;
void main(){
    local=vec2((gl_VertexID&1)==0?-1:1,(gl_VertexID&2)==0?-1:1);
    type=velocityType.w;radius=spacing*(type<.5?.035:(type<1.5?.18:.12));
    radius*=mix(.8,1.2,lifetimeAge.z);
    center=(view*vec4(positionLife.xyz,1)).xyz;
    float life=clamp(positionLife.w/max(lifetimeAge.x,.001),0,1);
    // FleX diffuse sprites spread with age and conserve opacity over their area.
    float spread=type<.5?1.0:mix(1.7,1.0,life);
    radius*=spread;
    fade=smoothstep(0,.06,lifetimeAge.y)*min(1.0,positionLife.w*.5)/(spread*spread);
    if(positionLife.w<=0 || -center.z<=radius*2){gl_Position=vec4(2,2,2,1);fade=0;return;}
    vec4 clip=projection*vec4(center,1);
    vec2 velocity=(mat3(view)*velocityType.xyz).xy;
    float speed=length(velocity);
    along=speed>1e-5?velocity/speed:vec2(0,1);across=vec2(along.y,-along.x);
    // A 1/60 s shutter, as in the FleX demo; cap trails near impacts.
    float longRadius=max(radius,min(speed/60.0,radius*6));
    fade*=radius/longRadius;
    vec2 projectionScale=vec2(projection[0][0],projection[1][1])/(-center.z);
    vec2 minorAxis=across*radius*projectionScale,majorAxis=along*longRadius*projectionScale;
    float minorPixels=length(minorAxis*resolution*.5),majorPixels=length(majorAxis*resolution*.5);
    float minorScale=max(1.0,.8/max(minorPixels,1e-6)),majorScale=max(1.0,.8/max(majorPixels,1e-6));
    fade/=minorScale*majorScale;
    gl_Position=vec4(clip.xy/clip.w+local.x*minorAxis*minorScale+local.y*majorAxis*majorScale,clip.z/clip.w,1);
}
#endif
#ifdef FRAGMENT_SHADER
in vec2 local;flat in vec3 center;flat in float radius,fade,type;
flat in vec2 along,across;
layout(location=0) out vec4 color;
void main(){
    float rr=dot(local,local);if(rr>=1 || fade<=0)discard;
    vec2 uv=gl_FragCoord.xy/resolution;float chord=2*radius*sqrt(1-rr),depth=-center.z-chord*.5;
    vec4 opaque=inverseProjection*vec4(uv*2-1,texture(sceneDepth,uv).r*2-1,1);
    float solid=-opaque.z/opaque.w;if(depth>=solid)discard;
    float water=texture(waterDepth,uv).r;
    float submersion=water>0?max(0,depth-water):0;
    float edge=1-smoothstep(max(0.0,1-fwidth(rr)*1.5),1.0,rr);
    // Flex diffuse shading uses a squared radial falloff, not an opaque disc.
    float soft=(1-rr)*(1-rr);
    float alpha=foamOpacity*soft*fade*edge;
    alpha*=exp(-submersion/max(spacing*4,.001))*smoothstep(0,radius,solid-depth);
    if(type>=.5 && alpha<.001)discard;
    if(type<.5){
        vec3 n=normalize(vec3(across*local.x+along*local.y,sqrt(max(0,1-rr))));
        vec3 incident=normalize(center),refracted=refract(incident,n,1/1.333);
        vec2 offset=(refracted.xy-incident.xy)*vec2(projection[0][0],projection[1][1])*chord/max(depth,.001)*.5;
        vec2 q=clamp(uv+offset,.5/resolution,1-.5/resolution);
        vec4 target=inverseProjection*vec4(q*2-1,texture(sceneDepth,q).r*2-1,1);
        float targetZ=-target.z/target.w;
        if(targetZ<=depth)q=uv;
        vec3 transmitted=texture(sceneColor,q).rgb*exp(-chord*vec3(.624,.156,.078));
        float fresnel=.02037+.97963*pow(1-clamp(dot(-incident,n),0,1),5);
        vec3 light=normalize(mat3(view)*normalize(sunDirection));
        float highlight=pow(max(0,dot(n,normalize(light-incident))),100)*.5;
        if(interiorPower>0){
            vec3 delta=(view*vec4(interiorPosition,1)).xyz-center;
            vec3 localL=delta*inversesqrt(max(dot(delta,delta),1e-8));
            vec3 localH=localL-incident;localH*=inversesqrt(max(dot(localH,localH),1e-8));
            highlight+=pow(max(dot(n,localH),0.0),64.0)*max(dot(n,localL),0.0)*interiorPower/(1.0+.06*dot(delta,delta));
        }
        float coverage=clamp(fade*edge*smoothstep(0,radius,solid-depth),0,1);
        coverage*=exp(-submersion/max(spacing*4,.001));
        color=vec4((mix(transmitted,texture(environmentMap,mat3(inverseView)*reflect(incident,n)).rgb,fresnel)+vec3(highlight))*coverage,coverage);return;
    }
    vec3 tint=mix(vec3(.96,.98,1),vec3(.55,.78,.85),1-exp(-submersion/max(spacing*3,.001)));
    color=vec4(tint*alpha,alpha);
}
#endif
