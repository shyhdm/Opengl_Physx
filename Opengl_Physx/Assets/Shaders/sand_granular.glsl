#extension GL_ARB_conservative_depth : require
// Multiscale granular appearance: stable per-grain material, filtered local
// mineral facets near the camera, aggregate microfacet reflection at distance.
// Independent real-time implementation; references and limitations in README.
uniform mat4 view, projection, inverseProjection;
uniform vec2 resolution, viewportOrigin;
uniform float radius;
uint hash32(uint x){x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);}
float randomValue(uint x){return float(hash32(x)>>8u)*(1./16777216.);}
mat3 orientation(uint id){
    float a=randomValue(id+19u)*6.2831853,b=randomValue(id+37u)*6.2831853;
    return mat3(cos(a),sin(a),0,-sin(a),cos(a),0,0,0,1)*mat3(cos(b),0,-sin(b),0,1,0,sin(b),0,cos(b));
}
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
layout(location=2) in uint particleId;
flat out vec3 center;
flat out uint identity;



void main(){
#ifdef PASS_SHADE
    vec2 p=vec2((gl_VertexID&1)==0?-1.:1.,(gl_VertexID&2)==0?-1.:1.);
    gl_Position=vec4(p,-1,1);return;
#else
    identity=particleId;center=(view*vec4(position.xyz,1)).xyz;
    int largeGrain=radius*projection[1][1]*resolution.y/max(-center.z,.01)>8.?1:0;
    mat3 grainRotation=mat3(1); float grainExtent[6];
    float frontOffset=radius;
    if(largeGrain!=0){
    grainRotation=mat3(view)*orientation(particleId);
    float size=mix(.92,1.,randomValue(particleId+43u));
    for(int i=0;i<6;++i){
        float extent=i<6?(i/2==0?.64:(i/2==1?.55:.50)):.64;
        grainExtent[i]=extent*(size*mix(.90,1.,randomValue(particleId+uint(i)*73u+101u)));
    }
    // Maximum view-space Z over the six-plane box bounds the clipped grain.
    vec3 z=vec3(grainRotation[0].z,grainRotation[1].z,grainRotation[2].z);
    frontOffset=radius*(dot(max(z,vec3(0)),vec3(grainExtent[0],grainExtent[2],grainExtent[4]))
                    +dot(max(-z,vec3(0)),vec3(grainExtent[1],grainExtent[3],grainExtent[5])));
    frontOffset=-center.z>radius?min(radius,frontOffset+radius*.0001):radius;
    }
    vec2 corner=vec2((gl_VertexID&1)==0?-1.:1.,(gl_VertexID&2)==0?-1.:1.);
    vec4 clip=projection*vec4(center,1);
    // Conservative bounding quad; unlike GL_POINTS, no driver point-size cap.
    float expand=max(-center.z,.01)/max(-center.z-radius,.01);
    clip.xy+=corner*vec2(projection[0][0],projection[1][1])*radius*expand;
    // Project the eight corners of the grain's exact axis-plane box. The
    // diagonal cuts can only shrink this box, so this cannot remove grain pixels.
    // Retain the original bound when the box crosses the camera plane.
    if(largeGrain!=0){
    vec2 lower=vec2(1e20),upper=vec2(-1e20);bool safe=true;
    for(int j=0;j<8;++j){
        vec3 q=vec3((j&1)==0?-grainExtent[1]:grainExtent[0],
                    (j&2)==0?-grainExtent[3]:grainExtent[2],
                    (j&4)==0?-grainExtent[5]:grainExtent[4]);
        vec4 projected=projection*vec4(center+grainRotation*q*radius,1);
        if(projected.w<=.001)safe=false;
        vec2 ndc=projected.xy/max(projected.w,.001);
        lower=min(lower,ndc);upper=max(upper,ndc);
    }
    if(safe&&clip.w>0.){
        vec2 bound=mix(lower,upper,corner*.5+.5)+corner*(2./resolution);
        vec2 previous=clip.xy/clip.w;
        clip.xy=vec2(corner.x<0.?max(previous.x,bound.x):min(previous.x,bound.x),
                     corner.y<0.?max(previous.y,bound.y):min(previous.y,bound.y))*clip.w;
    }
    }
    // Rasterize on the conservative front of the box (sphere fallback near the
    // camera). Actual grain depth is greater, permitting early depth rejection.
    vec4 front=projection*vec4(center+vec3(0,0,frontOffset),1);
    if(front.w>0.)clip.z=max(-clip.w,front.z/front.w*clip.w);
    if(center.z>radius)clip=vec4(2,2,2,1);
    gl_Position=clip;
#endif
}
#endif
#ifdef FRAGMENT_SHADER
layout(depth_greater) out float gl_FragDepth;
flat in vec3 center;
#ifdef PASS_DEPTH
flat in uint identity;
#else
uint identity;
#endif



layout(location=0) out vec4 result;
#ifdef PASS_DEPTH
layout(location=1) out uint visibleId;
layout(location=2) out vec4 grainHit;
#endif
uniform usampler2D grainIds;
uniform sampler2D grainDepth,grainNormals,grainHits;
int hitFace=0;
float storedFootprint;
uniform vec3 sandColor,sunDirection;
uniform float roughness,sparkleStrength,mineralFraction,microScale,occlusionStrength;
const float PI=3.14159265359;

vec3 viewPosition(vec2 uv,float d){vec4 p=inverseProjection*vec4(uv*2.-1.,d*2.-1.,1);return p.xyz/p.w;}

// Opposing planes share one ray denominator. Solve their slab together.
bool clipGrainSlab(vec3 plane,vec2 extent,ivec2 index,vec3 ro,vec3 rd,
                   inout float entry,inout float exitDistance,inout vec3 face){
    float denominator=dot(plane,rd),origin=dot(plane,ro);
    if(abs(denominator)<1e-7)return origin<=extent.x&&origin>=-extent.y;
    vec2 t=vec2(extent.x-origin,-extent.y-origin)/denominator;
    float nearT=denominator<0.?t.x:t.y;
    float farT=denominator<0.?t.y:t.x;
    int nearFace=denominator<0.?index.x:index.y;
    if(nearT>entry||(nearT==entry&&nearFace<hitFace)){
        entry=nearT;face=denominator<0.?plane:-plane;hitFace=nearFace;
    }
    exitDistance=min(exitDistance,farT);
    return entry<=exitDistance;
}
bool intersectGrain(vec3 ro,vec3 rd,out vec3 hit,out vec3 normal){
    float entry=-1e20,exitDistance=1e20;vec3 face=vec3(0,0,1);
    float grainSize=mix(.92,1.,randomValue(identity+43u));
    vec2 extent;
    extent=.64*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+101u),randomValue(identity+174u))));
    if(!clipGrainSlab(vec3(1.,0.,0.),extent,ivec2(0,1),ro,rd,entry,exitDistance,face))return false;
    extent=.55*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+247u),randomValue(identity+320u))));
    if(!clipGrainSlab(vec3(0.,1.,0.),extent,ivec2(2,3),ro,rd,entry,exitDistance,face))return false;
    extent=.50*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+393u),randomValue(identity+466u))));
    if(!clipGrainSlab(vec3(0.,0.,1.),extent,ivec2(4,5),ro,rd,entry,exitDistance,face))return false;
    extent=.64*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+539u),randomValue(identity+1050u))));
    if(!clipGrainSlab(normalize(vec3(1.,1.,1.)),extent,ivec2(6,13),ro,rd,entry,exitDistance,face))return false;
    extent=.64*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+612u),randomValue(identity+977u))));
    if(!clipGrainSlab(normalize(vec3(-1.,1.,1.)),extent,ivec2(7,12),ro,rd,entry,exitDistance,face))return false;
    extent=.64*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+685u),randomValue(identity+904u))));
    if(!clipGrainSlab(normalize(vec3(1.,-1.,1.)),extent,ivec2(8,11),ro,rd,entry,exitDistance,face))return false;
    extent=.64*(grainSize*mix(vec2(.90),vec2(1.),vec2(randomValue(identity+758u),randomValue(identity+831u))));
    if(!clipGrainSlab(normalize(vec3(-1.,-1.,1.)),extent,ivec2(9,10),ro,rd,entry,exitDistance,face))return false;
    if(entry<0.)return false;
    hit=ro+rd*entry;normal=face;return true;
}

// Nearby depth gives a coarse pile orientation and local cavity attenuation.
// Single-layer screen-space approximation: not a volumetric light transport solver.
void neighborhood(vec3 p,vec3 n,vec2 uv,out vec3 macroNormal,out float ao){
    float range=radius*3.5;
    float pixelRadius=clamp(range*projection[1][1]*resolution.y*.5/max(-p.z,.01),2.,64.);
    vec3 average=n*2.;float weights=2.,obscurance=0.;
    for(int i=0;i<12;++i){
        float a=float(i)*2.39996323,t=(float(i)+.5)/12.;
        vec2 q=uv+vec2(cos(a),sin(a))*sqrt(t)*pixelRadius/resolution;
        if(any(lessThan(q,vec2(0)))||any(greaterThan(q,vec2(1))))continue;
        float d=texture(grainDepth,q).r;if(d>=1.)continue;
        vec3 delta=viewPosition(q,d)-p;float distance=length(delta);
        float weight=1.-smoothstep(range*.15,range,distance);
        average+=texture(grainNormals,q).xyz*weight;weights+=weight;
        obscurance+=max(dot(n,delta)/max(distance,.0001)-.10,0.)*weight*smoothstep(radius*.08,radius*.3,distance);
    }
    macroNormal=normalize(average/weights);
    ao=clamp(1.-obscurance*occlusionStrength*.24,.32,1.);
}
float contactShadow(vec3 p,vec3 n,vec3 light){
    float visibility=1.;
    for(int i=0;i<12;++i){
        vec3 q=p+n*radius*.10+light*radius*(.4+float(i)*.38);
        vec4 projected=projection*vec4(q,1);if(projected.w<=0.)break;
        vec2 uv=projected.xy/projected.w*.5+.5;
        if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))break;
        float d=texture(grainDepth,uv).r;if(d>=1.)continue;
        float gap=viewPosition(uv,d).z-q.z;
        float hit=smoothstep(radius*.1,radius*.23,gap)*(1.-smoothstep(radius*.6,radius,gap));
        float edge=smoothstep(0.,.03,min(min(uv.x,uv.y),min(1.-uv.x,1.-uv.y)));
        visibility=min(visibility,1.-hit*edge*.8);
    }
    return visibility;
}
float ggx(vec3 n,vec3 l,vec3 v,float r){
    vec3 h=normalize(l+v);float nl=max(dot(n,l),.001),nv=max(dot(n,v),.001),nh=max(dot(n,h),0.);
    float a=r*r,a2=a*a,d=nh*nh*(a2-1.)+1.;
    float distribution=a2/(PI*d*d);
    float visibility=.5/(nl*sqrt(nv*nv*(1.-a2)+a2)+nv*sqrt(nl*nl*(1.-a2)+a2));
    float fresnel=.045+.955*pow(1.-max(dot(v,h),0.),5.);
    return distribution*visibility*fresnel*nl;
}
// Procedural mineral facets are fixed in local grain coordinates. Screen-space
// footprint filtering blends unresolved facets into a continuous BRDF instead
// of randomizing them each frame. A simulated grain can contain many micrograins.
vec2 microAppearance(vec3 hit,vec3 normal,mat3 rotation,vec3 l,vec3 v){
    vec3 tangent=normalize(cross(abs(normal.z)<.8?vec3(0,0,1):vec3(0,1,0),normal));
    vec3 bitangent=cross(normal,tangent);
    vec2 coord=vec2(dot(hit,tangent),dot(hit,bitangent))*microScale;
    float footprint=storedFootprint;
    float resolved=1.-smoothstep(.5,2.,footprint);
    vec2 cell=floor(coord);float nearest=10.,tone=0.,glints=0.;
    vec3 halfVector=normalize(l+v);
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
        ivec2 tile=ivec2(cell)+ivec2(x,y);
        uint key=hash32(uint(tile.x)*73856093u^uint(tile.y)*19349663u^identity*83492791u);
        vec3 random=vec3(randomValue(key),randomValue(key+1u),randomValue(key+2u));
        vec2 delta=coord-(vec2(tile)+.15+.7*random.xy);float distance=dot(delta,delta);
        if(distance<nearest){nearest=distance;tone=mix(.8,1.15,random.z);}
        float width=.15+min(footprint,1.5)*.32;
        float cover=exp(-distance*2./(width*width))*.0225/(width*width);
        vec3 facet=normalize(normal+tangent*(random.x-.5)*1.4+bitangent*(random.y-.5)*1.4);
        float reflection=pow(max(dot(normalize(rotation*facet),halfVector),0.),80.);
        float mineral=1.-smoothstep(mineralFraction-.03,mineralFraction+.03,random.z);
        glints+=cover*reflection*mineral*18.;
    }
    return vec2(mix(1.,tone,resolved),min(glints,3.)*resolved);
}
void main(){
    vec2 uv=(gl_FragCoord.xy-viewportOrigin)/resolution;
#ifdef PASS_DEPTH
    vec3 ray=normalize(viewPosition(uv,1.));
    mat3 rotation=mat3(view)*orientation(identity);
    mat3 inverseRotation=transpose(rotation);
    vec3 origin=inverseRotation*(-center/radius);
    vec3 hit,localNormal;
    if(!intersectGrain(origin,inverseRotation*ray,hit,localNormal))discard;
    vec3 surface=center+rotation*hit*radius;
    vec4 clip=projection*vec4(surface,1);gl_FragDepth=clip.z/clip.w*.5+.5;
    vec3 normal=normalize(rotation*localNormal);
    vec3 tangent=normalize(cross(abs(localNormal.z)<.8?vec3(0,0,1):vec3(0,1,0),localNormal));
    vec3 bitangent=cross(localNormal,tangent);
    vec2 coord=vec2(dot(hit,tangent),dot(hit,bitangent))*microScale;
    float footprint=max(length(dFdx(coord)),length(dFdy(coord)));
    result=vec4(normal,float(hitFace));visibleId=identity;grainHit=vec4(hit,footprint);return;
#endif
#ifdef PASS_SHADE
    ivec2 pixel=ivec2(gl_FragCoord.xy-viewportOrigin);
    float depth=texelFetch(grainDepth,pixel,0).r;if(depth>=1.)discard;
    gl_FragDepth=depth;
    identity=texelFetch(grainIds,pixel,0).r;
    vec4 hitData=texelFetch(grainHits,pixel,0);vec3 hit=hitData.xyz;storedFootprint=hitData.w;
    int face=int(texelFetch(grainNormals,pixel,0).w+.5);
    vec3 localNormal=vec3(0);
    if(face<6)localNormal[face/2]=(face%2==0)?1.:-1.;
    else{int k=face-6;localNormal=normalize(vec3(k%2==0?1.:-1.,(k/2)%2==0?1.:-1.,k<4?1.:-1.));}
    mat3 rotation=mat3(view)*orientation(identity);
    vec3 normal=normalize(rotation*localNormal);
    vec3 surface=viewPosition(uv,depth);
    vec3 light=normalize(mat3(view)*sunDirection),eye=normalize(-surface);
    vec3 macroNormal;float ao;neighborhood(surface,normal,uv,macroNormal,ao);
    float distanceBlend=smoothstep(4.,20.,radius*projection[1][1]*resolution.y/max(-surface.z,.01));
    vec3 shadingNormal=normalize(mix(macroNormal,normal,distanceBlend*.65));
    vec2 micro=microAppearance(hit,localNormal,rotation,light,eye);
    float palette=randomValue(identity+301u);
    vec3 base=sandColor*mix(.78,1.16,palette);
    float mineral=randomValue(identity+397u);
    base=mix(base,vec3(.78,.73,.61),smoothstep(.88,.98,mineral)*.45);
    base*=micro.x;
    float nl=max(dot(shadingNormal,light),0.);
    float visibility=nl>0.?contactShadow(surface,normal,light):1.;
    vec3 worldNormal=transpose(mat3(view))*shadingNormal;
    // Warm diffuse body with weak ambient fill approximates unresolved
    // inter-grain scattering. This is not the 2015 paper's diffusion solver.
    vec3 sky=mix(vec3(.13,.12,.10),vec3(.30,.33,.38),clamp(worldNormal.y*.5+.5,0.,1.));
    vec3 diffuse=base*(sky*ao+vec3(1.15,1.07,.90)*nl*visibility);
    float aggregate=ggx(macroNormal,light,eye,roughness);
    float fine=ggx(shadingNormal,light,eye,max(.2,roughness));
    float specular=mix(aggregate,fine,distanceBlend*.25);
    float glint=micro.y*sparkleStrength*max(dot(normal,light),0.);
    vec3 color=diffuse+vec3(1.,.96,.86)*(specular+glint)*visibility;
    // Bounded highlight compression retains visible bright flecks without
    // turning them into clipped, frame-dependent white pixels.
    result=vec4(color/(1.+color*.35),1);
#endif
}
#endif
