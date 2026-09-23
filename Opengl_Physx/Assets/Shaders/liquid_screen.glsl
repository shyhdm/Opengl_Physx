#if defined(FRAGMENT_SHADER) && defined(PASS_DEPTH)
#extension GL_ARB_conservative_depth : require
layout(depth_greater) out float gl_FragDepth;
#endif
uniform mat4 projection,inverseProjection,view,inverseView;
uniform vec2 resolution;
uniform float radius;
uniform vec3 waterColor,waterThinColor;
uniform vec3 sceneBackgroundColor;
uniform vec2 colorTransition;
uniform float absorptionStrength,reflectionStrength,refractionStrength,thicknessStrength,smoothRadius,smoothSharpness,depthRejection;
const float emptyDepth=10000000.0;
#if defined(PASS_DEPTH) || defined(PASS_THICKNESS)
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
layout(location=1) in vec4 velocity;
flat out vec3 center;
void main(){
    center=(view*vec4(position.xyz,1)).xyz;
    vec2 corner=vec2((gl_VertexID&1)==0?-1:1,(gl_VertexID&2)==0?-1:1);
    float z=-center.z;
    float nearPlane=projection[3][2]/(projection[2][2]-1.0);
    if(z+radius<=nearPlane){gl_Position=vec4(2,2,2,1);return;}
    float nearZ=max(z-radius,nearPlane),farZ=z+radius;
    vec2 low,high;
    if(z>radius*1.01){
        vec2 extent=radius*sqrt(max(center.xy*center.xy+z*z-radius*radius,vec2(0)));
        float denominator=z*z-radius*radius;
        low=(center.xy*z-extent)/denominator;
        high=(center.xy*z+extent)/denominator;
    }else{
        low=min((center.xy-radius)/nearZ,(center.xy-radius)/farZ);
        high=max((center.xy+radius)/nearZ,(center.xy+radius)/farZ);
    }
    vec2 scale=vec2(projection[0][0],projection[1][1]);low*=scale;high*=scale;
    if(any(greaterThan(low,vec2(1)))||any(lessThan(high,vec2(-1)))){gl_Position=vec4(2,2,2,1);return;}
    low=max(low,vec2(-1));high=min(high,vec2(1));
    vec2 screen=mix(low,high,corner*.5+.5);
    vec4 nearest=projection*vec4(0,0,-nearZ,1);
    gl_Position=vec4(screen,clamp(nearest.z/nearest.w,-1,1),1);
}
#endif
#ifdef FRAGMENT_SHADER
flat in vec3 center;
layout(location=0) out vec4 result;
uniform sampler2D sceneDepth;
void main(){
    vec2 uv=gl_FragCoord.xy/resolution;
    vec4 ray4=inverseProjection*vec4(uv*2-1,1,1);
    vec3 ray=normalize(ray4.xyz/ray4.w);
    float b=dot(ray,center),c=dot(center,center)-radius*radius;
    float discriminant=b*b-c;if(discriminant<=0)discard;
    float root=sqrt(discriminant),front=b-root,back=b+root;
    vec4 near4=inverseProjection*vec4(uv*2-1,-1,1);
    float nearT=length(near4.xyz/near4.w);
    front=max(front,nearT);if(back<=front)discard;
    vec4 opaque=inverseProjection*vec4(uv*2-1,texture(sceneDepth,uv).r*2-1,1);
    float sceneT=length(opaque.xyz/opaque.w);
    if(front>=sceneT)discard;
#ifdef PASS_DEPTH
    vec4 clip=projection*vec4(ray*front,1);
    gl_FragDepth=max(gl_FragCoord.z,clamp(clip.z/clip.w*.5+.5,0,1));
    result=vec4(front,0,0,1);
#else
    float segment=max(0,min(back,sceneT)-front);
    float radial=clamp(1-discriminant/(radius*radius),0,1);
    float weight=(exp(-2*radial)-exp(-2.0))/(1-exp(-2.0));
    result=vec4(segment*.46*weight,0,0,1);
#endif
}
#endif
#else
#ifdef VERTEX_SHADER
out vec2 uv;
void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2-1,0,1);}
#endif
#ifdef FRAGMENT_SHADER
in vec2 uv;layout(location=0) out vec4 result;
uniform sampler2D waterDepth,waterThickness,surfaceNormals,sceneColor,sceneDepth;
uniform vec2 axis;
uniform vec3 boundsLow,boundsHigh;
vec3 viewRay(vec2 q){vec4 v=inverseProjection*vec4(q*2-1,1,1);return normalize(v.xyz/v.w);}
vec3 viewPosition(vec2 q,float depth){return viewRay(q)*depth;}
vec3 colorForThickness(float thickness){
    float start=max(colorTransition.x,0),end=max(colorTransition.y,start+.001);
    return mix(waterThinColor,waterColor,smoothstep(start,end,max(thickness,0)));
}
bool nearClipped(float rawDepth){
    float nearZ=projection[3][2]/(projection[2][2]-1);
    vec2 rayXY=(uv*2-1)/vec2(projection[0][0],projection[1][1]);
    return rawDepth<=nearZ*sqrt(1+dot(rayXY,rayXY))*1.002;
}
float sceneDistance(vec2 q){vec4 v=inverseProjection*vec4(q*2-1,texture(sceneDepth,q).r*2-1,1);return length(v.xyz/v.w);}
#ifdef PASS_PACK
void main(){float d=texture(waterDepth,uv).r,t=texture(waterThickness,uv).r*thicknessStrength;result=vec4(d,t,t,d);}
#elif defined(PASS_SMOOTH)
void main(){
    vec4 original=texture(waterDepth,uv);
    if(original.a>10000 || smoothRadius<=0 || nearClipped(original.a)){result=original;return;}
    float radiusFloat=resolution.x*projection[0][0]/(2*original.a)*(radius*smoothRadius);
    int taps=int(ceil(radiusFloat));if(taps<=1)taps=2;taps=min(32,taps);
    float fractional=max(0,float(taps)-radiusFloat);
    float sigma=max(.0000001,(float(taps)-fractional)/(6*smoothSharpness));
    vec4 sum=vec4(0);float weights=0;
    for(int x=-taps;x<=taps;++x){
        vec2 q=clamp(uv+axis*float(x)/resolution,.5/resolution,1-.5/resolution);
        vec4 value=texture(waterDepth,q);
        if(value.a<=0 || value.a>=10000 || value.r<=0 || value.r>=10000)continue;
        float difference=original.a-value.a;
        float weight=exp(-float(x*x)/(2*sigma*sigma))*exp(-difference*difference*depthRejection);
        sum+=value*weight;weights+=weight;
    }
    result=vec4(sum.rg/max(weights,1e-20),original.ba);
}
#elif defined(PASS_NORMALS)
bool neighbor(vec2 q,float d,out vec3 p){
    if(any(lessThan(q,.5/resolution))||any(greaterThan(q,1-.5/resolution)))return false;
    float v=texture(waterDepth,q).r;
    if(v<=0||v>=10000||abs(v-d)>max(radius*4,.01))return false;
    p=viewPosition(q,v);return true;
}
void main(){
    vec4 data=texture(waterDepth,uv);float d=data.r;if(d<=0||d>=10000||nearClipped(data.a)){result=vec4(0);return;}
    vec2 pixel=1/resolution;vec3 p=viewPosition(uv,d),a,b;
    vec3 dx=viewPosition(uv+vec2(pixel.x,0),d)-p;
    vec3 dy=viewPosition(uv+vec2(0,pixel.y),d)-p;
    bool plus=neighbor(uv+vec2(pixel.x,0),d,a),minus=neighbor(uv-vec2(pixel.x,0),d,b);
    if(plus||minus)dx=plus&&(!minus||abs(a.z-p.z)<abs(p.z-b.z))?a-p:p-b;
    plus=neighbor(uv+vec2(0,pixel.y),d,a);minus=neighbor(uv-vec2(0,pixel.y),d,b);
    if(plus||minus)dy=plus&&(!minus||abs(a.z-p.z)<abs(p.z-b.z))?a-p:p-b;
    vec3 c=cross(dx,dy),n=dot(c,c)>1e-20?normalize(c):-viewRay(uv);
    if(dot(n,-p)<0)n=-n;
    result=vec4(mat3(inverseView)*n,1);
}
#elif defined(PASS_VIEW_DEPTH)
void main(){float d=texture(waterDepth,uv).r;result=vec4(d>10000?0:-viewPosition(uv,d).z,0,0,0);}
#else
float depthToZ(float depth){return projection[3][2]/(depth*2-1+projection[2][2]);}
float sceneZ(vec2 q){return depthToZ(texture(sceneDepth,q).r);}
bool projectPoint(vec3 p,out vec2 q){
    vec4 clip=projection*vec4(p,1);
    if(clip.w<=0||clip.z < -clip.w || clip.z>clip.w)return false;
    q=clip.xy/clip.w*.5+.5;
    return all(greaterThan(q,.5/resolution))&&all(lessThan(q,1-.5/resolution));
}
vec3 reflectedScene(vec3 start,vec3 direction){
    float lengthLimit=clamp(-start.z*2,8,60),previous=0;
    float bias=max(.005,radius*.1);
    for(int i=1;i<=16;++i){
        float t=lengthLimit*pow(float(i)/16,2);
        vec3 p=start+direction*(t+bias);vec2 q;
        if(!projectPoint(p,q))break;
        float opaqueDepth=texture(sceneDepth,q).r;
        if(opaqueDepth<1 && -p.z>=depthToZ(opaqueDepth)){
            float lo=previous,hi=t;
            for(int j=0;j<4;++j){
                float mid=(lo+hi)*.5;vec2 sampleUv;
                vec3 sampleP=start+direction*(mid+bias);
                if(projectPoint(sampleP,sampleUv)&&-sampleP.z>=sceneZ(sampleUv))hi=mid;else lo=mid;
            }
            p=start+direction*(hi+bias);
            if(projectPoint(p,q)&&abs(-p.z-sceneZ(q))<max(.025,radius*.5)){
                float edge=min(min(q.x,q.y),min(1-q.x,1-q.y));
                return mix(sceneBackgroundColor,texture(sceneColor,q).rgb,smoothstep(0,.04,edge));
            }
            return sceneBackgroundColor;
        }
        previous=t;
    }
    return sceneBackgroundColor;
}
bool clearRefraction(vec2 q,float expectedZ){
    ivec2 size=textureSize(sceneDepth,0),base=ivec2(floor(q*vec2(size)-.5));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        float d=texelFetch(sceneDepth,clamp(base+ivec2(x,y),ivec2(0),size-1),0).r;
        if(depthToZ(d)<=expectedZ+max(.002,radius*.02))return false;
    }
    return true;
}
vec3 refractedScene(vec3 start,vec3 end){
    vec2 target;
    if(!projectPoint(end,target))return texture(sceneColor,uv).rgb;
    vec2 delta=target-uv;
    float pixelDistance=length(delta*resolution);
    if(pixelDistance<.5)return texture(sceneColor,uv).rgb;
    delta*=min(1.0,16.0/pixelDistance);target=uv+delta;
    float fraction=min(1.0,16.0/pixelDistance);
    int steps=clamp(int(ceil(min(pixelDistance,16.0))),1,16);
    for(int i=1;i<=steps;++i){
        float t=float(i)/float(steps);
        float expectedZ=1/mix(1/(-start.z),1/(-end.z),t*fraction);
        if(!clearRefraction(mix(uv,target,t),expectedZ))return texture(sceneColor,uv).rgb;
    }
    return texture(sceneColor,target).rgb;
}
float reflectance(vec3 incoming,vec3 normal){
    float cosine=clamp(-dot(incoming,normal),0,1),ratio=1/1.33;
    float sineSquared=ratio*ratio*(1-cosine*cosine);
    if(sineSquared>=1)return 1;
    float transmitted=sqrt(1-sineSquared);
    float perpendicular=(cosine-1.33*transmitted)/max(cosine+1.33*transmitted,1e-6);
    float parallel=(1.33*cosine-transmitted)/max(1.33*cosine+transmitted,1e-6);
    return (perpendicular*perpendicular+parallel*parallel)*.5;
}
void main(){
    vec4 data=texture(waterDepth,uv);
    if(data.r>10000||data.r<=0||data.r>=sceneDistance(uv))discard;
    vec3 ray=viewRay(uv),position=ray*data.r,world=(inverseView*vec4(position,1)).xyz;
    vec4 nearPoint=inverseProjection*vec4(uv*2-1,-1,1);
    float nearDistance=length(nearPoint.xyz/nearPoint.w);
    if(data.a<=nearDistance*1.002){
        vec3 absorption=exp(-max(data.g,0)*vec3(.624,.156,.078)*absorptionStrength);
        result=vec4(texture(sceneColor,uv).rgb*absorption+colorForThickness(data.g)*(1-absorption),1);
        gl_FragDepth=0;return;
    }
    vec3 incoming=mat3(inverseView)*ray;
    vec3 n=texture(surfaceNormals,uv).xyz;
    if(dot(n,n)<1e-10)n=-incoming;else n=normalize(n);
    if(dot(n,incoming)>0)n=-n;
    float reflectedWeight=clamp(reflectance(incoming,n)*reflectionStrength,0,1);
    vec3 transmittedDirection=refract(incoming,n,1/1.33);
    vec3 viewNormal=mat3(view)*n;
    vec3 reflected=sceneBackgroundColor;
    if(reflectedWeight>.001)reflected=reflectedScene(position,reflect(ray,viewNormal));
    vec3 exitPoint=position+mat3(view)*transmittedDirection*max(data.g,0)*refractionStrength;
    vec3 transmitted=refractedScene(position,exitPoint);
    vec3 absorption=exp(-max(data.g,0)*vec3(.624,.156,.078)*absorptionStrength);
    transmitted=transmitted*absorption+colorForThickness(data.g)*(1-absorption);
    result=vec4(mix(transmitted,reflected,reflectedWeight),1);
    vec4 clip=projection*vec4(position,1);gl_FragDepth=clip.z/clip.w*.5+.5;
}
#endif
#endif
#endif
