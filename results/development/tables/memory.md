Allocator-counted memory (bytes live after the pass). Engine state = engine bytes minus the hash-view run (harness buffers); the shared FIB and guard index are listed separately.

| FIB | C | engine | engine state (KiB) |
|---|---|---|---|
| 2,000,000 | 1024 | Full-name cache | 333 |
| 2,000,000 | 1024 | Hot-prefix B2 | 357 |
| 2,000,000 | 1024 | Hot-prefix B1 | 1509 |
| 2,000,000 | 1024 | PBC | 428 |
| 2,000,000 | 1024 | Hot-prefix B2h | 348 |
| 2,000,000 | 1024 | Kim (128-bit) | 368 |
| 2,000,000 | 1024 | hot_b0 | 4488 |
| 2,000,000 | 16384 | Full-name cache | 5240 |
| 2,000,000 | 16384 | Hot-prefix B2 | 4289 |
| 2,000,000 | 16384 | Hot-prefix B1 | 13551 |
| 2,000,000 | 16384 | PBC | 5694 |
| 2,000,000 | 16384 | Hot-prefix B2h | 4298 |
| 2,000,000 | 16384 | Kim (128-bit) | 5573 |
| 2,000,000 | 16384 | hot_b0 | 8183 |
| 500,000 | 1024 | Full-name cache | 339 |
| 500,000 | 1024 | Hot-prefix B2 | 364 |
| 500,000 | 1024 | Hot-prefix B1 | 1500 |
| 500,000 | 1024 | PBC | 411 |
| 500,000 | 1024 | Hot-prefix B2h | 357 |
| 500,000 | 1024 | Kim (128-bit) | 368 |
| 500,000 | 1024 | hot_b0 | 3862 |
| 500,000 | 16384 | Full-name cache | 5291 |
| 500,000 | 16384 | Hot-prefix B2 | 4298 |
| 500,000 | 16384 | Hot-prefix B1 | 12799 |
| 500,000 | 16384 | PBC | 5587 |
| 500,000 | 16384 | Hot-prefix B2h | 4297 |
| 500,000 | 16384 | Kim (128-bit) | 5484 |
| 500,000 | 16384 | hot_b0 | 7559 |

| FIB | trie (MiB) | hash FIB (MiB) | two-stage hash (MiB) | guard index (MiB) |
|---|---|---|---|---|
| 500,000 | 195 | 60 | 110 | 45 |
| 2,000,000 | 0 | 242 | 432 | 175 |
