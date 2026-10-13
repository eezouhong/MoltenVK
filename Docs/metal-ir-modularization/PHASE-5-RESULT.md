# Phase 5: library residency boundary

The selected shader backend owns a stateless residency adapter. The version1, sixteen-field query and explicit pressure entry point wrap the existing IR artifact/disk statistics and MSL canonical repository. The field contract is in LIBRARY-RESIDENCY-ABI.md; payload bytes keep different names because serialized metallib and uncompressed source are not comparable physical-memory measurements.

No cache algorithm, budget, ownership or warning callback changed. IR retains 64 entries / 8 MiB and delegates pressure to existing idle compiler allocation relief. MSL delegates to the configured repository trim, including its existing highwater and protection. Stronger memory-warning cleanup remains #331. MSL ShaderModule.h/mm are byte-identical to phase4.

Validation: Release native build passed. The initial platform include-order failure was repaired by including the normal MVKFoundation header; its log is retained. Shared Debug Tool ticket2228 ran six populated pipeline-cache cases: MSL graphics/compute and IR graphics/compute cold/warm. Every GPU output oracle passed under Metal validation. Both routes retained two entries; limits were MSL512 / IR64. Consecutive reads were identical. Under-budget pressure calls preserved entries, eviction/restore counts and payload bytes. Warm IR restored both entries. Null handles and short arrays rejected safely. Earlier ticket2227 used an empty MSL repository; it is not the populated-repository evidence.

This is an interface/behavior preservation test, not a proof of memory savings, over-budget eviction or stronger warning cleanup. Raw evidence is outside Git under ir-memory-plan/phase5-evidence.
