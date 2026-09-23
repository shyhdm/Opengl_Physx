uniform mat4 view;
uniform mat4 projection;
uniform vec3 eye;
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
layout(location=1) in vec4 normal;
out vec3 worldPosition;
out vec3 worldNormal;
void main(){worldPosition=position.xyz;worldNormal=normal.xyz;gl_Position=projection*view*vec4(position.xyz,1);}
#endif
#ifdef FRAGMENT_SHADER
in vec3 worldPosition;
in vec3 worldNormal;
out vec4 outputColor;
void main(){
vec3 n=normalize(worldNormal);if(!gl_FrontFacing)n=-n;
vec3 v=normalize(eye-worldPosition);vec3 l=normalize(vec3(-.4,.9,.3));
float fresnel=.02+.98*pow(1-clamp(dot(n,v),0,1),5);
vec3 reflected=reflect(-v,n);
vec3 sky=mix(vec3(.055,.09,.13),vec3(.5,.66,.8),smoothstep(-.15,.7,reflected.y));
vec3 water=vec3(.025,.22,.34)*(.4+.6*max(dot(n,l),0));
float specular=pow(max(dot(n,normalize(l+v)),0),160);
vec3 color=mix(water,sky,fresnel)+vec3(1,.96,.87)*specular*.85;
outputColor=vec4(color,1);
}
#endif
