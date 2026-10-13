# Legacy IR PSO archive: product integration No-Go

Host: Apple M1 Pro, 16 GiB RAM, macOS 26.5.2 build 25F84, Xcode 27 beta 3 SDK.
Product baseline: MeloNX `2555e56026`, pinned RC6 `78eaad5f`. The probe calls
system Metal directly, with frozen IR metallibs; it loads no MoltenVK/MSC plugin.
Metal 4 integration was withdrawn. No product/default/resident-policy changes.

## Restored-library compatibility

Corrected 16-input smoke passed fresh-process strict archive hits, a real missing
function followed by ordinary IR PSO compilation, and corrupt-archive rejection
followed by ordinary compilation. Strict lookup also passed the 200/1000 cohorts.
The source is an incomplete captured render prefix: 200 exact descriptors;
1000/2000 include synthetic write-mask extensions. There is no compute, draw,
dispatch, game or pixel acceptance.

## Cleared private system disk caches

All times below are milliseconds; library restoration is recorded separately.

| N | Ordinary PSO total | Archive lookup total | Archive open | Population add | First serialize | File bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 200 | 3055 / 3065 | 10.52 / 10.81 | 173 / 182 | 3699 / 3716 | 2578 / 2258 | 7,197,888 |
| 1000 | 10131 / 10278 | 74.31 / 73.20 | 4534 / 5540 | 12929 / 12935 | 6631 / 13097 | 15,237,808 |
| 2000 | not measured | not run | not run | 22991 / 24031 | 21740 / 11125 | 21,783,344 |

The 2000 archive was populated and serialized but not reopened: the 1000 load
already reached about 2.5–3.4 GiB process footprint on this 16 GiB host. The
200 archive reached about 277 MiB versus about 63 MiB ordinary; warm follow-up
checkpoints showed about 272–273 MiB immediately after archive open, versus
about 57 MiB after library restoration. Footprint is not API-owned allocation
accounting; the exact owner and expansion cause are unresolved.

## Fully warmed ordinary system caches

An ordinary seed preceded retained-cache ABAB at N=200. Ordinary creation was
10.83 / 10.70 ms; archive lookup 14.71 / 10.68 ms plus 181.74 / 172.23 ms open.
Final footprint was about 62 MiB ordinary versus 276–277 MiB archived. This
condition has no net startup benefit. Initial merely-retained comparisons were
not fully ordinary-warmed and are retained as setup-limited evidence.

## Concurrent compiler samples and limits

| N | 32 control PSOs total ms | 32 PSOs during serialize total ms | Add-only then serialize ms |
|---:|---:|---:|---:|
| 200 | 472 / 468 | 465 / 460 | 2219 / 2326 |
| 1000 | 131 / 120 | 124 / 117 | 33556 / 30543 |
| 2000 | 492 / 540 | 497 / 497 | 13797 / 11046 |

All 32 requests overlapped serialization, but finished early. At N=1000 they
covered only about 0.37–0.38% of the serialize interval; N=2000 about 3.6–4.5%.
No extra wait was observed in those samples. This does not exclude a late global
compiler lock or establish render-thread/game-frame safety. Population+PSO and
add-only sequences differ; their serialization times must not be pooled.

## Decision and delivery

**No-Go for the monolithic archive product path:** large open-time/footprint
cost, no benefit when ordinary caches are warm, and no whole-flush/game/pixel
gate. True safe points and bounded/on-demand shards remain unimplemented future
candidates. Old-iOS Phase 3 was not implemented. No phone/iPad was operated.

All Metal executions used the canonical queue. Only signed disposable sandbox
Metal/GPUArchiver disk caches were cleared; driver-memory coldness is unknown.
Original failures remain: missing explicit build SDK, second serialization to a
different URL relocating the first file, and a command owner-path mistake before
Metal launch. An owned waiting ticket was cancelled before unsafe 2000 loading.
The fixture now defaults to 200 and protects large loads with explicit opt-in.

Full raw evidence, source/binary/input/cache hashes, queue logs, old-experiment
index, proposal comparison and reviewer notes are in the local task-4 handoff
`ir-metal4-pso/phase2`. No captured shader bytes or archives are in Git.
