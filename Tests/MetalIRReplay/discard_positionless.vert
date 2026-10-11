#version 460
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } data;
layout(push_constant) uniform Params { uint destination;uint origin; } params;
void main() {
    uint i=uint(gl_VertexIndex)-params.origin;
    if(i<3u) {
        vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
        data.rows[params.destination+i]=floatBitsToUint(vec4(positions[i],0,1));
    }
    // Position is not required when rasterizer discard is enabled.
}
