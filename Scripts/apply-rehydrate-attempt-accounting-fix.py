#!/usr/bin/env python3
from pathlib import Path

shader = Path("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm")
text = shader.read_text()
old = "\t\tif (!wasResident) { rehydrateStartedAt = mvkGetTimestamp(); }"
new = (
    "\t\tif (!wasResident && allowLibraryCompile) { "
    "rehydrateStartedAt = mvkGetTimestamp(); }"
)
if text.count(old) != 1:
    raise SystemExit(f"expected one rehydrate timer start, found {text.count(old)}")
text = text.replace(old, new, 1)
shader.write_text(text)

policy = Path("Scripts/test-metal4-shared-shader-repository.py")
text = policy.read_text()
anchor = '    assert "if (!allowLibraryCompile) { return false; }" in ensure_body\n'
addition = anchor + (
    '    require(\n'
    '        shader_mm,\n'
    '        "if (!wasResident && allowLibraryCompile)",\n'
    '        SHADER_MM,\n'
    '    )\n'
)
if text.count(anchor) != 1:
    raise SystemExit(f"policy anchor count={text.count(anchor)}")
policy.write_text(text.replace(anchor, addition, 1))
