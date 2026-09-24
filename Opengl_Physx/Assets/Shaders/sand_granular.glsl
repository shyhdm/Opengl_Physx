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
    identity=particleId;center=(view*vec4(position.xyz,1)).xyz;
    vec2 corner=vec2((gl_VertexID&1)==0?-1.:1.,(gl_VertexID&2)==0?-1.:1.);
    vec4 clip=projection*vec4(center,1);
    // Conservative bounding quad; unlike GL_POINTS, no driver point-size cap.
    float expand=max(-center.z,.01)/max(-center.z-radius,.01);
    clip.xy+=corner*vec2(projection[0][0],projection[1][1])*radius*expand;
    // Rasterize the bounding quad on the nearest sphere plane. Actual grain
    // depth can only be greater, enabling conservative early depth rejection.
    vec4 front=projection*vec4(center+vec3(0,0,radius),1);
    if(front.w>0.)clip.z=max(-clip.w,front.z/front.w*clip.w);
    if(center.z>radius)clip=vec4(2,2,2,1);
    gl_Position=clip;
}
#endif
#ifdef FRAGMENT_SHADER
layout(depth_greater) out float gl_FragDepth;
flat in vec3 center;
flat in uint identity;
out vec4 result;
uniform sampler2D grainDepth,grainNormals;
uniform vec3 sandColor,sunDirection;
uniform float roughness,sparkleStrength,mineralFraction,microScale,occlusionStrength;
const float PI=3.14159265359;

vec3 viewPosition(vec2 uv,float d){vec4 p=inverseProjection*vec4(uv*2.-1.,d*2.-1.,1);return p.xyz/p.w;}

bool intersectGrain(vec3 ro,vec3 rd,out vec3 hit,out vec3 normal){
    float entry=-1e20,exitDistance=1e20;vec3 face=vec3(0,0,1);
    float size=mix(.92,1.,randomValue(identity+43u));
    for(int i=0;i<14;++i){
        vec3 plane=vec3(0);float extent;
        if(i<6){int axis=i/2;plane[axis]=(i%2==0)?1.:-1.;extent=axis==0?.64:(axis==1?.55:.50);}
        else{int k=i-6;plane=normalize(vec3(k%2==0?1.:-1.,(k/2)%2==0?1.:-1.,k<4?1.:-1.));extent=.64;}
        extent*=size*mix(.90,1.,randomValue(identity+uint(i)*73u+101u));
        float denominator=dot(plane,rd),distance=extent-dot(plane,ro);
        if(abs(denominator)<1e-7){if(distance<0.)return false;}
        else{float t=distance/denominator;if(denominator<0.){if(t>entry){entry=t;face=plane;}}else exitDistance=min(exitDistance,t);}
    }
    if(entry>exitDistance||entry<0.)return false;
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
    float footprint=max(length(dFdx(coord)),length(dFdy(coord)));
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
#ifdef PASS_SHADE
    float nearest=texture(grainDepth,uv).r;
    if(nearest>=1.)discard;
    if(center.z+radius<viewPosition(uv,nearest).z-radius*.001)discard;
#endif
    vec3 ray=normalize(viewPosition(uv,1.));
    mat3 rotation=mat3(view)*orientation(identity),inverseRotation=transpose(rotation);
    vec3 hit,localNormal;
    if(!intersectGrain(inverseRotation*(-center/radius),inverseRotation*ray,hit,localNormal))discard;
    vec3 surface=center+rotation*hit*radius;
    vec4 clip=projection*vec4(surface,1);gl_FragDepth=clip.z/clip.w*.5+.5;
    vec3 normal=normalize(rotation*localNormal);
#ifdef PASS_DEPTH
    result=vec4(normal,1);return;
#endif
#ifdef PASS_SHADE
    if(gl_FragDepth>texture(grainDepth,uv).r)discard;
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
