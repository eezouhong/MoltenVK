#!/usr/bin/env python3
"""Compile and run the platform-independent Metal 4 admission policy tests."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Scripts/test-metal4-compiler-admission.cpp"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mvk-metal4-admission-") as temporary:
        executable = Path(temporary) / "test-metal4-admission"
        build = subprocess.run(
            [
                "clang++",
                "-std=c++17",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT),
                str(SOURCE),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if build.returncode:
            print(build.stdout, end="")
            print(build.stderr, end="")
            return build.returncode
        run = subprocess.run(
            [str(executable)], capture_output=True, text=True, timeout=10
        )
    print(run.stdout, end="")
    print(run.stderr, end="")
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
