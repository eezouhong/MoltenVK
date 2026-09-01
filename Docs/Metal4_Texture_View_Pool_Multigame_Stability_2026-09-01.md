# Metal 4 Texture View Pool Multi-game Stability — 2026-09-01

## Revisions and profile

- APP master: `a036c9e088f02c9e072c5659fa4080ec2f1a1fcf`.
- APP validation branch: `codex/metal4-texture-view-pool-ui-latest-test`
  at `d289507e0087be877114c136115446cc1d082bcd`.
- Backend RC6 Texture View Pool: `050acd2c53a53d0a2b9953e952733a4c6932fb8a`.
- Command Backend: Legacy.
- Pool: ON with coverage telemetry.
- Display: docked override with the captured iPhone 15 Pro / iOS 27 profile.
- Shader and MSL caches were cleared between titles. Title saves and settings
  created by the games were preserved.

The latest master adds shader-output truncation protection, RGB32 buffer
texture emulation, and the merged WO3U crash fixes. The Texture View Pool UI
commit cherry-picked cleanly on top. Runtime and Simplified Chinese localization
contracts passed.

## Results

| Title and milestone | Luna/high visual result | Cached pool hits | Assignments | High-water slots | Heavyweight views | Fallback/reset failure |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Xenoblade Chronicles 3 DLC tutorial 1/2 | PASS; central attack-menu image, characters, scene, and UI complete across four frames | 1,007,661 | 512 | 302 | 166,449 | 0 / 0 |
| The Legend of Zelda: Tears of the Kingdom opening cave gameplay | PASS; Link/Zelda, cave, stone platforms, torch lighting, and HUD complete across four frames | 2,160,317 | 366 | 363 | 8,192 | 0 / 0 |
| Octopath Traveler II character selection | PASS; HD-2D map, pixel characters, lighting, shadows, frame art, and UI complete across four frames | 349,607 | 174 | 138 | 16,384 | 0 / 0 |
| Xenoblade Chronicles X DE opening space battle | PASS; starfield, ships, beams, explosion particles, geometry, and bloom complete across four frames | 159,516 | 512 | 286 | 34 | 0 / 0 |

No run reported a missing backing resource, multi-plane bypass, block-alias
bypass, pool failure, reset failure, or base rebind.

## TOTK cold/warm note

The clean-cache TOTK run remained on the black loading screen for roughly four
minutes while creating about 703 pipelines. The emulator remained active at
about 120% CPU and continuously wrote its MSL cache; this was not a deadlock.
The immediate warm rerun entered the opening cave gameplay and passed the
four-frame visual check. This is cold pipeline compilation behavior, not a
Texture View Pool correctness failure.

## Conclusion

The current conservative pool representation is visually stable across four
materially different rendering workloads on the macOS iOS-host harness. It
produces substantial cached binding reuse with no observed pool fallback or
reset failure. This does not prove physical-iPhone performance or universal
game compatibility. Keep the APP switch default off and retain the existing
attachment, multi-plane, TexBufSoA, 2D-of-3D, block-alias, push/direct, and
unsupported-target exclusions.
