#version 450
layout(depth_greater) out float gl_FragDepth;
layout(location=0) in float depth;
layout(location=0) out vec4 color;
void main() { color=vec4(depth); gl_FragDepth=depth*0.5; float kept=gl_FragDepth; color.y=kept; }
