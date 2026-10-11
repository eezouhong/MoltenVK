#version 460
layout(set=0,binding=0,std430) buffer Output { uvec4 rows[]; } data;
layout(location=0) out vec4 color;
void main() { data.rows[15]=uvec4(0xbad00bad);color=vec4(0,1,0,1); }
