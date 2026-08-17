#!/usr/bin/env python3
from pathlib import Path

pipeline = Path("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
text = pipeline.read_text()
old = "_stageResources.resources.allBits.areAllBitsClear()"
new = "_stageResources.resources.allBits.empty()"
if text.count(old) != 1:
    raise SystemExit(f"MVKPipeline.h: expected one compute predicate, found {text.count(old)}")
pipeline.write_text(text.replace(old, new, 1))

contract = Path("Scripts/test-metal4-command-backend-phase1.py")
text = contract.read_text()
old = r"supportsMetal4DescriptorlessExecution[\s\S]*?resources\.allBits\.areAllBitsClear[\s\S]*?implicitBuffers\.needed\.empty"
new = r"supportsMetal4DescriptorlessExecution[\s\S]*?resources\.allBits\.empty[\s\S]*?implicitBuffers\.needed\.empty"
if text.count(old) != 1:
    raise SystemExit(f"source contract: expected one compute predicate, found {text.count(old)}")
contract.write_text(text.replace(old, new, 1))
