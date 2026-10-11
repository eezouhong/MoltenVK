# Library residency adapter ABI, version1

Sixteen uint64 fields. Read-only snapshots are approximate/nonblocking. Array order is fixed; availability0 is unknown, not an observed zero. Scope1 is IR recent Artifact strong references; scope2 is MSL canonical repository payloads. Counts do not measure PSO count or physical memory.

|Index/name|Object scope|Time scope|Unit/meaning|Validity|
|---|---|---|---|---|
|0 schema|interface|constant|version1|always|
|1 available|selected data source|snapshot|bool|always|
|2 objectScope|interface|constant|1 IR recent /2 MSL canonical|always|
|3 residentEntries|selected objectScope|gauge|entries; scopes differ|available|
|4 residentLimitEntries|selected objectScope|config|entries; IR0 retain-none,MSL0 unlimited|available|
|5 evictions|selected objectScope|cumulative|IR recent releases/MSL resident cold transitions|available|
|6 restores|selected objectScope|cumulative|IR successful disk restores/MSL rehydrations|available|
|7 irSerializedMetallibBytes|IR recent artifacts|gauge|serialized metallib bytes,not physical allocation|available and payloadMask1|
|8 mslResidentUncompressedSourceBytes|MSL resident payload|gauge|uncompressed source bytes,not library physical size|available and payloadMask2|
|9 diskAvailable|IR DiskCache|snapshot|enabled bool;MSL disk belongs to program service|IR available|
|10 diskIndexedBytes|IR DiskCache|gauge|indexed file bytes|diskAvailable|
|11 diskEntries|IR DiskCache|gauge|indexed files|diskAvailable|
|12 metadataEntries|selected objectScope|gauge|IR weak/negative index entries;MSL logical membership|available|
|13 payloadMask|interface|constant|1 IR bytes/2 MSL bytes|always|
|14 trimImplementation|interface|constant|1 existing IR idle malloc relief;2 MSL configured-limit trim|always|
|15 coverageAvailable|selected source|snapshot|IR lifetime from-before-first-artifact/MSL source snapshot valid|available|

The pressure interface forwards existing algorithms only. It does not lower targets, clear IR caches, change protection/recency or invoke UIKit warnings. IR forwards idle compiler allocation relief; MSL forwards trimToResidentLimit with its existing highwater/protection. #331 must decide any stronger warning policy separately. Querying the interface cannot create/retain libraries or change eviction counts.
