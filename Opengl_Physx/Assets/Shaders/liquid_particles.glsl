uniform mat4 view;
uniform mat4 projection;
uniform float radius;
uniform float viewportHeight;
uniform float region;
uniform float granular;
uniform float sandRender;
uniform float sandShapeScale;
float hashGrain(float n){return fract(sin(n*12.9898+78.233)*43758.5453);}
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
out vec3 viewCenter;
flat out float grainSeed;
flat out float shapeSeed;
void main(){shapeSeed=hashGrain(float(gl_VertexID));grainSeed=.5+(sin(dot(position.xyz,vec3(12.9898,7.233,5.17)))+sin(dot(position.xyz,vec3(-4.13,15.71,9.23)))+sin(dot(position.xyz,vec3(8.31,-6.73,13.41))))/6.;viewCenter=(view*vec4(position.xyz,1)).xyz;gl_Position=projection*vec4(viewCenter,1);gl_PointSize=clamp(viewportHeight*projection[1][1]*radius*(sandRender>.5?sandShapeScale:1.)/max(-viewCenter.z,.01),1,256);}
#endif
#ifdef FRAGMENT_SHADER
in vec3 viewCenter;
flat in float grainSeed;
flat in float shapeSeed;
out vec4 outputColor;
void main(){
if(region>.5){outputColor=region>1.5?vec4(.1,.85,1,1):vec4(1,.7,.15,1);gl_FragDepth=gl_FragCoord.z;return;}
vec2 p=gl_PointCoord*2-1;float r=dot(p,p);if(r>1)discard;
if(sandRender>.5){
    // Filter fine detail as grains approach pixel size, rather than adding frame noise.
    float footprint=max(length(fwidth(p)),.001);
    float detail=1.-smoothstep(.08,.6,footprint);
    // Intersect a rotated convex grain, so silhouette, depth and normals agree.
    // Shape stays fixed while the particle occupies the same buffer slot.
    float a=shapeSeed*6.2831853;
    float b=hashGrain(shapeSeed*97.+3.)*6.2831853;
    mat3 rz=mat3(cos(a),sin(a),0.,-sin(a),cos(a),0.,0.,0.,1.);
    mat3 ry=mat3(cos(b),0.,-sin(b),0.,1.,0.,sin(b),0.,cos(b));
    mat3 localToView=mat3(view)*rz*ry;
    mat3 viewToLocal=transpose(localToView);
    vec3 ro=viewToLocal*vec3(p.x,-p.y,1.5);
    vec3 rd=viewToLocal*vec3(0.,0.,-1.);
    float enter=-1e6, leave=1e6;
    vec3 hitNormal=vec3(0.,0.,1.);
    float size=mix(.9,1.,hashGrain(shapeSeed*31.+7.));
    for(int i=0;i<14;++i){
        vec3 plane;
        float extent;
        if(i<6){
            int axis=i/2;
            plane=vec3(0.);plane[axis]=(i%2==0)?1.:-1.;
            extent=(axis==0?.64:(axis==1?.55:.50));
        }else{
            int k=i-6;
            plane=normalize(vec3((k%2==0)?1.:-1.,((k/2)%2==0)?1.:-1.,(k<4)?1.:-1.));
            extent=.64;
        }
        extent*=size*mix(.9,1.,hashGrain(shapeSeed*127.+float(i)*17.));
        float denom=dot(plane,rd);
        float distanceToPlane=extent-dot(plane,ro);
        if(abs(denom)<1e-6){if(distanceToPlane<0.)discard;}
        else{
            float t=distanceToPlane/denom;
            if(denom<0.){if(t>enter){enter=t;hitNormal=plane;}}
            else leave=min(leave,t);
        }
    }
    if(enter>leave||leave<0.)discard;
    vec3 localHit=ro+rd*enter;
    // Slightly soften facets without turning the grain back into a sphere.
    vec3 localNormal=normalize(mix(hitNormal,normalize(localHit),.18));
    vec3 n=normalize(localToView*localNormal);
    vec3 nw=transpose(mat3(view))*n;
    vec3 facets=normalize(floor(nw*5.+vec3(grainSeed))+.5-vec3(grainSeed));
    nw=normalize(mix(nw,facets,.65*detail));
    n=normalize(mat3(view)*nw);
    vec3 surface=viewCenter+localToView*localHit*radius*sandShapeScale;
    vec4 clip=projection*vec4(surface,1);gl_FragDepth=clip.z/clip.w*.5+.5;
    vec3 light=normalize(mat3(view)*normalize(vec3(-.4,.8,.5)));
    vec3 eye=normalize(-surface);
    float nl=max(dot(n,light),0.);
    vec3 base=mix(vec3(.30,.19,.085),vec3(.72,.53,.28),grainSeed);
    float mineral=hashGrain(grainSeed*719.);
    base=mix(base,vec3(.79,.74,.61),.65*smoothstep(.84,.9,mineral));
    base*=mix(.45,1.,smoothstep(.02,.07,mineral));
    float back=pow(max(dot(light,eye),0.),6.);
    float ambient=.24+.12*max(nw.y,0.);
    float diffuse=nl*(.62+.12*back);
    vec3 halfVector=normalize(light+eye);
    float broad=pow(max(dot(n,halfVector),0.),12.)*.035;
    float glint=pow(max(dot(n,halfVector),0.),72.)*detail*step(.82,mineral)*.16;
    outputColor=vec4(base*(ambient+diffuse)+vec3(broad+glint)*nl,1);
    return;
}
vec3 n=vec3(p.x,-p.y,sqrt(1-r));vec3 surface=viewCenter+n*radius;vec4 clip=projection*vec4(surface,1);gl_FragDepth=clip.z/clip.w*.5+.5;
float diffuse=max(dot(n,normalize(vec3(-.4,.7,.6))),0);float spec=pow(max(dot(n,normalize(vec3(-.2,.3,1))),0),40);
vec3 color=granular>.5?vec3(.68,.46,.20):vec3(.025,.28,.65);
outputColor=vec4(color*(.3+.7*diffuse)+vec3(granular>.5?.08:.65)*spec,1);
}
#endif
