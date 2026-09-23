#if defined(FRAGMENT_SHADER) && defined(PASS_DEPTH)
#extension GL_ARB_conservative_depth : enable
#ifdef GL_ARB_conservative_depth
layout(depth_greater) out float gl_FragDepth;
#endif
#endif
uniform mat4 projection, inverseProjection, view, inverseView;
uniform vec2 resolution;
uniform float radius;
#ifdef PASS_NOISE
uniform sampler3D detailNoise;
#endif
#if defined(PASS_DEPTH) || defined(PASS_THICKNESS) || defined(PASS_NOISE)
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
layout(location=1) in vec4 velocity;
flat out vec3 particleWorld;
flat out float particleSeed;
flat out vec3 center;
void main(){
    center=(view*vec4(position.xyz,1)).xyz;particleWorld=position.xyz;particleSeed=float(gl_InstanceID);
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
flat in vec3 particleWorld;
flat in float particleSeed;
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
    float opaque=texture(sceneDepth,uv).r;
    vec4 scene=inverseProjection*vec4(uv*2-1,opaque*2-1,1);
    float sceneT=length(scene.xyz/scene.w);

#ifndef PASS_DEPTH
    if(front>=sceneT)discard;
#endif
#if defined(PASS_THICKNESS) || defined(PASS_NOISE)
    float segment=max(0,min(back,sceneT)-front);
    float radial=clamp(1-discriminant/(radius*radius),0,1);
    float weight=(exp(-2*radial)-exp(-2.0))/(1-exp(-2.0));
#ifdef PASS_THICKNESS
    result=vec4(segment*.46*weight,0,0,1);
#else
    vec3 local=mat3(inverseView)*(ray*(front+back)*.5-center)/radius;
    vec3 phase=local*.13+particleSeed*vec3(.137,.219,.073);
    vec3 detail=texture(detailNoise,phase).xyz*2-1;
    result=vec4(detail*weight,weight);
#endif
#else
    vec3 surface=ray*front;vec4 clip=projection*vec4(surface,1);
    gl_FragDepth=max(gl_FragCoord.z,clamp(clip.z/clip.w*.5+.5,0,1));
    result=vec4(-surface.z,-scene.z/scene.w,0,0);
#endif
}
#endif
#else
#ifdef VERTEX_SHADER
out vec2 uv;
void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2-1,0,1);}
#endif
#ifdef FRAGMENT_SHADER
in vec2 uv;
layout(location=0) out vec4 result;
uniform sampler2D waterDepth,waterThickness,sceneColor,sceneDepth;
uniform vec2 axis;
uniform bool firstPass;
vec3 viewPosition(vec2 p,float d){vec4 v=inverseProjection*vec4(p*2-1,1,1);return v.xyz*(-d/v.z);}
float sceneDistance(vec2 p){float d=texture(sceneDepth,p).r;vec4 v=inverseProjection*vec4(p*2-1,d*2-1,1);return -v.z/v.w;}
#ifdef PASS_SMOOTH
float filteredDepth(vec2 q,float anchor){
    ivec2 size=textureSize(waterDepth,0);vec2 grid=q*vec2(size)-.5;
    ivec2 base=ivec2(floor(grid));vec2 f=fract(grid);float sum=0,total=0;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        ivec2 cell=clamp(base+ivec2(x,y),ivec2(0),size-1);
        vec2 stored=texelFetch(waterDepth,cell,0).rg;
        float d=stored.r;
        if(d>=stored.g)continue;
        if(d<=0 || abs(d-anchor)>max(radius*12,anchor*.15))continue;
        float w=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);sum+=w/d;total+=w;
    }
    return total>.25?total/max(sum,1e-8):0;
}
float slope(vec2 offset,float d){
    float a=filteredDepth(uv-offset,d),b=filteredDepth(uv+offset,d);
    if(a>0 && b>0)return (1/b-1/a)*.5;
    if(a>0)return 1/d-1/a;
    if(b>0)return 1/b-1/d;
    return 0;
}
void main(){
    vec2 centerData=texture(waterDepth,uv).rg;
    float center=centerData.r;
    if(center<=0){result=vec4(0);return;}
    if(center>=centerData.g){result=vec4(centerData,0,0);return;}
    float kernel=clamp(resolution.y*projection[1][1]*radius/center*2.5,2,32);
    float sigma=max(kernel*.5,1),range=radius*1.5;
    float probe=max(kernel*.5,1);
    vec2 gradient=vec2(slope(vec2(probe/resolution.x,0),center),slope(vec2(0,probe/resolution.y),center))/probe;
    float sum=0,inverseDepth=0;
    for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x){
        vec2 offset=vec2(x,y)*(kernel*.25);
        vec2 q=clamp(uv+offset/resolution,.5/resolution,1-.5/resolution);
        float d=filteredDepth(q,center);if(d<=0)continue;
        float expected=1/center+dot(gradient,offset);
        float difference=(1/d-expected)*center*center/range;
        float weight=exp(-.5*dot(offset,offset)/(sigma*sigma)-difference*difference);
        inverseDepth+=weight*(1/d-dot(gradient,offset));sum+=weight;
    }
    result=vec4(sum/max(inverseDepth,1e-8),centerData.g,0,0);
}
#elif defined(PASS_THICKNESS_BLUR)
void main(){
    float sum=0,total=0;
    for(int i=-4;i<=4;++i){float w=exp(-float(i*i)/8);sum+=texture(waterThickness,clamp(uv+axis*float(i)/resolution,.5/resolution,1-.5/resolution)).r*w;total+=w;}
    result=vec4(sum/total,0,0,0);
}
#else
uniform samplerCube environmentMap;
uniform sampler2D surfaceNoise;
vec4 sampleWater(vec2 p,out float coverage){
    ivec2 dimensions=textureSize(waterDepth,0);
    vec2 coordinate=p*vec2(dimensions)-.5;ivec2 base=ivec2(floor(coordinate));vec2 f=fract(coordinate);
    float opaque=sceneDistance(p),anchor=1e20,depth=0;coverage=0;
    float values[4];float weights[4];
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        int index=y*2+x;ivec2 cell=clamp(base+ivec2(x,y),ivec2(0),dimensions-1);
        vec2 stored=texelFetch(waterDepth,cell,0).rg;
        float d=stored.r;
        bool valid=d>0 && d<opaque && d<stored.g;
        values[index]=valid?d:0;
        weights[index]=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
        if(valid)anchor=min(anchor,d);
    }
    for(int i=0;i<4;++i){float d=values[i];if(d<=0||abs(d-anchor)>max(radius*3,anchor*.03))continue;depth+=d*weights[i];coverage+=weights[i];}
    return vec4(depth/max(coverage,1e-6),texture(waterThickness,p).r,0,0);
}
float depthAt(vec2 q){if(any(lessThan(q,vec2(0)))||any(greaterThan(q,vec2(1))))return 0;float coverage;float d=sampleWater(q,coverage).r;return coverage>.5?d:0;}
vec3 tangent(vec2 q,vec2 step,float d){
    vec3 p=viewPosition(q,d);float a=depthAt(q-step),b=depthAt(q+step);
    bool va=a>0&&abs(a-d)<max(radius*3,d*.03),vb=b>0&&abs(b-d)<max(radius*3,d*.03);
    if(va&&vb){
        float left=abs(a-d),right=abs(b-d);
        if(max(left,right)<radius*.2 || max(left,right)<min(left,right)*2)
            return (viewPosition(q+step,b)-viewPosition(q-step,a))*.5;
    }
    if(va&&(!vb||abs(a-d)<abs(b-d)))return p-viewPosition(q-step,a);
    if(vb)return viewPosition(q+step,b)-p;
    return viewPosition(q+step,d)-p;
}
vec3 fittedNormal(vec2 q,float depth,vec3 fallback){
    vec2 pixel=1.0/vec2(textureSize(waterDepth,0));
    float stride=clamp(float(textureSize(waterDepth,0).y)*projection[1][1]*radius/depth*.8,1.5,8);
    float sw=0,sx=0,sy=0,sz=0,sxx=0,syy=0,sxy=0,sxz=0,syz=0;
    vec3 point=viewPosition(q,depth);
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
        vec2 offset=vec2(x,y)*stride;
        float d=depthAt(q+offset*pixel);if(d<=0)continue;
        vec3 ray=viewPosition(q+offset*pixel,1);
        float denominator=dot(fallback,ray);
        if(abs(denominator)<1e-5)continue;
        float predicted=dot(fallback,point)/denominator;
        float residual=(d-predicted)/max(radius*2,.001);
        float w=exp(-.4*float(x*x+y*y)-.5*residual*residual);
        float z=1/d;
        sw+=w;sx+=w*offset.x;sy+=w*offset.y;sz+=w*z;
        sxx+=w*offset.x*offset.x;syy+=w*offset.y*offset.y;sxy+=w*offset.x*offset.y;sxz+=w*offset.x*z;syz+=w*offset.y*z;
    }
    if(sw<1e-5)return fallback;
    float xx=sxx-sx*sx/sw,yy=syy-sy*sy/sw,xy=sxy-sx*sy/sw;
    float xz=sxz-sx*sz/sw,yz=syz-sy*sz/sw,det=xx*yy-xy*xy;
    if(det<1e-5)return fallback;
    vec2 g=vec2(yy*xz-xy*yz,xx*yz-xy*xz)/det;
    float a=1/depth+g.x,b=1/depth+g.y;if(a<=0||b<=0)return fallback;
    vec3 dx=viewPosition(q+vec2(pixel.x,0),1/a)-point;
    vec3 dy=viewPosition(q+vec2(0,pixel.y),1/b)-point;
    vec3 n=normalize(cross(dx,dy));return dot(n,fallback)<0?-n:n;
}
bool refractionVisible(vec2 q,float water,float background){
    ivec2 dimensions=textureSize(sceneDepth,0);
    ivec2 base=ivec2(floor(q*vec2(dimensions)-.5));
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){
        vec2 p=(vec2(clamp(base+ivec2(x,y),ivec2(0),dimensions-1))+.5)/vec2(dimensions);
        float d=sceneDistance(p);
        if(d<=water || d<background-max(radius*.25,background*.01))return false;
    }
    return true;
}
vec2 refractionUv(vec2 start,vec2 target,float water){
    vec2 size=vec2(textureSize(sceneDepth,0));
    vec2 delta=(target-start)*size;
    float lengthPixels=max(abs(delta.x),abs(delta.y));
    delta*=min(1.0,24.0/max(lengthPixels,1.0));
    int steps=int(ceil(max(abs(delta.x),abs(delta.y))));
    float background=sceneDistance(start);
    if(!refractionVisible(start,water,background))return start;
    for(int i=1;i<=24;++i){
        if(i>steps)break;
        float t=float(i)/float(max(steps,1));
        if(!refractionVisible(start+delta/size*t,water,background)){
            float safe=max(0.0,float(i-2)/float(max(steps,1)));
            return start+delta/size*safe;
        }
    }
    return start+delta/size;
}
void main(){
    float coverage;vec4 water=sampleWater(uv,coverage);if(water.r<=0||coverage<.05)discard;
    if(water.r>=sceneDistance(uv))discard;
    vec3 p=viewPosition(uv,water.r);vec2 pixel=1.0/vec2(textureSize(waterDepth,0));
    float stepSize=clamp(float(textureSize(waterDepth,0).y)*projection[1][1]*radius/water.r*.35,1.5,4);
    vec3 n=normalize(cross(tangent(uv,vec2(pixel.x*stepSize,0),water.r),tangent(uv,vec2(0,pixel.y*stepSize),water.r)));
    n=fittedNormal(uv,water.r,n);
    vec3 v=normalize(-p);if(dot(n,v)<0)n=-n;

    float thickness=max(water.g,0);
    vec2 refracted=clamp(uv-n.xy*min(thickness,2)*.025,pixel*.5,1-pixel*.5);
    refracted=refractionUv(uv,refracted,water.r);
    vec3 absorption=exp(-vec3(.52,.13,.065)*thickness);
    vec3 transmitted=texture(sceneColor,refracted).rgb*absorption+vec3(.025,.17,.21)*(1-absorption);
    float fresnel=.0204+.9796*pow(1-max(dot(n,v),0),5);
    vec3 reflected=texture(environmentMap,mat3(inverseView)*reflect(-v,n)).rgb;
    vec3 light=normalize(mat3(view)*vec3(-.4,.8,.3));
    float variance=dot(dFdx(n),dFdx(n))+dot(dFdy(n),dFdy(n));
    float exponent=96/(1+96*variance);
    float specular=pow(max(dot(n,normalize(light+v)),0),exponent)*(fresnel*3)*(exponent/96);
    vec3 color=mix(transmitted,reflected,fresnel)+vec3(1,.96,.90)*specular;
    result=vec4(mix(texture(sceneColor,uv).rgb,color,smoothstep(.05,.95,coverage)),1);
    vec4 clip=projection*vec4(p,1);gl_FragDepth=clip.z/clip.w*.5+.5;
}
#endif
#endif
#endif
