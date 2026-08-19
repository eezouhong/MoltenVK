#!/usr/bin/env python3
from pathlib import Path
import re

path = Path("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
text = path.read_text(encoding="utf-8")

pattern = re.compile(
    r'(?P<i>[ \t]+)bool useAsyncTasks = mvkGetEnvVarNumber\("MVK_CONFIG_METAL4_FLEXIBLE_ASYNC", 0\.0\) != 0\.0;\n'
    r'(?P=i)double configuredAsyncMax = mvkGetEnvVarNumber\("MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX", 3\.0\);\n'
    r'(?P=i)size_t configuredAsyncTaskMax = static_cast<size_t>\(mvkClamp\(configuredAsyncMax, 1\.0, 3\.0\)\);\n'
    r'(?P=i)size_t deviceAsyncTaskMax = max<size_t>\(\n'
    r'(?P=i)[ \t]+1,\n'
    r'(?P=i)[ \t]+static_cast<size_t>\(mtlDevice\.maximumConcurrentCompilationTaskCount\)\);\n'
    r'[ \t]+size_t effectiveAsyncTaskMax = useAsyncTasks\n'
    r'[ \t]+\? min\(configuredAsyncTaskMax, deviceAsyncTaskMax\)\n'
    r'[ \t]+: 1;'
)

match = pattern.search(text)
if match is None:
    raise SystemExit("Metal 4 compiler concurrency block not found exactly once")
if pattern.search(text, match.end()) is not None:
    raise SystemExit("Metal 4 compiler concurrency block matched more than once")

indent = match.group("i")
replacement = (
    f'{indent}bool useAsyncTasks = mvkGetEnvVarNumber("MVK_CONFIG_METAL4_FLEXIBLE_ASYNC", 0.0) != 0.0;\n'
    f'{indent}// shouldMaximizeConcurrentCompilation controls the device-selected CPU\n'
    f'{indent}// compilation width. Do not impose a second MoltenVK-local cap here.\n'
    f'{indent}size_t deviceAsyncTaskMax = max<size_t>(\n'
    f'{indent}\t1,\n'
    f'{indent}\tstatic_cast<size_t>(mtlDevice.maximumConcurrentCompilationTaskCount));\n'
    f'{indent}size_t configuredAsyncTaskMax = deviceAsyncTaskMax;\n'
    f'{indent}size_t effectiveAsyncTaskMax = useAsyncTasks\n'
    f'{indent}\t? deviceAsyncTaskMax\n'
    f'{indent}\t: 1;'
)

path.write_text(text[:match.start()] + replacement + text[match.end():], encoding="utf-8")

updated = path.read_text(encoding="utf-8")
if "MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX" in updated:
    raise SystemExit("legacy Metal 4 async max override still present")
if "? min(configuredAsyncTaskMax, deviceAsyncTaskMax)" in updated:
    raise SystemExit("legacy Metal 4 local concurrency cap still present")
if "configuredAsyncTaskMax = deviceAsyncTaskMax" not in updated:
    raise SystemExit("device-selected Metal 4 concurrency policy missing")
