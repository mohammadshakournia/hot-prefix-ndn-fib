#!/bin/bash
# Stage D: cost-model inputs for every configuration (appends to results/D_profiles.csv)
set -e
cd "$(dirname "$0")/../.."
OUT=results/D_profiles.csv
[ -f $OUT ] || echo "tag,C,rep,n,h,F_hot,F_cold,T_hash,c_hit,c_miss,T_cache" > $OUT
run() { grep -q "^$5," $OUT && return 0; ./profile_v3 "$1" "$2" "$3" 2 $OUT "$5" 2>/dev/null; echo "done $5"; }
for n in 50000 500000 2000000 4000000; do for a in 0.8 1.0 1.2; do for s in 1 2 3; do
  run data/synth_n${n}_nest0.3.csv data/q_synth_n${n}_nest0.3_stationary_a${a}_r0.5_ts_c300000_s${s}.csv 1024 2 "synth;n=$n;alpha=$a;nest=0.3;C=1024;seed=$s"
done; done; done
for a in 0.8 1.0 1.2; do for s in 1 2 3; do
  run data/domain_n1000000.csv data/q_domain_n1000000_stationary_a${a}_r0.5_ts_c300000_s${s}.csv 1024 2 "domain;n=1000000;alpha=$a;nest=dom;C=1024;seed=$s"
done; done
for c in 256 4096 16384; do for s in 1 2; do
  run data/synth_n2000000_nest0.3.csv data/q_synth_n2000000_nest0.3_stationary_a1.2_r0.5_ts_c300000_s${s}.csv $c 2 "synth;n=2000000;alpha=1.2;nest=0.3;C=$c;seed=$s"
done; done
for nest in 0.0 0.5; do for s in 1 2; do
  run data/synth_n500000_nest${nest}.csv data/q_synth_n500000_nest${nest}_stationary_a1.2_r0.5_ts_c300000_s${s}.csv 1024 2 "synth;n=500000;alpha=1.2;nest=$nest;C=1024;seed=$s"
done; done
echo ALLDONE
