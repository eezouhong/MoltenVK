#!/usr/bin/env python3
"""Replace one-field Vulkan aggregate initializers with fully zeroed structures."""

from pathlib import Path
import re

PATH = Path("Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp")
source = PATH.read_text(encoding="utf-8")

helper_anchor = """void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        fail(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

"""
helper = """void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        fail(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

template <typename T>
T makeVkStruct(VkStructureType sType) {
    T value{};
    value.sType = sType;
    return value;
}

"""
if source.count(helper_anchor) != 1:
    raise SystemExit("E2E helper insertion anchor was not found exactly once")
source = source.replace(helper_anchor, helper, 1)

pattern = re.compile(
    r"^(?P<indent>[ \t]*)(?P<type>Vk[A-Za-z0-9_]+)[ \t]+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\{[ \t\r\n]*"
    r"(?P<stype>VK_STRUCTURE_TYPE_[A-Z0-9_]+)[ \t\r\n]*\};",
    re.MULTILINE,
)

replacements: list[tuple[str, str, str]] = []


def replace(match: re.Match[str]) -> str:
    type_name = match.group("type")
    name = match.group("name")
    s_type = match.group("stype")
    replacements.append((type_name, name, s_type))
    return (
        f"{match.group('indent')}{type_name} {name} = "
        f"makeVkStruct<{type_name}>({s_type});"
    )

source = pattern.sub(replace, source)
if len(replacements) < 25:
    raise SystemExit(
        f"expected at least 25 Vulkan structure initializers, replaced {len(replacements)}"
    )

remaining = re.findall(
    r"Vk[A-Za-z0-9_]+\s+[A-Za-z_][A-Za-z0-9_]*\{\s*VK_STRUCTURE_TYPE_",
    source,
    re.MULTILINE,
)
if remaining:
    raise SystemExit(f"unconverted one-field Vulkan aggregate initializers: {len(remaining)}")

PATH.write_text(source, encoding="utf-8")
print(f"converted {len(replacements)} Vulkan structures to makeVkStruct()")
