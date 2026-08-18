#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"{path}: expected one occurrence, found {count}: {old[:160]!r}"
        )
    path.write_text(text.replace(old, new, 1))


header = Path("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h")
replace_once(
    header,
    "\tvoid libraryBecameCold(MVKShaderLibrary* library);",
    "\tvoid libraryBecameCold(\n"
    "\t\tMVKShaderLibrary* library,\n"
    "\t\tuint64_t evictedUncompressedMSLBytes);",
)

source = Path("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm")
replace_once(
    source,
    "\tid<MTLLibrary> releasedLibrary = _mtlLibrary;\n"
    "\t_mtlLibrary = nil;\n"
    "\tmap<vector<pair<uint32_t, MVKShaderMacroValue>>, MVKShaderLibrary*> releasedVariants;\n"
    "\treleasedVariants.swap(_specializationVariants);",
    "\tuint64_t evictedUncompressedMSLBytes =\n"
    "\t\t_compressedMSL._uncompressedSize;\n"
    "\tfor (const auto& item : _specializationVariants) {\n"
    "\t\tif (item.second) {\n"
    "\t\t\tevictedUncompressedMSLBytes +=\n"
    "\t\t\t\titem.second->_compressedMSL._uncompressedSize;\n"
    "\t\t}\n"
    "\t}\n\n"
    "\tid<MTLLibrary> releasedLibrary = _mtlLibrary;\n"
    "\t_mtlLibrary = nil;\n"
    "\tmap<vector<pair<uint32_t, MVKShaderMacroValue>>, MVKShaderLibrary*> releasedVariants;\n"
    "\treleasedVariants.swap(_specializationVariants);",
)
replace_once(
    source,
    "\tif (_repository) { _repository->libraryBecameCold(this); }",
    "\tif (_repository) {\n"
    "\t\t_repository->libraryBecameCold(\n"
    "\t\t\tthis,\n"
    "\t\t\tevictedUncompressedMSLBytes);\n"
    "\t}",
)
replace_once(
    source,
    "void MVKShaderLibraryRepository::libraryBecameCold(MVKShaderLibrary* library) {",
    "void MVKShaderLibraryRepository::libraryBecameCold(\n"
    "\tMVKShaderLibrary* library,\n"
    "\tuint64_t evictedUncompressedMSLBytes) {",
)
replace_once(
    source,
    "\t\t_evictedUncompressedMSLBytes.fetch_add(\n"
    "\t\t\tlibrary->_compressedMSL._uncompressedSize,\n"
    "\t\t\tmemory_order_relaxed);",
    "\t\t_evictedUncompressedMSLBytes.fetch_add(\n"
    "\t\t\tevictedUncompressedMSLBytes,\n"
    "\t\t\tmemory_order_relaxed);",
)

policy = Path("Scripts/test-metal4-shared-shader-repository.py")
text = policy.read_text()
anchor = '    assert "releasedVariants.swap(_specializationVariants)" in eviction_body\n'
addition = anchor + (
    '    assert "evictedUncompressedMSLBytes" in eviction_body\n'
    '    require(\n'
    '        shader_mm,\n'
    '        "item.second->_compressedMSL._uncompressedSize",\n'
    '        SHADER_MM,\n'
    '    )\n'
)
if text.count(anchor) != 1:
    raise SystemExit(f"policy anchor count={text.count(anchor)}")
policy.write_text(text.replace(anchor, addition, 1))
