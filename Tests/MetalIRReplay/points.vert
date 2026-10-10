#version 460
layout(push_constant) uniform Params { vec2 center; float size; uint triangle; } params;
void main() {
    vec2 corners[3] = vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position = vec4(params.triangle != 0 ? corners[gl_VertexIndex] : params.center,0.5,1);
    gl_PointSize = params.size;
}
