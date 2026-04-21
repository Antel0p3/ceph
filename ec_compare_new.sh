#!/usr/bin/env bash
# ec_compare_new.sh
# Erasure code comparison: twotone vs jerasure (RS-Van) vs ISA-L
# Mirrors the per-erasure-case logic of coding_benchmark_cases.sh,
# but runs from /root/ceph/build without requiring cluster or ninja install.
#
# Usage: bash ec_compare_new.sh [k] [m] [chunk_size_kb] [test_count] [max_data_erasures]
# Defaults: k=3  m=2  chunk=2048KB  count=1000  max_data_erasures=1

set -uo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    echo "Usage: $0 [k] [m] [chunk_size_kb] [test_count] [max_data_erasures]"
    echo "  max_data_erasures  only test cases where lost data chunks <= N (default: 1)"
    exit 0
fi

K=${1:-3}
M=${2:-2}
CHUNK_SIZE_KB=${3:-2048}
TEST_COUNT=${4:-1000}
MAX_DATA_ERASURES=${5:-1}   # TwoTone 2+ data erasures are O(n²), default to 1
TOTAL_CHUNKS=$((K + M))
BUFFER_SIZE=$((CHUNK_SIZE_KB * K * 1024))

BENCH=./bin/ceph_erasure_code_benchmark
PLUGIN_DIR=./lib

# Validate directory
if [[ ! -f "$BENCH" ]]; then
    echo "Error: $BENCH not found. Run this script from /root/ceph/build"
    exit 1
fi

# ── plugins ───────────────────────────────────────────────────────────────────
# technique="" means no --parameter technique= is added (twotone has none)
PLUGINS=("twotone" "jerasure" "isa")
TECHNIQUES=("" "reed_sol_van" "reed_sol_van")

# ── erasure test cases (data lost : parity lost) ─────────────────────────────
declare -A TEST_CASES=()
ORDERED_CASES=()

# 1 erasure
TEST_CASES["1data-0parity"]="1:0"
TEST_CASES["0data-1parity"]="0:1"
ORDERED_CASES+=("1data-0parity" "0data-1parity")

# 2 erasures
if [ "$M" -ge 2 ]; then
    TEST_CASES["2data-0parity"]="2:0"
    TEST_CASES["1data-1parity"]="1:1"
    TEST_CASES["0data-2parity"]="0:2"
    ORDERED_CASES+=("2data-0parity" "1data-1parity" "0data-2parity")
    # ORDERED_CASES+=("1data-1parity" "0data-2parity")
fi

# 3 erasures
if [ "$M" -ge 3 ]; then
    TEST_CASES["3data-0parity"]="3:0"
    TEST_CASES["2data-1parity"]="2:1"
    TEST_CASES["1data-2parity"]="1:2"
    TEST_CASES["0data-3parity"]="0:3"
    ORDERED_CASES+=("3data-0parity" "2data-1parity" "1data-2parity" "0data-3parity")
    # ORDERED_CASES+=("1data-2parity" "0data-3parity")
fi

# 4 erasures
if [ "$M" -ge 4 ]; then
    TEST_CASES["4data-0parity"]="4:0"
    TEST_CASES["3data-1parity"]="3:1"
    TEST_CASES["2data-2parity"]="2:2"
    TEST_CASES["1data-3parity"]="1:3"
    TEST_CASES["0data-4parity"]="0:4"
    ORDERED_CASES+=("4data-0parity" "3data-1parity" "2data-2parity" "1data-3parity" "0data-4parity")
    # ORDERED_CASES+=("1data-3parity" "0data-4parity")
fi

# Filter cases by max_data_erasures
FILTERED_CASES=()
for case in "${ORDERED_CASES[@]}"; do
    IFS=':' read -r d _p <<< "${TEST_CASES[$case]}"
    [ "$d" -le "$MAX_DATA_ERASURES" ] && FILTERED_CASES+=("$case")
done
ORDERED_CASES=("${FILTERED_CASES[@]}")

NUM_CASES=${#ORDERED_CASES[@]}
PER_CASE_COUNT=$(( (TEST_COUNT + NUM_CASES - 1) / NUM_CASES ))

# ── print configuration ───────────────────────────────────────────────────────
echo ""
echo "----------------------------------------------------------"
echo "Test Configuration:"
echo "  Chunk size:      ${CHUNK_SIZE_KB}KB"
echo "  Buffer size:     $((CHUNK_SIZE_KB * K))KB"
echo "  Total tests:     $TEST_COUNT"
echo "  Per-case tests:  $PER_CASE_COUNT"
echo "  Erasure scheme:  K=${K}, M=${M}, Total chunks=${TOTAL_CHUNKS}"
echo "Test Cases:"
for case in "${ORDERED_CASES[@]}"; do
    IFS=':' read -r d p <<< "${TEST_CASES[$case]}"
    echo "  $case — missing: ${d} data + ${p} parity"
done

# ── helpers ───────────────────────────────────────────────────────────────────

# Build "--erased N --erased M ..." string from random selection
select_erased_indexes() {
    local data_to_erase=$1 parity_to_erase=$2
    local erased_args=""
    if [ "$data_to_erase" -gt 0 ]; then
        while IFS= read -r idx; do
            erased_args+=" --erased $idx"
        done < <(shuf -i "0-$((K-1))" -n "$data_to_erase")
    fi
    if [ "$parity_to_erase" -gt 0 ]; then
        while IFS= read -r idx; do
            erased_args+=" --erased $idx"
        done < <(shuf -i "$K-$((TOTAL_CHUNKS-1))" -n "$parity_to_erase")
    fi
    echo "$erased_args"
}

# Run one benchmark command; prints throughput MB/s to stdout or "FAILED"
run_bench() {
    local plugin=$1 technique=$2 workload=$3 iters=$4
    shift 4
    local extra_flags=("$@")   # e.g. --erased N --erased M ...

    local cmd=("$BENCH"
        --plugin        "$plugin"
        --workload      "$workload"
        --iterations    "$iters"
        --size          "$BUFFER_SIZE"
        --erasure-code-dir "$PLUGIN_DIR"
        --parameter     "k=$K"
        --parameter     "m=$M"
    )
    [[ -n "$technique" ]] && cmd+=(--parameter "technique=$technique")
    cmd+=("${extra_flags[@]}")

    local output
    output=$("${cmd[@]}" 2>&1)

    local time_s total_kb
    time_s=$(echo "$output" | grep -oP '^\d+\.\d+')
    total_kb=$(echo "$output" | grep -oP '\d+$')

    if [[ -n "$time_s" && -n "$total_kb" && "$time_s" != "0.000000" ]]; then
        python3 -c "print(round($total_kb / 1024 / $time_s, 2))"
    else
        echo "FAILED"
    fi
}

# ── warmup ────────────────────────────────────────────────────────────────────
echo ""
echo "----------------------------------------------------------"
echo "Running warmup..."
erased_warmup=$(select_erased_indexes 1 0)
for i in "${!PLUGINS[@]}"; do
    plugin="${PLUGINS[$i]}"
    technique="${TECHNIQUES[$i]}"
    # encode warmup
    run_bench "$plugin" "$technique" encode 2 > /dev/null
    # decode warmup
    # shellcheck disable=SC2086
    run_bench "$plugin" "$technique" decode 2 $erased_warmup > /dev/null
done
echo "Warmup done."

# ── encoding ──────────────────────────────────────────────────────────────────
echo ""
echo "----------------------------------------------------------"
echo "ENCODING PERFORMANCE TESTS"

declare -A encode_results
for i in "${!PLUGINS[@]}"; do
    plugin="${PLUGINS[$i]}"
    technique="${TECHNIQUES[$i]}"
    echo -n "  $plugin ... "
    mbps=$(run_bench "$plugin" "$technique" encode "$TEST_COUNT")
    encode_results["$plugin"]="$mbps"
    echo "${mbps} MB/s"
done

# ── decoding (per erasure case) ───────────────────────────────────────────────
echo ""
echo "----------------------------------------------------------"
echo "DECODING PERFORMANCE TESTS"

declare -A decode_total
for plugin in "${PLUGINS[@]}"; do
    decode_total["$plugin"]=0
done

tested_cases=0

for case in "${ORDERED_CASES[@]}"; do
    echo ""
    echo "  Case: $case"
    IFS=':' read -r data_chunks parity_chunks <<< "${TEST_CASES[$case]}"
    total_erased=$((data_chunks + parity_chunks))
    erased_args=$(select_erased_indexes "$data_chunks" "$parity_chunks")

    for i in "${!PLUGINS[@]}"; do
        plugin="${PLUGINS[$i]}"
        technique="${TECHNIQUES[$i]}"
        echo -n "    $plugin ... "
        # shellcheck disable=SC2086
        mbps=$(run_bench "$plugin" "$technique" decode "$PER_CASE_COUNT" \
               --erasures "$total_erased" $erased_args)
        echo "${mbps} MB/s"
        if [[ "$mbps" != "FAILED" ]]; then
            decode_total["$plugin"]=$(python3 -c \
                "print(${decode_total[$plugin]} + $mbps)")
        fi
    done
    ((tested_cases++))
done

# ── summary ───────────────────────────────────────────────────────────────────
declare -A decode_avg
for plugin in "${PLUGINS[@]}"; do
    decode_avg["$plugin"]=$(python3 -c \
        "print(round(${decode_total[$plugin]} / max($tested_cases,1), 2))")
done

echo ""
echo "----------------------------------------------------------"
echo "PERFORMANCE SUMMARY  (K=${K}  M=${M}  chunk=${CHUNK_SIZE_KB}KB)"
echo ""
echo "ENCODING:"
for plugin in "${PLUGINS[@]}"; do
    mbps=${encode_results[$plugin]}
    gbps=$(python3 -c "print('{:.2f}'.format(${mbps:-0} / 1024))" 2>/dev/null || echo "?")
    printf "  %-12s %8s MB/s  (%s GB/s)\n" "$plugin" "$mbps" "$gbps"
done
echo ""
echo "DECODING (average of $tested_cases erasure cases):"
for plugin in "${PLUGINS[@]}"; do
    mbps=${decode_avg[$plugin]}
    gbps=$(python3 -c "print('{:.2f}'.format(${mbps:-0} / 1024))" 2>/dev/null || echo "?")
    printf "  %-12s %8s MB/s  (%s GB/s)\n" "$plugin" "$mbps" "$gbps"
done
echo ""
echo "All tests completed."
