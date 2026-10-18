#version 450
#extension GL_ARB_shader_stencil_export : require
layout(location=0) in float depth;
layout(location=0) out vec4 color;
float scratch;
void main() { scratch=depth; color=vec4(scratch); gl_FragDepth=depth; gl_FragStencilRefARB=int(depth*255.0); }
