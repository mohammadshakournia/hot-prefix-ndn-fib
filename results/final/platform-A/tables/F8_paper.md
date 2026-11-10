F8: first-prototype dataset (50K, no nesting, object suffix, C=512), incl. the prototype engines

Reduction of mean latency vs Hash FIB % [95% CI over seeds] (hit ratio). 'A/A' = identical copy of the baseline (noise floor).

| tag | hash ns | Hot-prefix B2 (v2) | Hot-prefix B2 (earlier design) | Hot-prefix B1 | Kim (128-bit) | Hot-prefix (final) | Trie | Hash (v2) | Trie (v2) | PBC | Hot-prefix B1 (v2) | A/A |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| paper-dataset | 219 | -586.2 [-605.1, -557.6] (0.57) | -42.6 [-47.0, -38.1] (0.57) | -106.9 [-115.8, -99.4] (0.57) | -45.1 [-50.4, -40.1] (0.48) | -12.2 [-14.0, -9.9] (0.41) | -403.3 [-410.3, -397.8] | -538.0 [-563.0, -508.0] | -799.3 [-812.2, -786.0] | -44.0 [-48.6, -39.8] (0.48) | -653.8 [-673.4, -628.7] (0.57) | -2.3 [-6.7, +2.2] |
