#!/usr/bin/env python3
"""Run the resident-cache CPU regression without Metal or compiler-plugin SDKs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mvk-ir-resident-cache-") as directory:
        executable = Path(directory) / "resident_cache_test"
        subprocess.run(
            compiler + ["-std=c++17", "-O2", "-pthread", "-UNDEBUG",
                        str(ROOT / "MetalIRCompiler/tests/resident_cache_test.cpp"),
                        "-o", str(executable)],
            check=True, timeout=60,
        )
        subprocess.run([str(executable)], check=True, timeout=30)
    print("PASS: resident-cache count/byte limits, unlimited/disabled retention, ownership and concurrency")


if __name__ == "__main__":
    main()
