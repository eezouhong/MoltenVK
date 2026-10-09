#version 460
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } data;
void main() {
    if (gl_VertexIndex==0) { atomicExchange(data.rows[0].x,1u); atomicExchange(data.rows[0].y,2u); atomicExchange(data.rows[0].z,3u); atomicExchange(data.rows[0].w,4u); }
    vec2 p[3]=vec2[3](vec2(-1,-1),vec2(1,-1),vec2(-1,1));
    gl_Position=vec4(p[uint(gl_VertexIndex)%3u],0,1);
}
