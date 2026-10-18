#!/usr/bin/env python3
"""Check that the Metal IR route drops depth/stencil outputs that have no attachment.

Needs glslangValidator, spirv-as, spirv-val and spirv-dis on PATH; no Metal or compiler plugin.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "MetalIRCompiler/tests/fragment_outputs"
SPIRV_CROSS = os.environ.get("SPIRV_CROSS_INCLUDE", "/opt/homebrew/include/spirv_cross")


def run(*command):
    return subprocess.run(command, check=True, timeout=60, capture_output=True, text=True).stdout


def entry_interface(text):
    return re.search(r"OpEntryPoint Fragment %main \"main\"(.*)", text).group(1).split()


def check(test, directory, name, source, depth, stencil, expect, env="vulkan1.2"):
    output = directory / f"{name}.out.spv"
    result = run(str(test), str(source), str(output), str(int(depth)), str(int(stencil))).strip()
    assert result == expect, f"{name}: expected {expect}, got {result}"
    run("spirv-val", "--target-env", env, str(output))
    return run("spirv-dis", str(output))


def main():
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mvk-ir-fragment-outputs-") as directory:
        directory = Path(directory)
        test = directory / "fragment_outputs_test"
        subprocess.run(compiler + ["-std=c++17", "-O2", "-UNDEBUG", "-I", SPIRV_CROSS,
                                   str(ROOT / "MetalIRCompiler/tests/fragment_outputs_test.cpp"), "-o", str(test)],
                       check=True, timeout=60)
        for env, legacy in (("vulkan1.1", True), ("vulkan1.2", False)):
            modules = {}
            for shader in ("depth", "stencil"):
                modules[shader] = directory / f"{shader}-{env}.spv"
                run("glslangValidator", "-V", "--target-env", env, str(FIXTURES / f"{shader}.frag"), "-o", str(modules[shader]))

            text = check(test, directory, f"depth-{env}", modules["depth"], True, False, "changed")
            assert "BuiltIn FragDepth" not in text and "DepthGreater" not in text and "DepthReplacing" not in text
            assert "OpVariable %_ptr_Private_float Private" in text
            assert ("%gl_FragDepth" in entry_interface(text)) != legacy, f"{env}: interface list"
            assert "%color" in entry_interface(text) and "Location 0" in text

            check(test, directory, f"depth-kept-{env}", modules["depth"], False, True, "unchanged")

            text = check(test, directory, f"stencil-only-{env}", modules["stencil"], False, True, "changed")
            assert "BuiltIn FragStencilRefEXT" not in text and "StencilRefReplacingEXT" not in text
            assert "BuiltIn FragDepth" in text and "DepthReplacing" in text

            text = check(test, directory, f"both-{env}", modules["stencil"], True, True, "changed")
            assert "BuiltIn" not in text and "Replacing" not in text
            # FragDepth reuses the module's Private float pointer; the int stencil pointer is new.
            assert text.count("OpTypePointer Private %float") == 1 and "OpTypePointer Private %int" in text

        copy = directory / "copy_pointer.spv"
        run("spirv-as", "--target-env", "spv1.3", str(FIXTURES / "copy_pointer.spvasm"), "-o", str(copy))
        text = check(test, directory, "copy-pointer", copy, True, True, "unchanged", "spv1.3")
        assert "BuiltIn FragDepth" in text
    print("PASS: unattached FragDepth/FragStencilRef outputs become private for SPIR-V 1.3 and 1.5; escaping pointers stay")


if __name__ == "__main__":
    main()
