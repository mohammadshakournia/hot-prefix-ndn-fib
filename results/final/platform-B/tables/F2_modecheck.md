F2: sequential vs interleaved measurement (alpha 1.2)

Reduction of mean latency vs Hash FIB % [95% CI over seeds] (hit ratio). 'A/A' = identical copy of the baseline (noise floor).

| n | mode | hash ns | A/A | Hot-prefix (final) |
|---|---|---|---|---|
| 500000 | interleave | 168 | -0.9 [-3.6, +1.9] | +11.0 [+7.0, +14.9] (0.66) |
| 500000 | timeline | 190 | -0.1 [-0.7, +0.5] | +11.0 [+6.7, +15.3] (0.66) |
| 2000000 | interleave | 195 | -1.3 [-4.6, +2.0] | +18.6 [+15.3, +21.9] (0.66) |
| 2000000 | timeline | 242 | +2.6 [+1.2, +4.5] | +21.6 [+19.4, +23.9] (0.66) |
