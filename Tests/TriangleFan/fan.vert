#version 460
#extension GL_ARB_shader_draw_parameters : require
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } result;
layout(push_constant) uniform Params { uint first; uint count; uint instanceBase; uint pad; } params;
void main() {
    uint relative = uint(gl_VertexIndex) - params.first;
    uint instance = uint(gl_InstanceIndex) - params.instanceBase;
    if (relative < params.count && instance < 2u) {
        uint slot = instance * params.count + relative;
        atomicExchange(result.rows[slot].x, uint(gl_VertexIndex));
        atomicExchange(result.rows[slot].y, uint(gl_BaseVertexARB));
        atomicExchange(result.rows[slot].z, uint(gl_InstanceIndex));
        atomicExchange(result.rows[slot].w, uint(gl_BaseInstanceARB));
    }
    vec2 corners[3] = vec2[3](vec2(-1,-1),vec2(1,-1),vec2(-1,1));
    gl_Position = vec4(corners[relative % 3u], 0, 1);
}
