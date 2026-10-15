#version 460
layout(set=0,binding=0,std430) readonly buffer Input { uvec4 rows[]; } data;
layout(push_constant) uniform Params { uint source;uint unused; } params;
void main() { gl_Position=uintBitsToFloat(data.rows[params.source+uint(gl_VertexIndex)]); }
