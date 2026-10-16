# Offline shader-cache corpus

These CPU-only tools inspect a stopped read-only Ryujinx host-cache snapshot.
They never load Vulkan, Metal, a converter plugin, a game, or a phone application.

`extract_cache.py` implements the TOCH v1.2 offset/length record format and the
stage packet in MeloNX `DiskCacheHostStorage`, `BinarySerializer`, and
`ShaderBinarySerializer` (format checked at `46996b2e`). It does not search for
SPIR-V magic. Compression values 0/1/2 are None/raw Deflate/Brotli; Brotli requires
the optional Python `brotli` package. Shared/guest metadata is not decoded, and
this is not a validator for the entire cache or its specialization state.

```sh
python3 extract_cache.py --toc /private/snapshot/vulkan_apple.toc \
  --data /private/snapshot/vulkan_apple.data \
  --output /private/new-corpus --validator /path/to/spirv-val \
  --source-label example-host --target-env vulkan1.2
python3 stats.py --manifest /private/new-corpus/manifest.json \
  --grammar /path/to/spirv.core.grammar.json \
  --glsl-grammar /path/to/extinst.glsl.std.450.grammar.json \
  --output /private/new-statistics
python3 -m unittest discover -s . -p 'test_*.py'
```

Use new output directories. Extraction returns 0 only for a complete, unchanged
input with all modules validated; 2 preserves an incomplete manifest. Invalid
TOC format fails before extraction. The 16 MiB per-program bound mirrors the
frontend. Output has a configurable total budget, stage/SHA identities and source
record references. Duplicate modules are exported once; counts in `stats.py` are
static instruction sites in unique modules, not runtime execution frequency.

Keep modules, manifests, statistics and logs outside the repository. The manifest
records TOCH's codegen field as stored (normally zero); the shared TOCS contains
the frontend host-codegen version. Do not identify the generating frontend or
claim a phone capture from this field alone. Record snapshot provenance separately.

The tests use synthetic packets, including corruption, duplicate stages, trailing
compression data and an unreferenced magic-value decoy. Actual corpus validation
is additional evidence, not a substitute for those format-boundary tests.
