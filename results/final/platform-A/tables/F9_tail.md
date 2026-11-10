F9: per-lookup latency (TSC), 2M FIB, C=1024; medians over seeds x runs. Hit / miss cost over the last 100K requests.

| alpha | engine | mean | P50 | P90 | P99 | P99.9 | hit cost | miss cost |
|---|---|---|---|---|---|---|---|---|
| 1.0 | Hash FIB | 648 | 598 | 981 | 1383 | 12014 | 0 | 688 |
| 1.0 | Two-stage hash | 685 | 624 | 1203 | 1755 | 15667 | 0 | 650 |
| 1.0 | Hot-prefix (final) | 658 | 606 | 1030 | 1475 | 13512 | 211 | 885 |
| 1.0 | PBC | 882 | 859 | 1624 | 2569 | 25030 | 176 | 1232 |
| 1.2 | Hash FIB | 404 | 307 | 742 | 1135 | 6410 | 0 | 387 |
| 1.2 | Two-stage hash | 363 | 200 | 881 | 1496 | 5448 | 0 | 340 |
| 1.2 | Hot-prefix (final) | 334 | 150 | 799 | 1230 | 5653 | 149 | 761 |
| 1.2 | PBC | 459 | 133 | 1184 | 2259 | 8926 | 136 | 1175 |
