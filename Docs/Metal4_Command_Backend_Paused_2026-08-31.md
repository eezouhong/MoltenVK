# Metal 4 Command Backend Pause Record

## Decision

Pause the experimental Metal 4 command backend. Preserve its branches and
evidence, but do not use it as the base for new Metal 4 features. New Texture
View Pool work starts from the maintained RC6 backend and must not depend on
Metal 4 command queues, command buffers, encoders, residency planning, or the
single-scan execution manifest.

Preserved checkpoints:

- `codex/metal4-texture-view-pool` at `bf7ca75c8`: command-backend-coupled
  Texture View Pool prototype, default off.
- `codex/metal4-texture-view-pool-attachment-fix` at `46d2447b1`: paused
  attachment-barrier investigation, including failed hypotheses and tests.
- `archive/metal4-command-backend-paused-20260831`: immutable pause tag.

## What was proven

- XC3 DLC New Game was run from the same profile, game image, save state,
  docked profile, APP master, and MoltenVK binary.
- With Texture View Pool explicitly off, the Metal 4 command backend could
  leave only the central `衔尾蛇之力` tutorial illustration blank for multiple
  frames and after a delayed check.
- The same binary and profile using the Legacy command backend rendered the
  illustration correctly in four consecutive frames. This excludes the game
  asset, NAS, cache state, and Texture View Pool as the cause.
- The outer tutorial panel and text are separate from the central offscreen
  image. Correct outer UI does not prove the producer-to-sampled-texture path.

## Failed attachment-restart hypotheses

The archived `78976ae92` restart treated nearly every in-render `GENERAL`
image barrier as attachment feedback. In the observed run it produced 57,768
in-render restarts, caused visible loading-icon flicker, and did not restore the
central image. Its write-to-read telemetry remained zero because Ryujinx emits
generic `VK_ACCESS_SHADER_READ_BIT`, not only the narrower sampled-read bit.

A second bounded attempt accepted generic shader-read access, limited restarts
to active attachments, and rejected memoryless restart. It removed the abnormal
loading behavior in four-frame visual validation, but still did not restore the
central illustration. The run accumulated roughly 70,000 active attachment
restarts, showing that per-barrier render-pass splitting is not a viable general
architecture for this workload.

Do not reapply `78976ae92`, `dbf26a864`, `ef267af3b`, or `df6d45ac0` piecemeal
without a new command-backend design review. In particular, do not accept
memoryless `DontCare` store/load as preservation, and do not trade correctness
for reduced fallback coverage.

## Validation lessons

- A single screenshot is insufficient. Capture endpoint plus at least three
  later frames and use visual model review; use a delayed frame when content may
  be loading.
- OCR is for navigation and text milestones. It cannot validate selection glow,
  missing embedded artwork, black blocks, or flicker.
- Record the exact APP revision, backend revision, DYLIB SHA-256, profile
  environment, command-backend selection, and feature switch in every session.
- A valid performance A/B requires a visually correct OFF baseline. Do not
  continue performance work when the baseline is nondeterministically wrong.

## Clean Texture View Pool boundary

`MTLTextureViewPool` resource IDs are supported by Metal argument buffers as
well as Metal 4 argument tables. The new implementation can therefore target
RC6's existing Metal 3 argument-buffer representation directly. It needs only:

- an independent, default-off runtime gate;
- a device-owned bounded/segmented view pool;
- stable slot lifetime and base-resource residency;
- heavyweight fallback for direct bindings, render attachments, push
  descriptors, special alias views, and unsupported targets;
- matched OFF/ON CPU, memory, and visual telemetry.

It must not require or silently enable the paused Metal 4 command backend.
