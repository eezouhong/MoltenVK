#version 450
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } outputData;
layout(push_constant) uniform Params {
    uint destination; int firstVertex; uint firstInstance; uint marker;
} params;
void main() {
    int vertex=gl_VertexIndex-params.firstVertex;
    uint instance=uint(gl_InstanceIndex)-params.firstInstance;
    if(vertex>=0 && vertex<3 && instance<2u) {
        uint slot=params.destination+instance*6u+uint(vertex)*2u;
        outputData.rows[slot]=uvec4(uint(gl_VertexIndex),uint(gl_InstanceIndex),params.marker,0xabcdef01u);
        outputData.rows[slot+1u]=uvec4(uint(vertex),instance,params.destination,0x12345678u);
    }
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(1,-1),vec2(-1,1));
    gl_Position=vec4(positions[uint(vertex)%3u],0,1);
}
