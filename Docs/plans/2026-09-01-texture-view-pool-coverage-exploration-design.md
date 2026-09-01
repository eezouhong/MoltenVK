# Texture View Pool Coverage Exploration Design

## Goal

Measure the real reusable coverage of the independent RC6 Metal 4 texture-view
pool before expanding any correctness boundary. Keep the feature default off
and independent of the paused Command Backend. The APP will eventually expose
one additional experimental Texture View Pool switch; the existing Metal 4
compiler switch remains unchanged.

## Problem with the current counters

`assignments` counts only new slot population or rebinding. A slot can then be
used by many descriptor writes without another assignment, so 512 assignments
does not mean 512 draw uses. Conversely, `bypassed_requests` currently combines
views that do not need a Metal view at all with genuinely unsupported views.
During the XC3 ON run it also acquired the pool mutex roughly nine million
times, which contaminates the performance measurement it is supposed to
explain.

## Phase A: behavior-neutral coverage telemetry

Replace the undifferentiated bypass counter with relaxed atomic counters that
are active only when telemetry is enabled:

- descriptor binding lookups;
- direct-base bindings where no texture view is required;
- cached pooled-slot hits;
- new assignments and base rebinds;
- multi-plane exclusions;
- 2D-of-3D exclusions;
- block-texel alias exclusions;
- pool allocation/reset failures.

Classify every actual heavyweight texture-view creation separately:

- pool-shape-eligible, shader-only usage;
- pool-shape-eligible, attachment-capable usage;
- multi-plane;
- 2D-of-3D;
- block-texel alias;
- other.

High-frequency observation must not take the pool structural mutex. Pool slot
allocation, reuse, release, and chunk mutation remain protected by the existing
mutex. Telemetry-disabled production runs perform no counter increment.

## Phase B: evidence-gated expansion

Run the deterministic XC3 DLC New Game route with Pool ON and telemetry enabled.
Rank heavyweight creation buckets by count and cumulative creation time.

Expand only a generic bucket that is both high-volume and representable by one
stable `{resourceID, residencyTexture}` tuple. Shader-only, single-plane views
are the first candidate. Render attachments are not candidates because Metal
render-pass descriptors require an actual `MTLTexture`. Multi-plane, TexBufSoA,
2D-of-3D, block aliases, push/direct bindings, and unsupported targets stay on
the heavyweight path until they have a separate representation and regression
test.

Each expansion gets its own checkpoint and must pass iOS/macOS builds, source
contracts, the XC3 cold/warm route, and four-frame Luna/high visual validation.
If a bucket does not reduce heavyweight creation or improve CPU/memory without
visual regression, revert to the coverage baseline instead of adding a
workaround.
