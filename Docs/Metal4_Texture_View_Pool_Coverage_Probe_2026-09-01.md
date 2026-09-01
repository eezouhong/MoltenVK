# Metal 4 Texture View Pool Coverage Probe — 2026-09-01

## Revisions and route

- APP master: `6cceb0b8ee5193683b19c9f3bac68a7003f05e7f`.
- Backend RC6 base: `fa828f460647e1c4d83b0ac42499ed30a68d6970`.
- Coverage probe: `97a1efbe22214bcba908bc17803fbe5a66bcca99`.
- Legacy Command Backend, Texture View Pool ON, docked XC3 DLC New Game route.
- Cold shader/MSL cache and the captured iPhone 15 Pro / iOS 27 profile.

Four tutorial endpoint frames passed Luna/high. The central attack-menu image,
geometry, and UI remained complete and stable.

## Final coverage snapshot

- descriptor binding lookups: 9,737,436;
- cached pooled binding hits: 968,819;
- assignments: 512;
- base rebinds: 0;
- direct-base bindings that need no view: 8,768,105;
- multi-plane / block-alias / missing-backing / pool-failure bypasses: 0;
- pool reuses / releases: 213 / 214;
- created / live / high-water slots: 299 / 298 / 299;
- pool fallbacks / reset failures: 0 / 0;
- total / maximum assignment time: 2.081 ms / 0.639 ms.

One assignment can serve many later descriptor writes. The 512 assignments
therefore serviced almost 969,000 cached binding hits. The pool represented
about 10% of descriptor lookups; the earlier assignment-to-heavyweight ratio
was not a valid lookup-coverage metric.

## Heavyweight creation classification

At the same endpoint:

- total heavyweight texture-view creations: 157,283;
- pool-shape-eligible shader-only: 0;
- pool-shape-eligible attachment-capable: 489;
- 2D views of 3D textures: 156,794;
- multi-plane / block-alias / other: 0.

The remaining heavyweight cost is therefore almost entirely the generic
2D-of-3D alias path. It cannot be enabled by merely deleting the eligibility
check: that path first creates a heap-backed 2D-array alias of the 3D texture,
then creates the final texture view from that alias. A correct pooled form must
retain the alias as the residency texture and preserve the same mip/slice and
base-rebinding semantics.

Before attempting that representation, split 2D-of-3D creations into
shader-only and attachment-capable usage. Only a high-volume shader-only bucket
is a candidate for the next checkpoint. Metal render attachments continue to
require a real `MTLTexture` and remain outside the pool.
