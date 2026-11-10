# Final results: vm

- OS: Linux-6.18.44-fc-v153-x86_64-with-glibc2.39
- CPU: "Architecture:                            x86_64\nCPU op-mode(s):                          32-bit, 64-bit\nAddress sizes:                           52 bits physical, 57 bits virtual\nByte Order:                              Little Endian\nCPU(s):                                  2\nOn-line CPU(s) list:                     0,1\nVendor ID:                               GenuineIntel\nModel name:                              Intel(R) Xeon(R) Processor @ 2.10GHz\nCPU family:                              6\nModel:                                   207\nThread(s) per core:                      1\nCore(s) per soc"
- Compiler: g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- Validated engine runs: 6932, runs with LPM errors: 0

Tables: F10_memory.md, F1_main.md, F2_modecheck.md, F3_nest.md, F4_capacity.md, F5_dynamics.md, F6_updates.md, F7_domain.md, F7_domain_vs_two_stage.md, F8_paper.md, F9_tail.md, stageD_model.md

Figures: F1_main.pdf, F6_updates.pdf, fig_stageD_pred_vs_measured.pdf, fig_stageD_regime_map.pdf

## F1 headline (Hot-prefix final vs hash FIB, % [95% CI])

| FIB | alpha | Hot-prefix | A/A |
|---|---|---|---|
| 50,000 | 0.8 | -23.7 [-25.2, -22.3] | -0.4 [-2.3, +1.5] |
| 50,000 | 1.0 | -15.5 [-17.3, -14.0] | -0.1 [-1.7, +1.5] |
| 50,000 | 1.2 | -4.5 [-7.2, -1.6] | +0.1 [-1.1, +1.5] |
| 500,000 | 0.8 | -14.8 [-16.5, -13.2] | +0.4 [-1.7, +2.5] |
| 500,000 | 1.0 | -6.8 [-7.6, -5.9] | -0.2 [-1.8, +1.5] |
| 500,000 | 1.2 | +10.5 [+7.9, +13.0] | +0.2 [-1.1, +1.5] |
| 2,000,000 | 0.8 | -10.2 [-12.2, -8.0] | -0.1 [-2.0, +1.7] |
| 2,000,000 | 1.0 | -2.2 [-3.5, -0.7] | +0.8 [-1.3, +2.8] |
| 2,000,000 | 1.2 | +14.5 [+12.4, +16.7] | -0.0 [-2.0, +1.7] |
| 4,000,000 | 0.8 | -9.9 [-12.0, -7.8] | -1.5 [-4.4, +1.3] |
| 4,000,000 | 1.0 | -2.9 [-4.6, -1.2] | +0.9 [-0.8, +2.7] |
| 4,000,000 | 1.2 | +15.1 [+12.5, +17.5] | +0.5 [-1.3, +2.3] |
