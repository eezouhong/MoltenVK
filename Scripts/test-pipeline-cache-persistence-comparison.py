#!/usr/bin/env python3
"""Compile the production persistence comparator with real metadata types."""

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CONVERTER_SOURCE = ROOT / "MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp"
CONVERTER_DIR = CONVERTER_SOURCE.parent
PIPELINE = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"
TEMPLATE = ROOT / "Scripts/test-pipeline-cache-persistence-comparison.cpp"


def between(text: str, begin: str, end: str) -> str:
    start = text.index(begin)
    return text[start:text.index(end, start)].strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spirv-cross", type=Path, required=True)
    parser.add_argument("--cereal", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("normal", "bad-alloc", "cereal-exception", "no-cereal"),
        default="normal",
    )
    args = parser.parse_args()

    converter = CONVERTER_SOURCE.read_text()
    config_methods = between(
        converter,
        "using namespace mvk;",
        "\n#pragma mark -\n#pragma mark SPIRVToMSLConverter\n",
    )
    pipeline = PIPELINE.read_text()
    serializers = between(
        pipeline,
        "namespace SPIRV_CROSS_NAMESPACE {",
        "\ntemplate<class Archive>\nvoid serialize(Archive & archive, MVKShaderModuleKey& k)",
    )
    comparator = between(
        pipeline,
        "bool mvkAreShaderLibraryPersistenceEqual(",
        "\n\n#pragma mark Construction",
    )
    source = (TEMPLATE.read_text()
              .replace("// @PRODUCTION_CONFIG_METHODS@", config_methods)
              .replace("// @PRODUCTION_METADATA_SERIALIZERS@", serializers)
              .replace("// @PRODUCTION_PERSISTENCE_COMPARATOR@", comparator))

    definitions = {
        "normal": [],
        "bad-alloc": ["-DINJECT_BAD_ALLOC=1"],
        "cereal-exception": ["-DINJECT_CEREAL_EXCEPTION=1"],
        "no-cereal": ["-DMVK_USE_CEREAL=0"],
    }[args.mode]
    with tempfile.TemporaryDirectory(prefix="mvk-persistence-compare-") as tmp:
        cpp = Path(tmp) / "test.cpp"
        executable = Path(tmp) / "test"
        cpp.write_text(source)
        command = [
            "clang++", "-std=c++17", "-O2", "-g",
            "-DSPIRV_CROSS_NAMESPACE_OVERRIDE=MVK_spirv_cross",
            *definitions,
            "-I", str(CONVERTER_DIR),
            "-I", str(args.spirv_cross),
            "-I", str(args.cereal / "include"),
            str(cpp), "-o", str(executable),
        ]
        build = subprocess.run(
            command, capture_output=True, text=True, timeout=45)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True, timeout=20)
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
