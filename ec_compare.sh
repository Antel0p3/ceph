#!/usr/bin/env bash
# Erasure code comparison: twotone vs jerasure (RS/Cauchy) vs isa
# Usage: bash ec_compare.sh
# Run from /root/ceph/build

set -e

BENCH=./bin/ceph_erasure_code_benchmark
PLUGIN_DIR=./lib
SIZE=$((4 * 1024 * 1024))       # MB per iteration
TOTAL=$((16 * 1024 * 1024 * 1024)) # MB total per config
ITER=$(( TOTAL / SIZE ))

# k/m pairs to test (twotone requires m <= k)
KM_PAIRS="6/3 3/2 4/2 10/4"

printf "%-12s %-10s %-6s %-6s %-8s %10s\n" \
    "plugin" "technique" "k" "m" "workload" "MB/s"
printf '%s\n' "$(printf '%.0s-' {1..60})"

run_bench() {
    local plugin=$1 k=$2 m=$3 workload=$4
    shift 4
    local extra_params=("$@")

    result=$($BENCH \
        --plugin "$plugin" \
        --workload "$workload" \
        --iterations "$ITER" \
        --size "$SIZE" \
        --erasures 1 \
        --erasure-code-dir "$PLUGIN_DIR" \
        --parameter k="$k" \
        --parameter m="$m" \
        "${extra_params[@]}" 2>/dev/null)

    local seconds total
    read -r seconds total <<< "$result"
    if [[ -z "$seconds" || "$seconds" == "0" || "$seconds" == "0.000000" ]]; then
        echo "N/A"
    else
        echo "$(echo "scale=1; ($total / 1024 / 1024) / $seconds" | bc -l)"
    fi
}

for km in $KM_PAIRS; do
    k=${km%/*}
    m=${km#*/}

    for workload in encode decode; do
        # TwoTone
        mbps=$(run_bench twotone "$k" "$m" "$workload")
        printf "%-12s %-10s %-6s %-6s %-8s %10s\n" \
            "twotone" "-" "$k" "$m" "$workload" "$mbps"

        # Jerasure Reed-Solomon Vandermonde
        mbps=$(run_bench jerasure "$k" "$m" "$workload" \
            --parameter technique=reed_sol_van \
            --parameter jerasure-per-chunk-alignment=true)
        printf "%-12s %-10s %-6s %-6s %-8s %10s\n" \
            "jerasure" "rs_van" "$k" "$m" "$workload" "$mbps"

        # Jerasure Cauchy
        
        # mbps=$(run_bench jerasure "$k" "$m" "$workload" \
        #     --parameter technique=cauchy_good \
        #     --parameter jerasure-per-chunk-alignment=true)
        # printf "%-12s %-10s %-6s %-6s %-8s %10s\n" \
        #     "jerasure" "cauchy" "$k" "$m" "$workload" "$mbps"

        # ISA-L Reed-Solomon
        mbps=$(run_bench isa "$k" "$m" "$workload" \
            --parameter technique=reed_sol_van)
        printf "%-12s %-10s %-6s %-6s %-8s %10s\n" \
            "isa" "rs_van" "$k" "$m" "$workload" "$mbps"

        printf '%s\n' "$(printf '%.0s-' {1..60})"
    done
done