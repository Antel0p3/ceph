#!/usr/bin/env bash
# ec_recovery_compare.sh
# Compare EC recovery throughput: TwoTone vs Jerasure-RS vs ISA-L
# across k,m = (3,2), (4,2), (6,3)
#
# Run from /root/ceph/build
set -uo pipefail

BUILD=$(cd "$(dirname "$0")" && pwd)
cd "$BUILD"

CEPH="./bin/ceph"
RADOS="./bin/rados"
CEPH_OSD="./bin/ceph-osd"

# ── colour / print helpers ────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
info()   { echo -e "${CYAN}[>] $*${NC}"; }
banner() { echo -e "\n${BOLD}${YELLOW}══════════════════════════════════════════${NC}"; \
           echo -e "${BOLD}${YELLOW}  $*${NC}"; \
           echo -e "${BOLD}${YELLOW}══════════════════════════════════════════${NC}"; }

# ── results accumulator ───────────────────────────────────────────────────────
# Each entry: "plugin k m time_s peak_bw avg_bw"
declare -a RESULTS=()

# ── cluster lifecycle ─────────────────────────────────────────────────────────
start_cluster() {
    local n_osd=$1
    info "Starting fresh cluster with $n_osd OSDs (bluestore)..."
    ../src/stop.sh 2>/dev/null || true
    rm -rf out/ dev/
    MDS=0 MON=1 MGR=1 OSD=$n_osd \
        ../src/vstart.sh -n -x --without-dashboard --bluestore 2>/dev/null
    for i in $(seq 1 40); do
        local up
        up=$("$CEPH" -s 2>/dev/null | grep -oP '\d+(?= up)' | head -1)
        [[ "$up" == "$n_osd" ]] && break
        sleep 2
    done
    # Remove recovery throttling
    "$CEPH" tell 'osd.*' injectargs \
        '--osd-recovery-max-active-hdd 10 --osd-max-backfills 10
         --osd-recovery-sleep-hdd 0 --osd-recovery-max-active-ssd 10
         --osd-recovery-sleep-ssd 0' 2>/dev/null || true
    info "Cluster up: $("$CEPH" osd stat 2>/dev/null | grep -oP '\d+ up.*')"
}

wait_clean() {
    for i in $(seq 1 120); do
        local s; s=$("$CEPH" -s 2>/dev/null)
        if ! echo "$s" | grep -qE "degraded|recovering|undersized|backfill|peering"; then
            return 0
        fi
        sleep 3
    done
    echo "  [warn] cluster did not become clean within timeout"
}

make_pool() {
    local pool=$1 plugin=$2 k=$3 m=$4 technique=${5:-}
    local prof="${plugin}-k${k}m${m}"
    local extra_args=""
    [[ -n "$technique" ]] && extra_args="technique=$technique"

    "$CEPH" osd erasure-code-profile set "$prof" \
        plugin="$plugin" k="$k" m="$m" crush-failure-domain=osd \
        "directory=$BUILD/lib" $extra_args --force 2>/dev/null
    "$CEPH" osd pool create "$pool" 32 32 erasure "$prof" 2>/dev/null
    "$CEPH" osd pool set "$pool" min_size "$k" 2>/dev/null
    "$CEPH" osd pool application enable "$pool" rados 2>/dev/null

    # wait active+clean
    for i in $(seq 1 30); do
        local active
        active=$("$CEPH" -s 2>/dev/null | grep -oP '\d+(?= active\+clean)' | head -1)
        [[ -n "$active" && "$active" -ge 32 ]] && return 0
        sleep 2
    done
}

delete_pool() {
    local pool=$1
    "$CEPH" osd pool rm "$pool" "$pool" \
        --yes-i-really-really-mean-it 2>/dev/null || true
}

# ── one recovery measurement ──────────────────────────────────────────────────
# Writes BENCH_SECS seconds of 4M objects, kills osd.0, measures recovery.
# Appends "plugin k m time_s peak avg" to RESULTS.
BENCH_SECS=30

run_recovery_test() {
    local plugin=$1 k=$2 m=$3 label=$4   # label = display name, e.g. "jerasure-rs"
    local pool="rectest-${plugin}-k${k}m${m}"

    info "[$label k=$k m=$m] Creating pool..."
    make_pool "$pool" "$plugin" "$k" "$m" \
        "$([ "$plugin" = jerasure ] && echo reed_sol_van; [ "$plugin" = isa ] && echo reed_sol_van)"

    info "[$label k=$k m=$m] Writing ${BENCH_SECS}s of 4M objects..."
    "$RADOS" bench -p "$pool" "$BENCH_SECS" write \
        --no-cleanup --object-size 4M 2>/dev/null | \
        grep -E "Total writes|Bandwidth" | sed 's/^/  /'

    info "[$label k=$k m=$m] Killing osd.0 + marking out..."
    local osd_pid
    osd_pid=$(cat out/osd.0.pid 2>/dev/null || echo "")
    if [[ -z "$osd_pid" ]]; then
        echo "  [skip] osd.0 pid not found"
        delete_pool "$pool"
        return
    fi
    kill "$osd_pid" 2>/dev/null || true
    sleep 4
    "$CEPH" osd out 0 2>/dev/null

    info "[$label k=$k m=$m] Monitoring recovery..."
    local START PEAK_BW SAMPLES TOTAL_BW END_T
    START=$(date +%s); PEAK_BW=0; SAMPLES=0; TOTAL_BW=0; END_T=0

    printf "  %5s  %10s  %s\n" "t(s)" "degraded" "bw(MiB/s)"
    printf "  %5s  %10s  %s\n" "-----" "----------" "---------"

    for i in $(seq 1 200); do
        local status DEGRADED BW ELAPSED
        status=$("$CEPH" -s 2>/dev/null)
        DEGRADED=$(echo "$status" | grep -oP '\d+(?=/\d+ objects degraded)' | head -1)
        BW=$(echo "$status" | grep -oP '\d+(?= MiB/s)' | head -1)
        ELAPSED=$(( $(date +%s) - START ))
        [[ -z "$DEGRADED" ]] && DEGRADED=0
        [[ -z "$BW" ]] && BW=0
        if [[ "$BW" -gt 0 ]]; then
            TOTAL_BW=$((TOTAL_BW + BW)); SAMPLES=$((SAMPLES+1))
            [[ "$BW" -gt "$PEAK_BW" ]] && PEAK_BW=$BW
        fi
        printf "  %5d  %-10s  %d\n" "$ELAPSED" "$DEGRADED" "$BW"
        if ! echo "$status" | grep -qE "degraded|recovering|undersized|backfill"; then
            END_T=$(date +%s); break
        fi
        sleep 3
    done
    [[ "$END_T" -eq 0 ]] && END_T=$(date +%s)

    local TOTAL_TIME AVG_BW
    TOTAL_TIME=$(( END_T - START ))
    AVG_BW=0; [[ "$SAMPLES" -gt 0 ]] && AVG_BW=$(( TOTAL_BW / SAMPLES ))

    RESULTS+=("$label $k $m $TOTAL_TIME $PEAK_BW $AVG_BW")
    printf "  ${GREEN}→ time=%ds  peak=%d MiB/s  avg=%d MiB/s${NC}\n" \
        "$TOTAL_TIME" "$PEAK_BW" "$AVG_BW"

    # Restore osd.0
    info "[$label k=$k m=$m] Restoring osd.0..."
    "$CEPH_OSD" -i 0 --conf ./ceph.conf &>/dev/null &
    sleep 3
    "$CEPH" osd in 0 2>/dev/null
    wait_clean

    delete_pool "$pool"
    wait_clean
    sleep 3
}

# =============================================================================
# TEST MATRIX
# OSD count = k+m+1 minimum (need 1 spare slot for backfill after 1 failure)
# =============================================================================

# ─────────────────────────────────────────────────────────────────────────────
banner "k=3 m=2  (6 OSDs)"
start_cluster 6
for plugin_label in "twotone:twotone" "jerasure:jerasure-rs" "isa:isa-l"; do
    plugin=${plugin_label%%:*}; label=${plugin_label##*:}
    run_recovery_test "$plugin" 3 2 "$label"
done

# ─────────────────────────────────────────────────────────────────────────────
banner "k=4 m=2  (8 OSDs)"
start_cluster 8
for plugin_label in "twotone:twotone" "jerasure:jerasure-rs" "isa:isa-l"; do
    plugin=${plugin_label%%:*}; label=${plugin_label##*:}
    run_recovery_test "$plugin" 4 2 "$label"
done

# ─────────────────────────────────────────────────────────────────────────────
banner "k=6 m=3  (10 OSDs)"
start_cluster 10
for plugin_label in "twotone:twotone" "jerasure:jerasure-rs" "isa:isa-l"; do
    plugin=${plugin_label%%:*}; label=${plugin_label##*:}
    run_recovery_test "$plugin" 6 3 "$label"
done

# =============================================================================
banner "RECOVERY THROUGHPUT COMPARISON"
echo ""
printf "${BOLD}%-14s  %2s  %2s  %8s  %10s  %9s${NC}\n" \
    "plugin" "k" "m" "time(s)" "peak(MiB/s)" "avg(MiB/s)"
printf "%-14s  %2s  %2s  %8s  %10s  %9s\n" \
    "--------------" "--" "--" "--------" "-----------" "---------"

prev_k=0
for entry in "${RESULTS[@]}"; do
    read -r label k m time_s peak avg <<< "$entry"
    [[ "$k" != "$prev_k" && "$prev_k" != "0" ]] && echo ""
    prev_k=$k
    printf "%-14s  %2s  %2s  %8s  %10s  %9s\n" \
        "$label" "$k" "$m" "$time_s" "$peak" "$avg"
done
echo ""
