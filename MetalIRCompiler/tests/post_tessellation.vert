#version 450
// Template of the post-tessellation probe shader. glslang has no TessCoord or PrimitiveId inputs in a vertex
// shader; prepare_post_tessellation.py redecorates the two inputs below as those built-ins.
layout(location = 0) in int patchIndex;
layout(location = 1) in vec3 tessCoord;
layout(set = 0, binding = 0, std430) readonly buffer Patches { vec4 rects[]; } patches;
layout(location = 0) out vec4 color;
void main()
{
    vec4 r = patches.rects[patchIndex];
    gl_Position = vec4(mix(r.x, r.z, tessCoord.x), mix(r.y, r.w, tessCoord.y), 0.0, 1.0);
    color = vec4(tessCoord, float(patchIndex));
}
