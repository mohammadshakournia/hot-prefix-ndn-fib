F10: allocator-counted memory. Engine state = live bytes after the pass minus the hash-FIB run (harness buffers).

| FIB | C | engine | engine state (KiB) |
|---|---|---|---|
| 2,000,000 | 1024 | Full-name cache | 325 |
| 2,000,000 | 1024 | Hot-prefix B2 (earlier design) | 346 |
| 2,000,000 | 1024 | Hot-prefix B1 | 1417 |
| 2,000,000 | 1024 | PBC | 410 |
| 2,000,000 | 1024 | Hot-prefix (final) | 315 |
| 2,000,000 | 1024 | Kim (128-bit) | 352 |
| 2,000,000 | 1024 | hot_b0 | 3972 |
| 2,000,000 | 16384 | Full-name cache | 5104 |
| 2,000,000 | 16384 | Hot-prefix B2 (earlier design) | 4340 |
| 2,000,000 | 16384 | Hot-prefix B1 | 12845 |
| 2,000,000 | 16384 | PBC | 5341 |
| 2,000,000 | 16384 | Hot-prefix (final) | 4261 |
| 2,000,000 | 16384 | Kim (128-bit) | 5306 |
| 2,000,000 | 16384 | hot_b0 | 7791 |
| 500,000 | 1024 | Full-name cache | 330 |
| 500,000 | 1024 | Hot-prefix B2 (earlier design) | 352 |
| 500,000 | 1024 | Hot-prefix B1 | 1400 |
| 500,000 | 1024 | PBC | 387 |
| 500,000 | 1024 | Hot-prefix (final) | 319 |
| 500,000 | 1024 | Kim (128-bit) | 351 |
| 500,000 | 1024 | hot_b0 | 3431 |
| 500,000 | 16384 | Full-name cache | 5155 |
| 500,000 | 16384 | Hot-prefix B2 (earlier design) | 4360 |
| 500,000 | 16384 | Hot-prefix B1 | 12129 |
| 500,000 | 16384 | PBC | 5239 |
| 500,000 | 16384 | Hot-prefix (final) | 4270 |
| 500,000 | 16384 | Kim (128-bit) | 5210 |
| 500,000 | 16384 | hot_b0 | 7246 |

| FIB | trie (MiB) | hash FIB (MiB) | two-stage (MiB) | guard index (MiB) |
|---|---|---|---|---|
| 2,000,000 | 0 | 227 | 416 | 168 |
| 500,000 | 179 | 56 | 106 | 43 |
