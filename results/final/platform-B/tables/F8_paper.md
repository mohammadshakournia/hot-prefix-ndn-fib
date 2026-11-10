F8: first-prototype dataset (50K, no nesting, object suffix, C=512), incl. the prototype engines

Reduction of mean latency vs Hash FIB % [95% CI over seeds] (hit ratio). 'A/A' = identical copy of the baseline (noise floor).

| tag | hash ns | Hot-prefix B2 (v2) | Hot-prefix B2 (earlier design) | Hot-prefix B1 | Kim (128-bit) | Hot-prefix (final) | Trie | Hash (v2) | Trie (v2) | PBC | Hot-prefix B1 (v2) | A/A |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| paper-dataset | 123 | -622.8 [-641.8, -597.5] (0.57) | -48.0 [-54.8, -41.6] (0.57) | -112.4 [-120.7, -102.3] (0.57) | -40.9 [-45.6, -35.9] (0.48) | -12.1 [-14.4, -9.5] (0.41) | -546.6 [-569.4, -512.4] | -655.8 [-690.3, -613.9] | -858.8 [-892.7, -809.2] | -38.2 [-44.1, -31.9] (0.48) | -682.2 [-717.8, -635.9] (0.57) | -5.3 [-8.5, -1.6] |
