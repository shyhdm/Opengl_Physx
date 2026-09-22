#ifdef VERTEX_SHADER
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexID], 0, 1);
}
#endif
#ifdef FRAGMENT_SHADER
uniform samplerBuffer nativeColor;
uniform int imageWidth;
out vec4 fragmentColor;
void main()
{
    fragmentColor = texelFetch(nativeColor, int(gl_FragCoord.y) * imageWidth + int(gl_FragCoord.x));
}
#endif
