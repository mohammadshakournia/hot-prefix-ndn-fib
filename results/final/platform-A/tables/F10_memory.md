F10: allocator-counted memory. Engine state = live bytes after the pass minus the hash-FIB run (harness buffers).

| FIB | C | engine | engine state (KiB) |
|---|---|---|---|
| 2,000,000 | 1024 | Full-name cache | 333 |
| 2,000,000 | 1024 | Hot-prefix B2 (earlier design) | 365 |
| 2,000,000 | 1024 | Hot-prefix B1 | 1517 |
| 2,000,000 | 1024 | PBC | 428 |
| 2,000,000 | 1024 | Hot-prefix (final) | 330 |
| 2,000,000 | 1024 | Kim (128-bit) | 368 |
| 2,000,000 | 1024 | hot_b0 | 4496 |
| 2,000,000 | 16384 | Full-name cache | 5240 |
| 2,000,000 | 16384 | Hot-prefix B2 (earlier design) | 4417 |
| 2,000,000 | 16384 | Hot-prefix B1 | 13679 |
| 2,000,000 | 16384 | PBC | 5694 |
| 2,000,000 | 16384 | Hot-prefix (final) | 4270 |
| 2,000,000 | 16384 | Kim (128-bit) | 5573 |
| 2,000,000 | 16384 | hot_b0 | 8311 |
| 500,000 | 1024 | Full-name cache | 339 |
| 500,000 | 1024 | Hot-prefix B2 (earlier design) | 374 |
| 500,000 | 1024 | Hot-prefix B1 | 1508 |
| 500,000 | 1024 | PBC | 411 |
| 500,000 | 1024 | Hot-prefix (final) | 336 |
| 500,000 | 1024 | Kim (128-bit) | 368 |
| 500,000 | 1024 | hot_b0 | 3864 |
| 500,000 | 16384 | Full-name cache | 5291 |
| 500,000 | 16384 | Hot-prefix B2 (earlier design) | 4428 |
| 500,000 | 16384 | Hot-prefix B1 | 12927 |
| 500,000 | 16384 | PBC | 5588 |
| 500,000 | 16384 | Hot-prefix (final) | 4281 |
| 500,000 | 16384 | Kim (128-bit) | 5484 |
| 500,000 | 16384 | hot_b0 | 7680 |

| FIB | trie (MiB) | hash FIB (MiB) | two-stage (MiB) | guard index (MiB) |
|---|---|---|---|---|
| 2,000,000 | 0 | 242 | 432 | 175 |
| 500,000 | 195 | 60 | 110 | 45 |
