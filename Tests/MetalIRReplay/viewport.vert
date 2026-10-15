#version 460
#extension GL_ARB_shader_viewport_layer_array : require
layout(location=0) flat out vec4 color;
void main() {
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
    gl_ViewportIndex=gl_InstanceIndex;
    color=gl_InstanceIndex==0?vec4(1,0,0,1):vec4(0,1,0,1);
}
