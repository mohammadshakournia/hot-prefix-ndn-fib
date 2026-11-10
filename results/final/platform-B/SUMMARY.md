# Final results: DESKTOP-QJGDBDC

- OS: Windows-11-10.0.26300-SP0
- CPU: {"Name": "AMD Ryzen 5 5600H with Radeon Graphics         ", "NumberOfCores": 6, "NumberOfLogicalProcessors": 12, "L2CacheSize": 3072, "L3CacheSize": 16384, "MaxClockSpeed": 3301}
- Compiler: g++.EXE (x86_64-win32-seh-rev0, Built by MinGW-W64 project) 8.1.0
- Validated engine runs: 6932, runs with LPM errors: 0

Tables: F10_memory.md, F1_main.md, F2_modecheck.md, F3_nest.md, F4_capacity.md, F5_dynamics.md, F6_updates.md, F7_domain.md, F7_domain_vs_two_stage.md, F8_paper.md, F9_tail.md, stageD_model.md

Figures: F1_main.pdf, F6_updates.pdf, fig_stageD_pred_vs_measured.pdf, fig_stageD_regime_map.pdf

## F1 headline (Hot-prefix final vs hash FIB, % [95% CI])

| FIB | alpha | Hot-prefix | A/A |
|---|---|---|---|
| 50,000 | 0.8 | -23.2 [-24.7, -21.6] | -0.1 [-1.5, +1.5] |
| 50,000 | 1.0 | -14.9 [-15.4, -14.4] | +0.2 [-1.3, +1.7] |
| 50,000 | 1.2 | -4.7 [-6.1, -3.3] | -0.1 [-0.8, +0.7] |
| 500,000 | 0.8 | -15.3 [-18.0, -12.6] | +0.1 [-2.2, +2.4] |
| 500,000 | 1.0 | -6.0 [-8.1, -3.9] | -0.7 [-3.1, +2.0] |
| 500,000 | 1.2 | +8.2 [+5.5, +10.7] | -0.0 [-1.7, +1.7] |
| 2,000,000 | 0.8 | -11.3 [-13.3, -9.1] | +0.4 [-1.3, +2.4] |
| 2,000,000 | 1.0 | -3.9 [-5.3, -2.5] | +0.3 [-2.0, +2.9] |
| 2,000,000 | 1.2 | +13.5 [+11.0, +15.8] | -1.0 [-2.4, +0.5] |
| 4,000,000 | 0.8 | -8.8 [-11.7, -6.1] | +1.1 [-1.0, +3.3] |
| 4,000,000 | 1.0 | -2.4 [-5.6, +0.4] | -0.7 [-3.1, +1.8] |
| 4,000,000 | 1.2 | +12.1 [+8.8, +15.6] | -0.7 [-2.7, +2.1] |
