#ifdef VERTEX_SHADER

layout(location = 0) in vec3 position;

uniform mat4 view;
uniform mat4 projection;
uniform vec3 boundsMinimum;
uniform vec3 boundsMaximum;

out vec3 worldPosition;

void main()
{
    worldPosition = mix(boundsMinimum, boundsMaximum, position);
    gl_Position = projection * view * vec4(worldPosition, 1.0);
}

#endif

#ifdef FRAGMENT_SHADER

in vec3 worldPosition;
out vec4 fragmentColor;

uniform sampler3D volumeTexture;
uniform vec3 cameraPosition;
uniform vec3 boundsMinimum;
uniform vec3 boundsMaximum;
uniform float densityScale;
uniform float fireBrightness;
uniform int raySteps;

vec2 intersectBox(vec3 origin, vec3 direction)
{
    vec3 safeDirection = vec3(
        abs(direction.x) < 0.00001 ? (direction.x < 0.0 ? -0.00001 : 0.00001) : direction.x,
        abs(direction.y) < 0.00001 ? (direction.y < 0.0 ? -0.00001 : 0.00001) : direction.y,
        abs(direction.z) < 0.00001 ? (direction.z < 0.0 ? -0.00001 : 0.00001) : direction.z
    );
    vec3 inverseDirection = 1.0 / safeDirection;
    vec3 first = (boundsMinimum - origin) * inverseDirection;
    vec3 second = (boundsMaximum - origin) * inverseDirection;
    vec3 nearValue = min(first, second);
    vec3 farValue = max(first, second);
    return vec2(
        max(max(nearValue.x, nearValue.y), nearValue.z),
        min(min(farValue.x, farValue.y), farValue.z)
    );
}

vec3 fireColor(float heat)
{
    vec3 deepRed = vec3(1.0, 0.035, 0.005);
    vec3 orange = vec3(1.0, 0.32, 0.015);
    vec3 yellow = vec3(1.0, 0.92, 0.32);
    vec3 whiteHot = vec3(1.0, 1.0, 0.92);
    vec3 low = mix(deepRed, orange, smoothstep(0.05, 0.40, heat));
    vec3 high = mix(yellow, whiteHot, smoothstep(0.70, 1.0, heat));
    return mix(low, high, smoothstep(0.35, 0.78, heat));
}

void main()
{
    vec3 direction = normalize(worldPosition - cameraPosition);
    vec2 hit = intersectBox(cameraPosition, direction);
    float entryDistance = max(hit.x, 0.0);
    float exitDistance = hit.y;
    if (exitDistance <= entryDistance)
    {
        discard;
    }

    int count = clamp(raySteps, 32, 256);
    float stepLength = (exitDistance - entryDistance) / float(count);
    vec3 extent = max(boundsMaximum - boundsMinimum, vec3(0.0001));
    vec4 accumulated = vec4(0.0);

    for (int index = 0; index < 256; ++index)
    {
        if (index >= count || accumulated.a > 0.985)
        {
            break;
        }

        float distanceAlongRay = entryDistance + (float(index) + 0.5) * stepLength;
        vec3 samplePosition = cameraPosition + direction * distanceAlongRay;
        vec3 texturePosition = (samplePosition - boundsMinimum) / extent;
        vec2 volume = texture(volumeTexture, texturePosition).rg;

        float smoke = max(volume.r, 0.0);
        float heat = max(volume.g, 0.0);
        float density = densityScale * (smoke + heat * 0.32);
        float sampleAlpha = 1.0 - exp(-density * stepLength * 1.8);

        vec3 emission = fireColor(heat) * heat * fireBrightness;
        vec3 soot = vec3(0.055, 0.06, 0.065) * smoke;
        vec3 sampleColor = emission + soot;

        float remaining = 1.0 - accumulated.a;
        accumulated.rgb += remaining * sampleColor * sampleAlpha;
        accumulated.a += remaining * sampleAlpha;
    }

    if (accumulated.a < 0.002)
    {
        discard;
    }
    fragmentColor = accumulated;
}

#endif
