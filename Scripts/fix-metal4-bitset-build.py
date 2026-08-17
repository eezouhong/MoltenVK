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

rescue = Path(".github/workflows/zzzzzzzz-rescue-metal4-v7.yml")
text = rescue.read_text()
old_pipeline = """          pipeline = Path('MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h')
          if pipeline.is_file() and 'areAllBitsClear' in log:
              text = pipeline.read_text()
              fixed = text.replace(
                  '_stageResources.resources.allBits.areAllBitsClear()',
                  '_stageResources.resources.allBits.empty()',
              )
              if fixed != text:
                  pipeline.write_text(fixed)
                  changed.append('fixed descriptorless pipeline bitset predicate')
"""
new_pipeline = """          pipeline = Path('MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h')
          if pipeline.is_file() and \"no member named 'empty'\" in log:
              text = pipeline.read_text()
              fixed = text.replace(
                  '_stageResources.resources.allBits.empty()',
                  '_stageResources.resources.allBits.areAllBitsClear()',
              )
              if fixed != text:
                  pipeline.write_text(fixed)
                  changed.append('fixed descriptorless small-bitset predicate')
"""
if text.count(old_pipeline) != 1:
    raise SystemExit("rescue workflow: stale pipeline repair block not found exactly once")
text = text.replace(old_pipeline, new_pipeline, 1)

old_contract = """          test_contract = Path('Scripts/test-metal4-command-backend-phase1.py')
          if test_contract.is_file() and 'areAllBitsClear' in log:
              text = test_contract.read_text()
              fixed = text.replace(
                  'resources\\\\.allBits\\\\.areAllBitsClear',
                  'resources\\\\.allBits\\\\.empty',
              )
              if fixed != text:
                  test_contract.write_text(fixed)
                  changed.append('updated the exact matching source contract')
"""
new_contract = """          test_contract = Path('Scripts/test-metal4-command-backend-phase1.py')
          if test_contract.is_file() and \"no member named 'empty'\" in log:
              text = test_contract.read_text()
              fixed = text.replace(
                  'resources\\\\.allBits\\\\.empty',
                  'resources\\\\.allBits\\\\.areAllBitsClear',
              )
              if fixed != text:
                  test_contract.write_text(fixed)
                  changed.append('updated the exact matching small-bitset contract')
"""
if text.count(old_contract) != 1:
    raise SystemExit("rescue workflow: stale contract repair block not found exactly once")
rescue.write_text(text.replace(old_contract, new_contract, 1))
