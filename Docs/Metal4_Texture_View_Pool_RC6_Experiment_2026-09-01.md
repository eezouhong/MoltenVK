# RC6 Metal 4 Texture View Pool Experiment — 2026-09-01

## Scope and revisions

- APP: remote `master` at `6cceb0b8ee5193683b19c9f3bac68a7003f05e7f`.
- Backend base: maintained RC6 branch at
  `fa828f460647e1c4d83b0ac42499ed30a68d6970`.
- Candidate: `d886b25079f800e6cc9a9ecf987d74a51a5a599f`.
- Backend `main` is not an ancestor of the candidate.
- Command submission remained Legacy in every run. The texture-view pool is an
  independent, default-off Metal 3 argument-buffer experiment.
- Host profile: iPhone 15 Pro / iOS 27 profile, docked override, Metal 4
  compiler on, streaming shader cache 2 GiB, MSL cache on and unlimited.

## Implementation boundary

The candidate adds a device-owned segmented `MTLTextureViewPool` and assigns a
stable resource ID to eligible single-plane shader views. Descriptor CPU and
GPU storage consume one atomically resolved `{resourceID, residencyTexture}`
tuple. OFF does not enter the pooled descriptor path. Unsupported and
correctness-sensitive view classes keep the existing heavyweight path.

The atomic tuple change removed three separate image-view lookups and locks per
descriptor. The final review found no remaining P0/P1 correctness issue. iOS
and macOS Release builds and all four Metal 4 source contracts passed.

## XC3 deterministic A/B

Test route: Xenoblade Chronicles 3 DLC New Game, Normal difficulty, no further
input, ending at the `New Feature / Ouroboros Power` tutorial page 1/2. Four
window-only frames were captured at each endpoint and checked with Luna/high.

| Run | Cache | Visual | Mean FPS | Median FPS | Mean CPU | Mean footprint | Peak footprint |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Pool OFF | cold | PASS | 22.78 | 28.32 | 5.58% | 3.51 GiB | 5.08 GiB |
| Pool ON | cold | PASS | 27.83 | 30.05 | 6.22% | 4.24 GiB | 5.48 GiB |
| Pool OFF | warm | PASS | 27.27 | 30.07 | 5.96% | 3.74 GiB | 5.49 GiB |
| Pool ON | warm | PASS | 27.40 | 29.96 | 6.01% | 3.60 GiB | 5.05 GiB |

Cold-cache FPS is not attributable: the OFF run contained a one-time 60–120 s
stall that did not repeat in OFF warm, while ON cold took longer to reach the
title screen. Treat it as cache/NAS/run variance, not a pool win.

The warm comparison is the useful result. Mean FPS changed by about +0.5%,
median FPS by about -0.4%, and CPU by +0.05 percentage points: all effectively
neutral at this sample size. Mean footprint was 0.13 GiB lower and peak
footprint 0.44 GiB lower with the pool, but one A/B pair is not enough to claim
a stable memory reduction.

All four OFF and all four ON endpoint frames retained the central attack-menu
illustration. No missing texture, black block, flicker proxy, geometry damage,
or UI damage was visible.

## Mechanism telemetry

Final ON warm snapshot:

- assignments: 512;
- reuses/releases: 196/196;
- fallbacks/reset failures: 0/0;
- chunks: 1;
- created/high-water/live slots: 316/316/316;
- total/max assignment time: 2.616 ms / 1.113 ms;
- heavyweight creations observed at that snapshot: 162,412.

OFF cold and warm both crossed 131,072 heavyweight creations before the same
tutorial endpoint. Only hundreds of pooled assignments occur against roughly
160,000 heavyweight creations. The current conservative eligibility boundary
therefore covers about 0.3% of this workload. It proves that the pool is active
and stable, but it cannot materially change overall performance in XC3.

## Decision

Keep the experiment default off and checkpoint the code. Do not broaden
eligibility speculatively: render attachments, multi-plane views, TexBufSoA,
2D-of-3D views, block aliases, push/direct bindings, and unsupported targets
remain correctness boundaries. Any continuation must first identify a generic,
high-volume eligible view class from telemetry and add a focused correctness
test before changing that boundary.

The old Metal 4 Command Backend remains paused and is not part of this result.
