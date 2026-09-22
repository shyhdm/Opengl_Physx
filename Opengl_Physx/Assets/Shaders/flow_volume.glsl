#ifdef VERTEX_SHADER
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
#endif
#ifdef FRAGMENT_SHADER

uniform sampler2D sceneDepth;
uniform mat4 inverseViewProjection;
out vec4 fragmentColor;

uniform sampler3D volumeTexture;
uniform vec3 cameraPosition;
uniform vec3 boundsMinimum;
uniform vec3 boundsMaximum;
uniform float densityScale;
uniform float fireBrightness;
uniform int raySteps;
uniform int displayMode;

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
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0));
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 farPoint = inverseViewProjection * vec4(ndc, 1.0, 1.0);
    vec3 direction = normalize(farPoint.xyz / farPoint.w - cameraPosition);
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 scenePoint = inverseViewProjection * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    float sceneDistance = max(dot(scenePoint.xyz / scenePoint.w - cameraPosition, direction), 0.0);
    vec2 hit = intersectBox(cameraPosition, direction);
    float entryDistance = max(hit.x, 0.0);
    float exitDistance = min(hit.y, sceneDistance);
    if (exitDistance <= entryDistance)
    {
        discard;
    }

    if (displayMode == 1)
    {
        ivec3 size = textureSize(volumeTexture, 0);
        vec3 cellSize = (boundsMaximum - boundsMinimum) / vec3(size);
        vec3 gridPosition = (cameraPosition + direction * entryDistance - boundsMinimum) / cellSize;
        ivec3 cell = clamp(ivec3(floor(gridPosition)), ivec3(0), size - 1);
        ivec3 advance = ivec3(sign(direction));
        vec3 nextHit = vec3(1e30);
        vec3 interval = vec3(1e30);
        for (int axis = 0; axis < 3; ++axis)
        {
            if (abs(direction[axis]) > 1e-8)
            {
                float boundary = boundsMinimum[axis] +
                    float(cell[axis] + (advance[axis] > 0 ? 1 : 0)) * cellSize[axis];
                nextHit[axis] = (boundary - cameraPosition[axis]) / direction[axis];
                interval[axis] = abs(cellSize[axis] / direction[axis]);
            }
        }
        vec3 normal = -direction;
        float distanceAlongRay = entryDistance;
        int limit = size.x + size.y + size.z + 3;
        for (int index = 0; index < limit; ++index)
        {
            if (distanceAlongRay >= exitDistance || any(lessThan(cell, ivec3(0))) ||
                any(greaterThanEqual(cell, size))) break;
            float boundary = min(nextHit.x, min(nextHit.y, nextHit.z));
            vec2 volume = texelFetch(volumeTexture, cell, 0).rg;
            if (min(boundary, exitDistance) > distanceAlongRay + 1e-6 &&
                (volume.r > 0.02 || volume.g > 0.10))
            {
                float heat = 1.0 - exp(-max(volume.g, 0.0) * 0.35);
                vec3 color = mix(vec3(0.22), fireColor(heat), smoothstep(0.0, 0.25, heat));
                float lighting = 0.55 + 0.45 * max(dot(normal, normalize(vec3(0.4, 0.8, 0.6))), 0.0);
                fragmentColor = vec4(color * lighting, 1.0);
                return;
            }
            distanceAlongRay = boundary;
            normal = vec3(0.0);
            for (int axis = 0; axis < 3; ++axis)
            {
                if (nextHit[axis] <= boundary)
                {
                    cell[axis] += advance[axis];
                    nextHit[axis] += interval[axis];
                    normal = vec3(0.0);
                    normal[axis] = -float(advance[axis]);
                }
            }
        }
        discard;
    }

    vec3 extent = max(boundsMaximum - boundsMinimum, vec3(0.0001));
    vec3 cellsPerUnit = abs(direction) * vec3(textureSize(volumeTexture, 0)) / extent;
    float cellRate = max(cellsPerUnit.x, max(cellsPerUnit.y, cellsPerUnit.z));
    int count = max(clamp(raySteps, 32, 256), int(ceil((exitDistance - entryDistance) * cellRate * 2.0)));
    float stepLength = (exitDistance - entryDistance) / float(count);
    vec4 accumulated = vec4(0.0);

    for (int index = 0; index < count; ++index)
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
        float temperature = max(volume.g, 0.0);
        // Smooth transfer retains differences above the old 2.86 temperature ceiling.
        float heat = 1.0 - exp(-temperature * 0.35);
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
