#!/usr/bin/env python3
"""Compile exact production cache-mutation bodies against dependency doubles."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"
SHADER = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm"
TEMPLATE = ROOT / "Scripts/test-pipeline-cache-mutation-generation.cpp"


def function(text: str, begin: str, end: str) -> str:
    start = text.index(begin)
    return text[start:text.index(end, start)].strip()


def main() -> int:
    pipeline = PIPELINE.read_text()
    shader = SHADER.read_text()
    bodies = {
        "// @PRODUCTION_SHADER_MERGE@": function(
            shader,
            "bool MVKShaderLibraryCache::merge(",
            "\nMVKShaderLibraryCache::~MVKShaderLibraryCache()",
        ),
        "// @PRODUCTION_MARK_DIRTY@": function(
            pipeline,
            "void MVKPipelineCache::markDirty()",
            "\nvoid MVKPipelineCache::markContentChanged()",
        ),
        "// @PRODUCTION_MARK_CONTENT_CHANGED@": function(
            pipeline,
            "void MVKPipelineCache::markContentChanged()",
            "\nVkResult MVKPipelineCache::mergePipelineCaches(",
        ),
        "// @PRODUCTION_REPOSITORY_MERGE@": function(
            pipeline,
            "VkResult MVKPipelineCache::mergePipelineCaches(",
            "\nVkResult MVKPipelineCache::mergePipelineCachesImpl(",
        ),
        "// @PRODUCTION_MERGE_IMPL@": function(
            pipeline,
            "VkResult MVKPipelineCache::mergePipelineCachesImpl(",
            "\n\n#pragma mark Cereal archive definitions",
        ),
        "// @PRODUCTION_SHADER_CACHE_BEHAVIOR@": (
            function(
                shader,
                "MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(",
                "\nMVKShaderLibrary* MVKShaderLibraryCache::getShaderLibraryConcurrent(",
            )
            + "\n\n"
            + function(
                shader,
                "MVKShaderLibrary* MVKShaderLibraryCache::findShaderLibrary(",
                "\nMVKShaderLibraryCache::~MVKShaderLibraryCache()",
            )
        ),
        "// @PRODUCTION_PIPELINE_CACHE_BEHAVIOR@": function(
            pipeline,
            "MVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(",
            "\n// Returns a shader library cache",
        ),
        "// @PRODUCTION_BEHAVIOR_MARKS@": function(
            pipeline,
            "void MVKPipelineCache::markDirty()",
            "\nVkResult MVKPipelineCache::mergePipelineCaches(",
        ),
        "// @PRODUCTION_RELEASE_CONTRIBUTIONS@": function(
            pipeline,
            "void MVKPipeline::releaseShaderLibraryContributions(",
            "\nMVKPipeline::MVKPipeline(",
        ),
        "// @PRODUCTION_ADOPT_CONTRIBUTIONS@": function(
            pipeline,
            "VkResult MVKPipeline::adoptShaderLibrariesInto(",
            "\nvoid MVKPipeline::discardShaderLibraryContributions(",
        ),
    }
    source = TEMPLATE.read_text()
    for marker, body in bodies.items():
        source = source.replace(marker, body)

    with tempfile.TemporaryDirectory(prefix="mvk-cache-generation-") as tmp:
        cpp = Path(tmp) / "test.cpp"
        executable = Path(tmp) / "test"
        cpp.write_text(source)
        build = subprocess.run(
            ["clang++", "-std=c++17", "-O1", str(cpp), "-o", str(executable)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True, timeout=10
        )
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
