#version 450

layout(set = 0, binding = 0) uniform ColorBlock {
    vec4 value;
} colorBlock;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = colorBlock.value;
}
