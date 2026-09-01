#!/usr/bin/env python3
"""Static and model acceptance gate for the Metal 4 texture-view pool.

The production implementation is Objective-C++ and needs the Xcode 26 Metal
SDK. This test locks the ownership, fallback, and descriptor-binding boundary
in every environment; device A/B remains the runtime source of truth.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKDevice.h"
DEVICE_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm"
IMAGE_H = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKImage.h"
IMAGE_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKImage.mm"
DESCRIPTOR_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKDescriptorSet.mm"


def require(text: str, needle: str, source: Path) -> None:
    if needle not in text:
        raise AssertionError(f"{source}: missing required invariant: {needle}")


def require_pattern(text: str, pattern: str, source: Path) -> None:
    if not re.search(pattern, text, re.DOTALL):
        raise AssertionError(f"{source}: missing required pattern: {pattern}")


@dataclass(frozen=True)
class Handle:
    chunk: int
    slot: int


class SegmentedPoolModel:
    def __init__(self, chunk_size: int, maximum_slots: int) -> None:
        assert chunk_size > 0
        assert maximum_slots >= chunk_size
        self.chunk_size = chunk_size
        self.maximum_slots = maximum_slots
        self.created_slots = 0
        self.free: list[Handle] = []
        self.live: set[Handle] = set()

    def acquire(self) -> Handle | None:
        if self.free:
            handle = self.free.pop()
        elif self.created_slots < self.maximum_slots:
            linear = self.created_slots
            self.created_slots += 1
            handle = Handle(linear // self.chunk_size, linear % self.chunk_size)
        else:
            return None
        assert handle not in self.live
        self.live.add(handle)
        return handle

    def release(self, handle: Handle) -> None:
        assert handle in self.live
        self.live.remove(handle)
        self.free.append(handle)


def test_segmented_slot_lifetime_model() -> None:
    pool = SegmentedPoolModel(chunk_size=2, maximum_slots=4)
    first = pool.acquire()
    second = pool.acquire()
    third = pool.acquire()
    fourth = pool.acquire()
    assert [first, second, third, fourth] == [
        Handle(0, 0),
        Handle(0, 1),
        Handle(1, 0),
        Handle(1, 1),
    ]
    assert pool.acquire() is None

    assert second is not None
    pool.release(second)
    assert pool.acquire() == second


def test_source_contract() -> None:
    device_h = DEVICE_H.read_text()
    device_mm = DEVICE_MM.read_text()
    image_h = IMAGE_H.read_text()
    image_mm = IMAGE_MM.read_text()
    descriptor_mm = DESCRIPTOR_MM.read_text()

    require(device_h, "struct MVKMetal4TextureViewHandle", DEVICE_H)
    require(device_h, "class MVKMetal4TextureViewPool", DEVICE_H)
    require(device_h, "MVKMetal4TextureViewHandle acquireTextureView", DEVICE_H)
    require(device_h, "void releaseTextureView", DEVICE_H)
    require(device_h, "MVKMetal4TextureViewPool* getMetal4TextureViewPool()", DEVICE_H)

    require(device_mm, "MTLResourceViewPoolDescriptor", DEVICE_MM)
    require(device_mm, "newTextureViewPoolWithDescriptor", DEVICE_MM)
    require(device_mm, "setTextureView:", DEVICE_MM)
    require(device_mm, "descriptor:", DEVICE_MM)
    require(device_mm, "atIndex:", DEVICE_MM)
    require(device_mm, "MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL", DEVICE_MM)
    require(device_mm, "MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL_TELEMETRY", DEVICE_MM)
    require(device_mm, "Metal 4 texture view pool telemetry:", DEVICE_MM)
    require_pattern(
        device_mm,
        r'mvkGetEnvVarNumber\(\s*"MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL",\s*0\.0\)',
        DEVICE_MM,
    )
    require_pattern(
        device_mm,
        r"if\s*\(!device\s*\|\|\s*\(!enabled\s*&&\s*!telemetryEnabled\)\)",
        DEVICE_MM,
    )

    require(image_h, "struct MVKMetal4TextureViewBinding", IMAGE_H)
    require(image_h, "getMetal4TextureViewBinding", IMAGE_H)
    require(image_mm, "MTLTextureViewDescriptor", IMAGE_MM)
    require(image_mm, "releaseMetal4TextureView", IMAGE_MM)
    require(image_mm, "isMetal4TextureViewPoolEligible", IMAGE_MM)
    require(image_mm, "is2dViewOf3d", IMAGE_MM)
    require(image_mm, "isBlockTexelView", IMAGE_MM)

    require(descriptor_mm, "setTextureResourceID", DESCRIPTOR_MM)
    require(descriptor_mm, "getMetal4TextureViewBinding", DESCRIPTOR_MM)
    require(descriptor_mm, "pooledBindings", DESCRIPTOR_MM)
    for duplicate_lookup in (
        "getMetal4TextureViewResourceID",
        "getMetal4TextureViewBaseTexture",
    ):
        if duplicate_lookup in descriptor_mm:
            raise AssertionError(
                f"{DESCRIPTOR_MM}: pooled descriptor binding is resolved more than once: {duplicate_lookup}"
            )
    require_pattern(
        descriptor_mm,
        r"useMetal4TextureViewPool\s*=\s*[\s\S]*?MVKArgumentBufferMode::Metal3\s*&&[\s\S]*?gpuLayout\s*!=\s*MVKDescriptorGPULayout::TexBufSoA[\s\S]*?textureViewPool\s*&&\s*textureViewPool->isEnabled\(\)",
        DESCRIPTOR_MM,
    )
    if "useMetal4TextureViewPool = set->supportsMetal4ArgumentTable()" in descriptor_mm:
        raise AssertionError(
            f"{DESCRIPTOR_MM}: per-set pooled representations make descriptor copies unsafe"
        )
    require_pattern(
        descriptor_mm,
        r"mvkPushDescriptorSet[\s\S]*?writeDescriptorSetCPUBufferDispatch\([^;]*nullptr\)",
        DESCRIPTOR_MM,
    )
    require_pattern(
        image_mm,
        r"pool\s*&&\s*pool->isEnabled\(\)\s*&&\s*isMetal4TextureViewPoolEligible",
        IMAGE_MM,
    )

    combined = device_h + device_mm + image_h + image_mm + descriptor_mm
    require(combined, "MVK_XCODE_26", ROOT)
    for forbidden in (
        "isMetal4CommandBackendEnabled",
        "MTL4CommandQueue",
        "MTL4CommandBuffer",
        "MTL4RenderCommandEncoder",
    ):
        if forbidden in combined:
            raise AssertionError(f"{ROOT}: texture-view pool depends on paused command backend: {forbidden}")


def main() -> int:
    test_segmented_slot_lifetime_model()
    test_source_contract()
    print("metal4 texture-view pool contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"metal4 texture-view pool contract: FAIL: {error}")
        raise SystemExit(1)
