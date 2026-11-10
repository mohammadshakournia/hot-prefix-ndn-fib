F2: sequential vs interleaved measurement (alpha 1.2)

Reduction of mean latency vs Hash FIB % [95% CI over seeds] (hit ratio). 'A/A' = identical copy of the baseline (noise floor).

| n | mode | hash ns | A/A | Hot-prefix (final) |
|---|---|---|---|---|
| 500000 | interleave | 236 | -0.4 [-1.9, +1.2] | +6.2 [+3.0, +9.3] (0.66) |
| 500000 | timeline | 242 | -1.7 [-5.8, +2.6] | +0.5 [-7.1, +8.0] (0.66) |
| 2000000 | interleave | 332 | +0.1 [-3.2, +3.0] | +15.2 [+12.2, +17.6] (0.66) |
| 2000000 | timeline | 386 | +4.8 [-2.7, +13.0] | +20.8 [+15.2, +26.3] (0.66) |
