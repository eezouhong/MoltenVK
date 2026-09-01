# RC6 Metal 4 Texture View Pool Design

## Scope

Add an optional `MTLTextureViewPool` optimization directly to the maintained
RC6 backend. It uses the existing Metal 3 argument-buffer representation and
does not create or depend on Metal 4 command queues, command buffers, command
encoders, execution manifests, or residency sets.

`MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL=1` enables the experiment. It is default
off. OFF creates no service, takes no pool lock, and keeps the original RC6
descriptor bytes and texture-view lifetime.

## Representation and lifetime

A device-owned service allocates fixed-size texture-view-pool chunks. An
eligible `MVKImageViewPlane` lazily owns one stable slot and `MTLResourceID`.
All Metal 3 argument buffers use that ID, so descriptor copies retain one
uniform representation. Their CPU-side resource record stores the base texture
for residency/lifetime instead of constructing the heavyweight view.

Render attachments, argument-encoder layouts, push descriptors, direct encoder
bindings, `TexBufSoA`, multiplanar views, 2D views of 3D textures, block-texel
aliases, and unsupported targets keep the existing heavyweight `MTLTexture`
view. Releasing a slot first replaces it with a tiny device-owned sentinel so
the pool does not pin the old base texture.

## Failure and telemetry

Pool creation, exhaustion, descriptor rejection, or Objective-C exceptions
fall back to the original heavyweight view for that descriptor. No failure
changes Vulkan-visible behavior.

`MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL_TELEMETRY=1` enables low-frequency,
power-of-two snapshots for ON test runs. It reports assignments, reuse,
fallback, live/high-water slots, chunk count, assignment time, and remaining
heavyweight creations. Production runs do not enable this telemetry.

## Acceptance

Use APP remote master and backend RC6 exact revisions. Run matched OFF/ON tests
with the same XC3 profile, cache state, docked setting, path, and milestone.
Require:

- four-frame Luna visual PASS and delayed-frame PASS;
- no loading/UI flicker;
- no FPS or frame-time regression outside repeat variance;
- proof that ON produced pooled assignments and fewer heavyweight views;
- no unacceptable physical-footprint growth.

If ON does not produce a measurable CPU or memory benefit, keep it default off
and stop rather than expanding eligibility speculatively.
