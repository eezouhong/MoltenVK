#version 460
#extension GL_ARB_shader_draw_parameters : require
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } outputData;
layout(push_constant) uniform Params {
    uint destination; uint padding;
    int vertexOrigin[2]; uint instanceOrigin[2];
} params;
void main() {
    uint draw = uint(gl_DrawIDARB);
    int vertex = gl_VertexIndex - params.vertexOrigin[min(draw,1u)];
    uint instance = uint(gl_InstanceIndex) - params.instanceOrigin[min(draw,1u)];
    if (draw < 2u && vertex >= 0 && vertex < 3 && instance < 2u) {
        uint slot = params.destination + draw * 12u + instance * 6u + uint(vertex)*2u;
        // Triangle-fan conversion may invoke a shared vertex more than once.
        // Atomic stores avoid any write/write race in this test oracle.
        atomicExchange(outputData.rows[slot].x, uint(gl_VertexIndex));
        atomicExchange(outputData.rows[slot].y, uint(gl_InstanceIndex));
        atomicExchange(outputData.rows[slot].z, uint(gl_BaseVertexARB));
        atomicExchange(outputData.rows[slot].w, uint(gl_BaseInstanceARB));
        atomicExchange(outputData.rows[slot+1u].x, draw);
        atomicExchange(outputData.rows[slot+1u].y, 0xabcdef01u);
        atomicExchange(outputData.rows[slot+1u].z, uint(params.vertexOrigin[min(draw,1u)]));
        atomicExchange(outputData.rows[slot+1u].w, params.instanceOrigin[min(draw,1u)]);
    }
    vec2 positions[3] = vec2[3](vec2(-1,-1),vec2(1,-1),vec2(-1,1));
    gl_Position = vec4(positions[uint(gl_VertexIndex)%3u],0,1);
}
