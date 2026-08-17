#!/usr/bin/env python3
from pathlib import Path


def replace_exact(path: str, old: str, new: str, count: int) -> None:
    target = Path(path)
    text = target.read_text()
    actual = text.count(old)
    if actual != count:
        raise SystemExit(f"{path}: expected {count} occurrences of {old!r}, found {actual}")
    target.write_text(text.replace(old, new))


replace_exact(
    "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h",
    "_stageResources.resources.allBits.empty()",
    "_stageResources.resources.allBits.areAllBitsClear()",
    1,
)
replace_exact(
    "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm",
    "resources.resources.allBits.empty()",
    "resources.resources.allBits.areAllBitsClear()",
    1,
)
replace_exact(
    "Scripts/test-metal4-command-backend-phase1.py",
    r"resources\.allBits\.empty",
    r"resources\.allBits\.areAllBitsClear",
    2,
)
