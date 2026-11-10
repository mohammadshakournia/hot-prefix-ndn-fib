F9: per-lookup latency (TSC), 2M FIB, C=1024; medians over seeds x runs. Hit / miss cost over the last 100K requests.

| alpha | engine | mean | P50 | P90 | P99 | P99.9 | hit cost | miss cost |
|---|---|---|---|---|---|---|---|---|
| 1.0 | Hash FIB | 305 | 281 | 491 | 771 | 1703 | 0 | 299 |
| 1.0 | Two-stage hash | 337 | 271 | 641 | 982 | 2054 | 0 | 318 |
| 1.0 | Hot-prefix (final) | 304 | 291 | 541 | 852 | 2004 | 91 | 446 |
| 1.0 | PBC | 493 | 501 | 1052 | 1593 | 3276 | 100 | 694 |
| 1.2 | Hash FIB | 235 | 180 | 431 | 721 | 1312 | 0 | 234 |
| 1.2 | Two-stage hash | 197 | 130 | 461 | 872 | 1363 | 0 | 186 |
| 1.2 | Hot-prefix (final) | 178 | 90 | 441 | 751 | 1463 | 83 | 402 |
| 1.2 | PBC | 263 | 100 | 711 | 1483 | 2605 | 91 | 701 |
