uniform mat4 projection,inverseProjection,view,inverseView;
uniform vec2 resolution;
uniform float radius;
uniform vec3 waterColor,waterThinColor;
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
#ifdef PASS_DEPTH
    vec4 clip=projection*vec4(ray*front,1);
    gl_FragDepth=max(gl_FragCoord.z,clamp(clip.z/clip.w*.5+.5,0,1));
    result=vec4(front,0,0,1);
#else
    if(front>=sceneT)discard;
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
float sceneDistance(vec2 q){vec4 v=inverseProjection*vec4(q*2-1,texture(sceneDepth,q).r*2-1,1);return length(v.xyz/v.w);}
#ifdef PASS_PACK
void main(){float d=texture(waterDepth,uv).r,t=texture(waterThickness,uv).r*thicknessStrength;result=vec4(d,t,t,d);}
#elif defined(PASS_SMOOTH)
void main(){
    vec4 original=texture(waterDepth,uv);
    if(original.a>10000 || smoothRadius<=0){result=original;return;}
    float radiusFloat=resolution.x*projection[0][0]/(2*original.a)*(radius*smoothRadius);
    int taps=int(ceil(radiusFloat));if(taps<=1)taps=2;taps=min(32,taps);
    float fractional=max(0,float(taps)-radiusFloat);
    float sigma=max(.0000001,(float(taps)-fractional)/(6*smoothSharpness));
    vec4 sum=vec4(0);float weights=0;
    for(int x=-taps;x<=taps;++x){
        vec2 q=clamp(uv+axis*float(x)/resolution,.5/resolution,1-.5/resolution);
        vec4 value=texture(waterDepth,q);
        float difference=original.a-value.a;
        float weight=exp(-float(x*x)/(2*sigma*sigma))*exp(-difference*difference*depthRejection);
        sum+=value*weight;weights+=weight;
    }
    result=vec4(sum.rg/max(weights,1e-20),original.ba);
}
#elif defined(PASS_NORMALS)
vec3 positionAt(vec2 q){q=clamp(q,.5/resolution,1-.5/resolution);return viewPosition(q,texture(waterDepth,q).r);}
void main(){
    float d=texture(waterDepth,uv).r;if(d>10000){result=vec4(0);return;}
    vec2 pixel=1/resolution;vec3 p=positionAt(uv);
    vec3 dx=positionAt(uv+vec2(pixel.x,0))-p,dx2=p-positionAt(uv-vec2(pixel.x,0));
    vec3 dy=positionAt(uv+vec2(0,pixel.y))-p,dy2=p-positionAt(uv-vec2(0,pixel.y));
    if(abs(dx2.z)<abs(dx.z))dx=dx2;if(abs(dy2.z)<abs(dy.z))dy=dy2;
    vec3 crossNormal=cross(dx,dy);
    vec3 n=dot(crossNormal,crossNormal)>1e-20?normalize(crossNormal):-viewRay(uv);
    if(dot(n,-p)<0)n=-n;
    result=vec4(mat3(inverseView)*n,1);
}
#elif defined(PASS_VIEW_DEPTH)
void main(){float d=texture(waterDepth,uv).r;result=vec4(d>10000?0:-viewPosition(uv,d).z,0,0,0);}
#else
vec3 sky(vec3 direction){
    vec3 ground=vec3(.35,.3,.35)*.53;
    float gradient=pow(smoothstep(0,.4,direction.y),.35);
    float horizon=smoothstep(-.01,0,direction.y);
    float sun=pow(max(0,dot(direction,normalize(vec3(-.4,.8,.3)))),1500)*16;
    return mix(ground,mix(vec3(1),vec3(.08,.37,.73),gradient),horizon)+sun*step(1,horizon);
}
vec3 environment(vec3 origin,vec3 direction){
    if(direction.y<-.00001 && origin.y>0){
        vec3 floorPoint=origin+direction*(-origin.y/direction.y);
        if(max(abs(floorPoint.x),abs(floorPoint.z))<30){
            vec4 cameraPoint=view*vec4(floorPoint,1),clip=projection*cameraPoint;
            if(clip.w>0){
                vec2 q=clip.xy/clip.w*.5+.5;
                if(all(greaterThan(q,vec2(0)))&&all(lessThan(q,vec2(1)))){
                    float actual=sceneDistance(q);
                    if(abs(actual-length(cameraPoint.xyz))<max(.03,actual*.003))return texture(sceneColor,q).rgb;
                }
            }
            float tile=mod(floor(floorPoint.x)+floor(floorPoint.z),2);
            return mix(vec3(.18,.20,.23),vec3(.65,.68,.72),tile);
        }
    }
    return sky(direction);
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
vec3 edgeNormal(vec3 normal,vec3 world){
    vec3 halfSize=(boundsHigh-boundsLow)*.5,p=world-(boundsHigh+boundsLow)*.5;
    vec3 o=halfSize-abs(p);
    if(any(lessThan(o,vec3(0))))return normal;
    vec3 face=o.x<o.y&&o.x<o.z?vec3(sign(p.x),0,0):(o.y<o.z?vec3(0,sign(p.y),0):vec3(0,0,sign(p.z)));
    float weight=(1-smoothstep(0,.01,max(0,min(o.x,o.z))))*clamp(abs(o.x-o.z)*6,0,1);
    vec3 edge=normalize(mix(normal,face,weight));
    return normalize(normal+edge*6*max(0,dot(normal,edge)));
}
bool visibleRefraction(vec2 q,float depth){
    ivec2 size=textureSize(sceneDepth,0),base=ivec2(floor(q*vec2(size)-.5));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        vec2 p=(vec2(clamp(base+ivec2(x,y),ivec2(0),size-1))+.5)/vec2(size);
        if(sceneDistance(p)<=depth)return false;
    }return true;
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
    n=edgeNormal(n,world);if(dot(n,incoming)>0)n=-n;
    float reflectedWeight=clamp(reflectance(incoming,n)*reflectionStrength,0,1);
    vec3 transmittedDirection=refract(incoming,n,1/1.33);
    vec3 exitPoint=world+transmittedDirection*data.g*refractionStrength;
    if(exitPoint.y<.0001 && transmittedDirection.y<-.0001)
        exitPoint=world+transmittedDirection*max(0,(.0001-world.y)/transmittedDirection.y);
    vec3 reflected=environment(world,reflect(incoming,n));
    vec3 transmitted=environment(exitPoint,incoming);
    vec4 projectedExit=projection*view*vec4(exitPoint,1);
    if(projectedExit.w>0){
        vec2 refractedUV=projectedExit.xy/projectedExit.w*.5+.5;
        if(all(greaterThan(refractedUV,vec2(0)))&&all(lessThan(refractedUV,vec2(1)))){
            float floorDepth=1e20;
            if(incoming.y<-.00001 && exitPoint.y>0){
                vec3 floorPoint=exitPoint+incoming*(-exitPoint.y/incoming.y);
                floorDepth=length((view*vec4(floorPoint,1)).xyz);
            }
            bool visible=visibleRefraction(refractedUV,data.r);
            if(visible && sceneDistance(refractedUV)<floorDepth-.03)
                transmitted=texture(sceneColor,refractedUV).rgb;
            else if(!visible)transmitted=texture(sceneColor,uv).rgb;
        }
    }
    vec3 absorption=exp(-max(data.g,0)*vec3(.624,.156,.078)*absorptionStrength);
    transmitted=transmitted*absorption+colorForThickness(data.g)*(1-absorption);
    result=vec4(mix(transmitted,reflected,reflectedWeight),1);
    vec4 clip=projection*vec4(position,1);gl_FragDepth=clip.z/clip.w*.5+.5;
}
#endif
#endif
#endif
