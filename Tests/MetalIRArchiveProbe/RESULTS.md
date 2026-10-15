# Legacy IR PSO archive: monolithic product integration No-Go

Host: Apple M1 Pro, 16 GiB RAM, macOS 26.5.2 build 25F84, Xcode 27 beta 3 SDK.
Product baseline: MeloNX `2555e56026`, pinned RC6 `78eaad5f`. The probe calls
system Metal directly, with frozen IR metallibs; it loads no MoltenVK/MSC plugin.
Metal 4 integration was withdrawn. No product/default/resident-policy changes.

## Primary real-data result: effective reuse, expensive open

For 200 exact captured descriptors with private system disk caches cleared,
ordinary PSO creation took 3.055 / 3.065 s. Archive open plus strict lookup took
0.184 / 0.193 s: about **15.9–16.6 times faster**. Restoring IR libraries and
finding their PSOs in a durable archive is effective in this condition.

The main No-Go evidence is the **same 200 real descriptors**: direct checkpoints
immediately after `newBinaryArchiveWithDescriptor`, before any lookup, grew from
about 57 MiB to 271.83 / 273.36 MiB. The increments are 214.52 / 216.00 MiB,
about 1.07–1.08 MiB per descriptor in this cohort. Lookup adds only about 4 MiB.
The obstacle is opening cost and footprint, not absence of a useful mechanism.

This local per-item average is not a proven law. Extrapolating to a complete
1731-PSO scene or predicting inevitable phone jetsam would be unvalidated.
Footprint includes driver/framework, objects and diagnostics; exact allocation
ownership is unresolved. No phone evidence was collected.

## Compatibility and cleared-cache cost

Corrected 16-input smoke passed fresh-process strict archive hits, a real missing
function followed by ordinary IR PSO compilation, and corrupt-archive rejection
followed by ordinary compilation. Strict lookup also passed the 200/1000 cohorts.
The source is an incomplete captured render prefix: 200 exact descriptors;
1000/2000 include synthetic write-mask extensions. There is no compute, draw,
dispatch, game or pixel acceptance.

All times below are milliseconds; library restoration is recorded separately.
The 1000 cost pairs here are the cleared-cache comparison, not its full range.

| N | Ordinary PSO total | Archive lookup total | Archive open | Population add | First serialize | File bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 200 | 3055 / 3065 | 10.52 / 10.81 | 173 / 182 | 3699 / 3716 | 2578 / 2258 | 7,197,888 |
| 1000 | 10131 / 10278 | 74.31 / 73.20 | 4534 / 5540 | 12929 / 12935 | 6631 / 13097 | 15,237,808 |
| 2000 | not measured | not run | not run | 22991 / 24031 | 21740 / 11125 | 21,783,344 |

## All four valid 1000 archive opens: supplemental synthetic stress

| System disk caches / sample | Open s | Footprint after first lookup GiB | Final footprint GiB | Remaining lookup increment MiB |
|---|---:|---:|---:|---:|
| clear 1 | 4.534 | 3.394 | 3.407 | 13.625 |
| clear 2 | 5.540 | 2.524 | 2.538 | 13.594 |
| retain 1 | 3.652 | 3.766 | 3.779 | 13.531 |
| retain 2 | 6.688 | 1.907 | 1.920 | 13.656 |

The complete range is **3.65–6.69 s open and 1.92–3.78 GiB final footprint**;
variation approaches twofold and its cause is unknown. These retained samples
are valid for open/footprint despite not being a fully warmed ordinary-PSO
comparison. The older binary had no immediate after-open checkpoint at N=1000;
that raw field remains unavailable. Almost all growth is present after the first
lookup and only 13.5–13.7 MiB follows. Together with the direct N=200 checkpoint,
this supports opening as the dominant phase, not gradual accumulation over the
1000 lookups. It does not invent an immediate-open value for the old batch.

Relative to N=200, this synthetic workload shows superlinear cost amplification.
Different shader/state coverage prevents treating item count as the sole cause.
It supplements, rather than replaces, the real 200-descriptor No-Go evidence.
The 2000 archive was populated/serialized but not reopened after this result.

## Fully warmed ordinary system caches

An ordinary seed preceded retained-cache ABAB at N=200. Ordinary creation was
10.83 / 10.70 ms; archive lookup 14.71 / 10.68 ms plus 181.74 / 172.23 ms open.
Final footprint was about 62 MiB ordinary versus 276–277 MiB archived. This
condition has no net startup benefit. Initial merely-retained comparisons were
not fully ordinary-warmed and remain setup-limited performance evidence.

## Concurrent samples: cold compilation not established

| N | 32 control PSOs total ms | 32 PSOs during serialize total ms | Add-only then serialize ms | Validity for cold-compile interference |
|---:|---:|---:|---:|---|
| 200 | 472 / 468 | 465 / 460 | 2219 / 2326 | early mixed requests only; no whole-flush proof |
| 1000 | 131 / 120 | 124 / 117 | 33556 / 30543 | invalid: possible reuse of already compiled results |
| 2000 | 492 / 540 | 497 / 497 | 13797 / 11046 | coldness unverified |

All 32 N=1000 workers are synthetic write-mask variants; both stage hashes
already occur in population. Their short latency cannot prove that a new backend
compile ran, so they are excluded from valid cold-compilation evidence. N=2000
also shares populated stages; longer latency alone does not establish coldness.
Raw latency and independent serialize measurements remain recorded.

All requests overlapped serialization but finished early. N=1000 covered only
about 0.37–0.38% of the interval; N=2000 about 3.6–4.5%. None excludes a late
global compiler lock or establishes render-thread/game-frame safety. Population
plus PSO and add-only sequences differ; 1000 add-only serialization taking
30–34 s, slower than 2000, is an unresolved anomaly and must not be hidden.

## Decision, benefit window and cheap next gate

**No-Go for the monolithic archive product path**, primarily from the real
N=200 opening footprint. Strong cleared-cache reuse remains worth distinguishing
from no benefit when ordinary caches are already warm. Whole-flush/game/pixel
gates are unverified. Old-iOS Phase 3 and bounded/on-demand shards are unimplemented.

The proposed identity includes OS build, so an OS update invalidates this archive
by policy; it cannot be the fallback for that event. Potential benefit is when
compatibility keys remain valid but ordinary system-cache entries are missing,
such as capacity eviction or storage-pressure cleanup. Their real-world frequency
was not measured; manually cleared caches do not quantify everyday benefit.

Before a shard implementation, proposed small tests are 25/50/100/200 real
entries in open-only mode and a release checkpoint after lookup. Clear every
descriptor's `binaryArchives`, release PSO/temporary references, drain the scoped
autorelease pool and use a weak witness to verify archive destruction. Setting
only the local archive variable to nil leaves descriptor references alive.
If footprint stays high after actual destruction, bounded repeated open/release
must distinguish reusable allocator retention from continued growth. A linear
per-item cost alone does not rule out on-demand shards when only a few stay live.
These follow-up tests were not run during this report correction.

All Metal executions used the canonical queue. Only signed disposable sandbox
Metal/GPUArchiver disk caches were cleared; driver-memory coldness is unknown.
Original failures remain: missing explicit build SDK, second serialization to a
different URL relocating the first file, and a command owner-path mistake before
Metal launch. An owned waiting ticket was cancelled before unsafe 2000 loading.
The fixture defaults to 200 and protects large loads with explicit opt-in.

Full raw evidence, source/binary/input/cache hashes, queue logs, old-experiment
index, proposal comparison and reviewer notes remain in the local task-4 handoff
`ir-metal4-pso/phase2`. Raw results were not changed by these review corrections.
No captured shader bytes or archives are in Git.
