uniform mat4 lightView,lightProjection,inverseLightView,inverseLightProjection,inverseProjection,inverseView;
uniform vec3 regionCenter;
uniform float span,radius;
uniform vec2 resolution,axis;
uniform sampler2D lightDepth,lightThickness,sceneDepth,sceneColor,causticMap;
#if defined(PASS_DEPTH) || defined(PASS_THICKNESS)
#ifdef VERTEX_SHADER
layout(location=0) in vec4 position;
out vec2 local;flat out vec3 center;
void main(){local=vec2((gl_VertexID&1)==0?-1:1,(gl_VertexID&2)==0?-1:1);center=(lightView*vec4(position.xyz,1)).xyz;gl_Position=lightProjection*vec4(center+vec3(local*radius,radius),1);}
#endif
#ifdef FRAGMENT_SHADER
in vec2 local;flat in vec3 center;out vec4 result;
void main(){float r2=dot(local,local);if(r2>=1)discard;float chord=sqrt(1-r2)*radius;
#ifdef PASS_DEPTH
vec4 clip=lightProjection*vec4(center+vec3(0,0,chord),1);gl_FragDepth=clip.z/clip.w*.5+.5;result=vec4(-center.z-chord,0,0,0);
#else
float w=(exp(-2*r2)-exp(-2.0))/(1-exp(-2.0));result=vec4(chord*.92*w,0,0,0);
#endif
}
#endif
#elif defined(PASS_PHOTONS)
#ifdef VERTEX_SHADER
out vec2 local;flat out vec3 energy;
vec3 positionAt(vec2 q,float d){vec4 v=inverseLightProjection*vec4(q*2-1,0,1);return vec3(v.xy,-d);}
void main(){
local=vec2((gl_VertexID&1)==0?-1:1,(gl_VertexID&2)==0?-1:1);
ivec2 dims=textureSize(lightDepth,0);vec2 q=(vec2(gl_InstanceID%dims.x,gl_InstanceID/dims.x)+.5)/vec2(dims);float d=texture(lightDepth,q).r;
if(d<=0){gl_Position=vec4(2,2,2,1);energy=vec3(0);return;}
vec2 pixel=1.0/vec2(dims);float a=texture(lightDepth,q-vec2(pixel.x,0)).r,b=texture(lightDepth,q+vec2(pixel.x,0)).r;
float c=texture(lightDepth,q-vec2(0,pixel.y)).r,e=texture(lightDepth,q+vec2(0,pixel.y)).r;
vec3 p=positionAt(q,d);vec3 dx=b>0&&(a<=0||abs(b-d)<abs(a-d))?positionAt(q+vec2(pixel.x,0),b)-p:p-positionAt(q-vec2(pixel.x,0),a>0?a:d);
vec3 dy=e>0&&(c<=0||abs(e-d)<abs(c-d))?positionAt(q+vec2(0,pixel.y),e)-p:p-positionAt(q-vec2(0,pixel.y),c>0?c:d);
vec3 n=normalize(mat3(inverseLightView)*normalize(cross(dx,dy)));vec3 incoming=-normalize(vec3(-.4,.8,.3));if(dot(n,incoming)>0)n=-n;
vec3 ray=refract(incoming,n,1/1.333);vec3 world=(inverseLightView*vec4(p,1)).xyz;
if(ray.y>=-.01||world.y<=0){gl_Position=vec4(2,2,2,1);energy=vec3(0);return;}
vec3 hit=world-ray*(world.y/ray.y);vec2 screen=(hit.xz-regionCenter.xz)/(span*1.5);
float bulk=texture(lightThickness,q).r;energy=exp(-vec3(.52,.13,.065)*bulk)*.018;
gl_Position=vec4(screen+local*(4.0/512.0),0,1);
}
#endif
#ifdef FRAGMENT_SHADER
in vec2 local;flat in vec3 energy;out vec4 result;void main(){float r=dot(local,local);if(r>1)discard;result=vec4(energy*exp(-3*r),1);}
#endif
#else
#ifdef VERTEX_SHADER
out vec2 uv;void main(){uv=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(uv*2-1,0,1);}
#endif
#ifdef FRAGMENT_SHADER
in vec2 uv;out vec4 result;
#ifdef PASS_SMOOTH
void main(){float d=texture(lightDepth,uv).r;if(d<=0){result=vec4(0);return;}float sum=0,total=0;float k=clamp(radius/span*512*2,1,12);for(int i=-12;i<=12;++i){if(abs(float(i))>k)continue;float v=texture(lightDepth,clamp(uv+axis*float(i)/512.0,vec2(.5/512),vec2(1-.5/512))).r;if(v<=0)continue;float w=exp(-float(i*i)/max(k*k*.5,1)-pow((v-d)/(radius*1.5),2));sum+=v*w;total+=w;}result=vec4(sum/max(total,1e-6),0,0,0);}
#else
void main(){
vec3 color=texture(sceneColor,uv).rgb;float depth=texture(sceneDepth,uv).r;if(depth>=1){result=vec4(color,1);return;}
vec4 v=inverseProjection*vec4(uv*2-1,depth*2-1,1);vec3 world=(inverseView*vec4(v.xyz/v.w,1)).xyz;
vec4 light=lightView*vec4(world,1),clip=lightProjection*light;vec2 q=clip.xy*.5+.5;
if(all(greaterThan(q,vec2(.002)))&&all(lessThan(q,vec2(.998)))){
vec3 transmission=vec3(0);for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){vec2 sampleUv=q+vec2(x,y)/512.0;float front=texture(lightDepth,sampleUv).r,bulk=texture(lightThickness,sampleUv).r;float passed=clamp((-light.z-front)/max(bulk,.001),0,1);transmission+=front>0&&-light.z>front+radius*.3?exp(-vec3(.52,.13,.065)*bulk*passed):vec3(1);}
color*=.35+.65*transmission/9;
}
vec2 c=(world.xz-regionCenter.xz)/(span*3)+.5;
if(abs(world.y)<.035&&all(greaterThan(c,vec2(.002)))&&all(lessThan(c,vec2(.998))))color+=texture(causticMap,c).rgb*color;
result=vec4(color,1);
}
#endif
#endif
#endif
