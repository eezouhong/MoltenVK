#version 460
layout(location=0) out vec4 color;
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
void main() {calculate(uint(gl_FragCoord.y)*16u+uint(gl_FragCoord.x));color=vec4(1);}
