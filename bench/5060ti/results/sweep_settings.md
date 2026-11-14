# Sweep of engine settings on the RTX 5060 Ti (before the tuning was built in)

Time of one pass in ms; 256 tokens per task, greedy, context 16,384, speed model. "1 row" is the prose task without look-ahead,
"8 rows" the prose task and "16 rows" the CSV-to-JSON task with the shipping look-ahead. The generated tokens were the same in every row.
Noise between repeats of the same setting is about 0.1 ms.

| label | settings | 1 row | 8 rows | 16 rows |
|---|---|---:|---:|---:|
| base | (defaults) | 26.18 | 34.20 | 41.02 |
| cfg0 | YSKN_LNO_CFG=0 | 26.19 | 33.95 | 40.93 |
| cfg1 | YSKN_LNO_CFG=1 | 26.12 | 33.75 | 40.87 |
| cfg2 | YSKN_LNO_CFG=2 | 25.93 | 34.15 | 41.02 |
| look2 | YSKN_LNO_LOOK=2 | 26.13 | 34.47 | 41.90 |
| prew0 | YSKN_LNO_PREW=0 | 26.17 | 34.26 | 41.18 |
| prew2 | YSKN_LNO_PREW=2 | 26.11 | 34.09 | 40.88 |
| prew4 | YSKN_LNO_PREW=4 | 26.12 | 34.17 | 41.03 |
| pre0 | YSKN_LNO_PRE=0 | 26.47 | 34.78 | 41.65 |
| pre1 | YSKN_LNO_PRE=1 | 26.17 | 34.62 | 41.04 |
| pre2 | YSKN_LNO_PRE=2 | 26.17 | 34.34 | 41.31 |
| pre7 | YSKN_LNO_PRE=7 | 26.14 | 34.10 | 40.92 |
| nopdl | YSKN_NO_PDL=1 | 26.81 | 34.89 | 42.12 |
| want144 | YSKN_Q35_WANT=144 | 26.11 | 38.57 | 44.66 |
| want576 | YSKN_Q35_WANT=576 | 26.13 | 33.47 | 40.92 |
| want1152 | YSKN_Q35_WANT=1152 | 26.13 | 33.34 | 41.78 |
| l2pf4 | YSKN_L2PF=4 | 26.22 | 34.20 | 41.16 |
| l2pf28 | YSKN_L2PF=28 | 26.15 | 34.34 | 42.24 |
| l2pf2_4 | YSKN_L2PF2=4 | 26.56 | 35.23 | 43.65 |
| lo0 | YSKN_Q35_LO=0 | 26.22 | 34.74 | 41.15 |
| bdrcfg0 | YSKN_BDR_CFG=0 | 26.16 | 34.18 | 41.37 |
| bdrcfg1 | YSKN_BDR_CFG=1 | 26.19 | 34.16 | 41.17 |
| bdrcfg2 | YSKN_BDR_CFG=2 | 26.17 | 34.17 | 41.08 |
| xsmr2 | YSKN_LNO_XSMR=2 | 26.17 | 34.22 | 46.09 |
| xsmr4 | YSKN_LNO_XSMR=4 | 26.15 | 34.16 | 50.44 |
| nwhalf | YSKN_Q35_NW=down=5,out=2,o=2,gate=3,up=3,qkv=4,z=4,q=2,k=2,v=2 | 26.54 | 34.26 | 41.24 |
| nwquarter | YSKN_Q35_NW=down=2,out=1,o=1,gate=1,up=1,qkv=2,z=2,q=1,k=1,v=1 | 27.30 | 34.34 | 41.01 |
| nwdouble | YSKN_Q35_NW=down=18,out=8,o=8,gate=10,up=10,qkv=16,z=16,q=8,k=8,v=8 | 26.10 | 34.16 | 41.25 |
| base2 | (defaults) | - | 34.25 | 41.22 |
| w432 | YSKN_Q35_WANT=432 | - | 33.86 | 41.35 |
| w576b | YSKN_Q35_WANT=576 | - | 33.61 | 40.89 |
| w720 | YSKN_Q35_WANT=720 | - | 33.57 | 41.38 |
| w864 | YSKN_Q35_WANT=864 | - | 33.42 | 41.31 |
| w576_cfg1 | YSKN_Q35_WANT=576 YSKN_LNO_CFG=1 | - | 33.56 | 41.20 |
| w576_cfg0 | YSKN_Q35_WANT=576 YSKN_LNO_CFG=0 | - | 33.59 | 40.85 |
| w864_cfg1 | YSKN_Q35_WANT=864 YSKN_LNO_CFG=1 | - | 33.45 | 41.40 |
| w576_pre7_prew2 | YSKN_Q35_WANT=576 YSKN_LNO_PRE=7 YSKN_LNO_PREW=2 | - | 33.54 | 41.02 |
| w576_pdlprew | YSKN_Q35_WANT=576 YSKN_PDL_PREW_M=8 | - | 33.58 | 41.15 |
| w576_l2pf4 | YSKN_Q35_WANT=576 YSKN_L2PF=4 | - | 33.63 | 41.38 |
| w576_headw5 | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=5 | - | 33.35 | 40.51 |
| w576_headw20 | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=20 | - | 33.54 | 40.95 |
| w576_shead256 | YSKN_Q35_WANT=576 YSKN_BDR_SHEAD_M=256 | - | 33.47 | 40.96 |
| w576_headw3 | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=3 | - | 33.25 | 39.89 |
| w576_headw4 | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=4 | - | 33.31 | 40.21 |
| w576_headw5b | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=5 | - | 33.38 | 40.21 |
| w576_headw7 | YSKN_Q35_WANT=576 YSKN_Q35_HEAD_W=7 | - | 33.39 | 40.71 |
| w864_headw5 | YSKN_Q35_WANT=864 YSKN_Q35_HEAD_W=5 | - | 33.23 | 40.64 |
