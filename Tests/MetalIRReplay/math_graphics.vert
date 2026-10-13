#version 460
layout(set=0,binding=0,std430) buffer Data { uint words[]; } data;
void calculate(uint index) {
    uint i=index*3;
    float a=uintBitsToFloat(data.words[i]);
    precise float product=a*1.00000012;
    precise float exact=product+0.5;
    float ordinary=(a*a+0.25)/(a+1.0);
    atomicExchange(data.words[i+1],floatBitsToUint(exact));
    atomicExchange(data.words[i+2],floatBitsToUint(ordinary+exact));
}
void main() {
    if(gl_VertexIndex==0) calculate(uint(gl_InstanceIndex));
    vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
}
