uniform mat4 view;
uniform mat4 projection;
uniform float radius;
uniform float viewportHeight;
uniform float region;
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
out vec3 viewCenter;
void main(){viewCenter=(view*vec4(position.xyz,1)).xyz;gl_Position=projection*vec4(viewCenter,1);gl_PointSize=clamp(viewportHeight*projection[1][1]*radius/max(-viewCenter.z,.01),1,256);}
#endif
#ifdef FRAGMENT_SHADER
in vec3 viewCenter;
out vec4 outputColor;
void main(){
if(region>.5){outputColor=region>1.5?vec4(.1,.85,1,1):vec4(1,.7,.15,1);gl_FragDepth=gl_FragCoord.z;return;}
vec2 p=gl_PointCoord*2-1;float r=dot(p,p);if(r>1)discard;
vec3 n=vec3(p.x,-p.y,sqrt(1-r));vec3 surface=viewCenter+n*radius;vec4 clip=projection*vec4(surface,1);gl_FragDepth=clip.z/clip.w*.5+.5;
float diffuse=max(dot(n,normalize(vec3(-.4,.7,.6))),0);float spec=pow(max(dot(n,normalize(vec3(-.2,.3,1))),0),40);
outputColor=vec4(vec3(.025,.28,.65)*(.3+.7*diffuse)+vec3(.65)*spec,1);
}
#endif
