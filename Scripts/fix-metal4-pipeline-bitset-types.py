#!/usr/bin/env python3
from pathlib import Path

pipeline = Path("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
text = pipeline.read_text()
replacements = (
    (
        "resources.resources.allBits.areAllBitsClear()",
        "resources.resources.allBits.empty()",
        1,
    ),
    (
        "_vkVertexBuffers.empty() && _mtlVertexBuffers.empty()",
        "_vkVertexBuffers.areAllBitsClear() && _mtlVertexBuffers.areAllBitsClear()",
        1,
    ),
)
for old, new, expected in replacements:
    actual = text.count(old)
    if actual != expected:
        raise SystemExit(f"MVKPipeline.mm: expected {expected} occurrences of {old!r}, found {actual}")
    text = text.replace(old, new, expected)
pipeline.write_text(text)

contract = Path("Scripts/test-metal4-command-backend-phase1.py")
text = contract.read_text()
old = r"resources\.allBits\.areAllBitsClear[\s\S]*?implicitBuffers\.needed\.empty"
new = r"resources\.allBits\.empty[\s\S]*?implicitBuffers\.needed\.empty"
actual = text.count(old)
if actual != 1:
    raise SystemExit(f"source contract: expected one render resource-bitset predicate, found {actual}")
text = text.replace(old, new, 1)
anchor = '''        "graphics pipeline eligibility does not reject descriptor or implicit-buffer use",
    )
'''
addition = '''        "graphics pipeline eligibility does not reject descriptor or implicit-buffer use",
    )
    require(
        pipeline_h + read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"),
        r"_vkVertexBuffers\\.areAllBitsClear\\(\\)[\\s\\S]*?_mtlVertexBuffers\\.areAllBitsClear\\(\\)",
        "strict render eligibility does not use the small-bitset clear predicate for vertex bindings",
    )
'''
if text.count(anchor) != 1:
    raise SystemExit("source contract: graphics eligibility anchor not found exactly once")
contract.write_text(text.replace(anchor, addition, 1))
