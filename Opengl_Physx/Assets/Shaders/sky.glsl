uniform mat4 inverseProjection,inverseView;
uniform samplerCube environmentMap;
uniform vec3 sunDirection;
uniform int face;
#ifdef VERTEX_SHADER
out vec2 uv;
void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2-1,1,1);}
#endif
#ifdef FRAGMENT_SHADER
in vec2 uv;out vec4 color;
#ifdef PASS_BAKE
float hash(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
float noise(vec3 p){
 vec3 i=floor(p),f=fract(p);f=f*f*(3-2*f);
 return mix(mix(mix(hash(i),hash(i+vec3(1,0,0)),f.x),mix(hash(i+vec3(0,1,0)),hash(i+vec3(1,1,0)),f.x),f.y),
            mix(mix(hash(i+vec3(0,0,1)),hash(i+vec3(1,0,1)),f.x),mix(hash(i+vec3(0,1,1)),hash(i+vec3(1,1,1)),f.x),f.y),f.z);
}
float clouds(vec3 p){float n=0,w=.55;for(int i=0;i<5;++i){n+=noise(p)*w;p=p*2.03+vec3(5.2,1.3,8.7);w*=.48;}return n;}
void main(){
 vec2 p=uv*2-1;vec3 d;
 if(face==0)d=vec3(1,-p.y,-p.x);else if(face==1)d=vec3(-1,-p.y,p.x);
 else if(face==2)d=vec3(p.x,1,p.y);else if(face==3)d=vec3(p.x,-1,-p.y);
 else if(face==4)d=vec3(p.x,-p.y,1);else d=vec3(-p.x,-p.y,-1);
 d=normalize(d);vec3 sun=normalize(sunDirection);
 float elevation=pow(clamp(d.y,0,1),.45);
 vec3 sky=mix(vec3(.72,.82,.91),vec3(.16,.38,.69),elevation);
 float facing=max(dot(d,sun),0);
 sky+=vec3(.23,.18,.1)*pow(facing,12);
 float cloud=smoothstep(.44,.66,clouds(d*5.5+vec3(0,2,0)))*smoothstep(.015,.18,d.y);
 vec3 cloudColor=mix(vec3(.60,.66,.73),vec3(.98,.97,.94),smoothstep(.2,.85,clouds(d*5.5+vec3(0,2,0))));
 sky=mix(sky,cloudColor,cloud*.92);
 // A softly filtered solar disc; its direction follows the scene light.
 sky+=vec3(1,.91,.72)*(.7*pow(facing,128)+2.5*smoothstep(.99965,.99993,facing))*(1-cloud*.85);
 sky=mix(vec3(.19,.22,.25),sky,smoothstep(-.18,.015,d.y));
 color=vec4(sky,1);
}
#else
void main(){vec4 p=inverseProjection*vec4(uv*2-1,1,1);vec3 d=mat3(inverseView)*normalize(p.xyz/p.w);color=vec4(texture(environmentMap,d).rgb,1);}
#endif
#endif
