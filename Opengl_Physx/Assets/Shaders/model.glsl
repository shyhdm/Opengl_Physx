// =================== Vertex ===================
#if defined(VERTEX_SHADER)
layout(location = 0) in vec3 position;
layout(location = 4) in mat4 instanceModel;
layout(location = 8) in float transformIndex;
uniform mat4 model;
uniform bool instanced;
uniform bool indexedTransforms;
uniform samplerBuffer transformMatrices;
mat4 GetIndexedModel()
{
    int first = int(transformIndex + 0.5) * 4;
    return mat4(texelFetch(transformMatrices,first),texelFetch(transformMatrices,first+1),texelFetch(transformMatrices,first+2),texelFetch(transformMatrices,first+3));
}
mat4 GetObjectModel() { return indexedTransforms ? GetIndexedModel() : (instanced ? instanceModel : model); }
uniform mat4 lightSpaceMatrix;
#if defined(PASS_COLLISION)
layout(location=1) in vec3 color;
uniform mat4 mvp;
out vec3 collisionColor;
uniform bool overrideCollisionColor;
uniform vec3 collisionTint;
void main() { collisionColor=overrideCollisionColor?collisionTint:color; gl_Position=mvp*vec4(position,1.0); }
#elif defined(PASS_OUTLINE)
void main()
{
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
#elif defined(PASS_SELECTION)
uniform mat4 mvp;
void main() { gl_Position=mvp*vec4(position,1.0); }
#elif defined(PASS_SHADOW)
void main() { gl_Position = lightSpaceMatrix * GetObjectModel() * vec4(position,1.0); }
#else
layout(location = 2) in vec3 normal;
uniform bool softGpu;
uniform samplerBuffer softPositions;
uniform usamplerBuffer softRanges;
uniform usamplerBuffer softFaces;

vec3 GetNormal()
{
    if(!softGpu) return normal;
    uvec2 range=texelFetch(softRanges,gl_VertexID).rg;
    vec3 sum=vec3(0.0);
    for(uint j=0u;j<range.y;++j)
    {
        int offset=int(range.x+j*3u);
        int a=int(texelFetch(softFaces,offset).r);
        int b=int(texelFetch(softFaces,offset+1).r);
        int c=int(texelFetch(softFaces,offset+2).r);
        vec3 p=texelFetch(softPositions,a).xyz;
        vec3 q=texelFetch(softPositions,b).xyz;
        vec3 r=texelFetch(softPositions,c).xyz;
        sum+=cross(q-p,r-p);
    }
    return dot(sum,sum)>1e-16 ? normalize(sum) : vec3(0,1,0);
}
layout(location = 3) in vec2 texCoord;
uniform mat4 mvp;
out VS_OUT
{
    vec4 lightPosition;
    vec2 uv;
    vec3 worldPosition;
    vec3 worldNormal;
} v;
void main()
{
    mat4 objectModel = GetObjectModel();
    vec4 worldPosition = objectModel * vec4(position,1.0);
    v.lightPosition = lightSpaceMatrix * worldPosition;
    v.uv = texCoord;
    v.worldPosition = worldPosition.xyz;
    v.worldNormal = transpose(inverse(mat3(objectModel))) * GetNormal();
    gl_Position = (instanced || indexedTransforms) ? (mvp * objectModel * vec4(position,1.0)) : (mvp * vec4(position,1.0));
}
#endif
#endif

// =================== Fragment ===================
#if defined(FRAGMENT_SHADER)
#if defined(PASS_COLLISION)
in vec3 collisionColor;
out vec4 outputColor;
void main() { outputColor=vec4(collisionColor,1.0); }
#elif defined(PASS_SELECTION)
out vec4 outputColor;
void main() { outputColor=vec4(1.0); }
#elif defined(PASS_OUTLINE)
uniform sampler2D selectionMask;
uniform sampler2D selectionDepth;
uniform sampler2D sceneDepth;
uniform vec3 outlineColor;
out vec4 outputColor;
void main()
{
    ivec2 p=ivec2(gl_FragCoord.xy);
    ivec2 size=textureSize(selectionMask,0);
    if(texelFetch(selectionMask,p,0).r>0.5) discard;
    float edge=0.0;
    for(int x=-2;x<=2;++x)
        for(int y=-2;y<=2;++y)
        {
            if(x*x+y*y>4) continue;
            ivec2 q=p+ivec2(x,y);
            if(any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,size))) continue;
            if(texelFetch(selectionMask,q,0).r<0.5) continue;
            float objectDepth=texelFetch(selectionDepth,q,0).r;
            // 不把遮挡截断处当轮廓，也不把向外扩展的线画到前景物体上。
            const float tolerance=0.000002;
            if(objectDepth>texelFetch(sceneDepth,q,0).r+tolerance) continue;
            if(objectDepth>texelFetch(sceneDepth,p,0).r+tolerance) continue;
            edge=1.0;
        }
    if(edge<0.5) discard;
    outputColor=vec4(outlineColor,1.0);
}
#elif defined(PASS_SHADOW)
void main() {}
#else
in VS_OUT
{

    vec4 lightPosition;
    vec2 uv;
    vec3 worldPosition;
    vec3 worldNormal;
} i;

uniform sampler2D baseTexture;
uniform bool hasBaseTexture;
uniform vec2 textureTiling;
uniform sampler2D shadowMap;
uniform bool hasShadowMap;
uniform float shadowStrength;
uniform float shadowBias;
uniform int shadowFilterRadius;
uniform vec3 materialColor;
uniform vec3 cameraPosition;
uniform vec3 lightDirection;
uniform vec3 lightColor;
uniform float ambientStrength;
uniform float diffuseStrength;
uniform float specularStrength;
uniform float shininess;

out vec4 outputColor;

vec3 SafeNormalize(vec3 value)
{
    return value * inversesqrt(max(dot(value, value), 0.00000001));
}

float CalculateShadow(vec3 N, vec3 L)
{
    if (!hasShadowMap || i.lightPosition.w <= 0.0) return 0.0;
    vec3 projected = i.lightPosition.xyz / i.lightPosition.w * 0.5 + 0.5;
    if (any(lessThan(projected, vec3(0.0))) || any(greaterThan(projected, vec3(1.0)))) return 0.0;
    float bias = max(max(shadowBias, 0.0) * (1.0 - max(dot(N,L),0.0)), max(shadowBias, 0.0003) * 0.5);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap,0));
    int radius = clamp(shadowFilterRadius,0,3);
    float shadow = 0.0, samples = 0.0;
    for (int x=-3; x<=3; ++x)
        for (int y=-3; y<=3; ++y)
        {
            if (abs(x)>radius || abs(y)>radius) continue;
            float depth = texture(shadowMap,projected.xy+vec2(x,y)*texel).r;
            shadow += projected.z-bias > depth ? 1.0 : 0.0;
            samples += 1.0;
        }
    return shadow/samples*clamp(shadowStrength,0.0,1.0);
}
void main()
{
    vec3 N = SafeNormalize(i.worldNormal);
    if (!gl_FrontFacing) N = -N;

    // lightDirection 是光线传播方向，L 指向光源。
    vec3 L = SafeNormalize(-lightDirection);
    vec3 V = SafeNormalize(cameraPosition - i.worldPosition);
    vec3 H = SafeNormalize(L + V);

    vec3 baseColor = max(materialColor, vec3(0.0));
    if (hasBaseTexture) baseColor *= texture(baseTexture, i.uv * textureTiling).rgb;
    vec3 illumination = max(lightColor, vec3(0.0));

    vec3 ambient = max(ambientStrength, 0.0) * baseColor * illumination;

    // 标准 Lambert 漫反射：背光面没有直接光。
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = max(diffuseStrength, 0.0) * NdotL * baseColor * illumination;

    // Blinn-Phong 高光：光照面才产生高光，不乘物体底色。
    float highlight = NdotL > 0.0 ? pow(max(dot(N, H), 0.0), max(shininess, 1.0)) : 0.0;
    vec3 specular = max(specularStrength, 0.0) * highlight * illumination;

    float shadow = CalculateShadow(N,L);
    outputColor = vec4(ambient + (1.0-shadow)*(diffuse+specular), 1.0);
}

#endif
#endif
