#!/usr/bin/env bash
# twotone_robustness.sh — write / read / recovery test for TwoTone EC
# Run from /root/ceph/build
set -uo pipefail

BUILD=$(cd "$(dirname "$0")" && pwd)
cd "$BUILD"

CEPH="./bin/ceph"
RADOS="./bin/rados"
CEPH_OSD="./bin/ceph-osd"
PASS=0; FAIL=0

# ── colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
ok()     { echo -e "  ${GREEN}PASS${NC}  $*"; PASS=$((PASS+1)); }
fail()   { echo -e "  ${RED}FAIL${NC}  $*"; FAIL=$((FAIL+1)); }
info()   { echo -e "${CYAN}>>> $*${NC}"; }
banner() { echo -e "\n${BOLD}${YELLOW}═══ $* ═══${NC}"; }

# ── cluster lifecycle ─────────────────────────────────────────────────────────
start_cluster() {
    local n_osd=$1
    info "Starting cluster with $n_osd OSDs (bluestore)..."
    ../src/stop.sh 2>/dev/null || true
    rm -rf out/ dev/
    MDS=0 MON=1 MGR=1 OSD=$n_osd \
        ../src/vstart.sh -n -x --without-dashboard --bluestore 2>/dev/null
    # wait for all OSDs up
    for i in $(seq 1 30); do
        local up
        up=$("$CEPH" -s 2>/dev/null | grep -oP '\d+(?= up)' | head -1)
        [[ "$up" == "$n_osd" ]] && break
        sleep 2
    done
    "$CEPH" tell 'osd.*' injectargs \
        '--osd-recovery-max-active-hdd 10 --osd-max-backfills 10 --osd-recovery-sleep-hdd 0 --osd-recovery-max-active-ssd 10 --osd-recovery-sleep-ssd 0' \
        2>/dev/null || true
    info "Cluster ready — $("$CEPH" -s 2>/dev/null | grep 'osd:')"
}

make_pool() {
    local pool=$1 k=$2 m=$3
    "$CEPH" osd erasure-code-profile set twotone-p${k}m${m} \
        plugin=twotone k=$k m=$m crush-failure-domain=osd \
        "directory=$BUILD/lib" --force 2>/dev/null
    "$CEPH" osd pool create "$pool" 32 32 erasure twotone-p${k}m${m} 2>/dev/null
    "$CEPH" osd pool set "$pool" min_size $k 2>/dev/null
    "$CEPH" osd pool application enable "$pool" rados 2>/dev/null
    # wait for active+clean
    for i in $(seq 1 20); do
        local active
        active=$("$CEPH" -s 2>/dev/null | grep -oP '\d+(?= active\+clean)' | head -1)
        [[ -n "$active" && "$active" -ge 32 ]] && return 0
        sleep 2
    done
}

# ── integrity helpers ─────────────────────────────────────────────────────────
make_testfile() {
    local path=$1 size=$2
    dd if=/dev/urandom of="$path" bs="$size" count=1 2>/dev/null
    sha256sum "$path" | awk '{print $1}'
}

# ── write / read integrity test ───────────────────────────────────────────────
test_write_read() {
    local pool=$1 obj=$2 size_label=$3 size_bytes=$4
    local src="/tmp/tt_src_${obj}" dst="/tmp/tt_dst_${obj}"
    local ref_hash actual_hash

    ref_hash=$(make_testfile "$src" "$size_bytes")
    if "$RADOS" -p "$pool" put "$obj" "$src" 2>/dev/null; then
        if "$RADOS" -p "$pool" get "$obj" "$dst" 2>/dev/null; then
            actual_hash=$(sha256sum "$dst" | awk '{print $1}')
            if [[ "$ref_hash" == "$actual_hash" ]]; then
                ok "write+read  pool=$pool  size=$size_label"
            else
                fail "write+read  pool=$pool  size=$size_label  HASH MISMATCH"
            fi
        else
            fail "write+read  pool=$pool  size=$size_label  GET FAILED"
        fi
    else
        fail "write+read  pool=$pool  size=$size_label  PUT FAILED"
    fi
    rm -f "$src" "$dst"
}

# ── bench smoke test ──────────────────────────────────────────────────────────
test_bench() {
    local pool=$1 obj_size=$2 label=$3
    local write_bw read_bw
    write_bw=$("$RADOS" bench -p "$pool" 10 write \
        --no-cleanup --object-size "$obj_size" 2>/dev/null | \
        grep "Bandwidth (MB/sec)" | awk '{print $NF}')
    read_bw=$("$RADOS" bench -p "$pool" 10 seq 2>/dev/null | \
        grep "Bandwidth (MB/sec)" | awk '{print $NF}')
    if [[ -n "$write_bw" && $(echo "$write_bw > 0" | bc -l 2>/dev/null) -eq 1 ]]; then
        ok "bench  pool=$pool  size=$label  write=${write_bw} MB/s  read=${read_bw:-n/a} MB/s"
    else
        fail "bench  pool=$pool  size=$label  write bandwidth=0 or error"
    fi
    "$RADOS" -p "$pool" cleanup 2>/dev/null || true
}

# ── recovery test ─────────────────────────────────────────────────────────────
test_recovery() {
    local pool=$1 k=$2 m=$3
    local src="/tmp/tt_rec_src" dst="/tmp/tt_rec_dst"
    local ref_hash actual_hash

    ref_hash=$(make_testfile "$src" $((4*1024*1024)))
    if ! "$RADOS" -p "$pool" put rec_obj "$src" 2>/dev/null; then
        fail "recovery  pool=$pool  PUT before kill failed"
        rm -f "$src"; return
    fi

    # Kill osd.0 and mark it out to trigger backfill
    local osd_pid
    osd_pid=$(cat out/osd.0.pid 2>/dev/null || echo "")
    if [[ -z "$osd_pid" ]]; then
        fail "recovery  pool=$pool  osd.0 pid not found"
        rm -f "$src"; return
    fi
    kill "$osd_pid" 2>/dev/null || true
    sleep 4
    "$CEPH" osd out 0 2>/dev/null

    # Wait for recovery to complete (up to 3 min)
    local recovered=0
    for i in $(seq 1 90); do
        local status
        status=$("$CEPH" -s 2>/dev/null)
        if ! echo "$status" | grep -qE "degraded|recovering|undersized|backfill"; then
            recovered=1; break
        fi
        sleep 2
    done

    if [[ $recovered -eq 0 ]]; then
        fail "recovery  pool=$pool  timed out"
        rm -f "$src"; return
    fi

    # Read back and verify integrity
    if "$RADOS" -p "$pool" get rec_obj "$dst" 2>/dev/null; then
        actual_hash=$(sha256sum "$dst" | awk '{print $1}')
        if [[ "$ref_hash" == "$actual_hash" ]]; then
            ok "recovery   pool=$pool  k=$k m=$m  data intact after osd.0 failure"
        else
            fail "recovery   pool=$pool  k=$k m=$m  HASH MISMATCH after recovery"
        fi
    else
        fail "recovery   pool=$pool  k=$k m=$m  GET after recovery FAILED"
    fi

    # Bring osd.0 back
    "$CEPH_OSD" -i 0 --conf ./ceph.conf &>/dev/null &
    sleep 3
    "$CEPH" osd in 0 2>/dev/null
    rm -f "$src" "$dst"
}

# =============================================================================
# MAIN TEST MATRIX
# =============================================================================

declare -A SIZES=(
    [4K]=$((4*1024))
    [64K]=$((64*1024))
    [1M]=$((1024*1024))
    [4M]=$((4*1024*1024))
)

# Recovery requires at least k+m+1 OSDs (1 spare slot for remapping after failure):
#   k=2,m=2 → k+m=4 → 6 OSDs ✓
#   k=3,m=2 → k+m=5 → 6 OSDs ✓
#   k=4,m=2 → k+m=6 → 8 OSDs  (6 would leave no spare after 1 failure)
#   k=6,m=3 → k+m=9 → 10 OSDs (9 would leave no spare after 1 failure)

# ─────────────────────────────────────────────────────────────────────────────
banner "PHASE 1a: k=2,m=2  k=3,m=2  (6 OSDs)"
start_cluster 6

for km in "2 2" "3 2"; do
    k=${km% *}; m=${km#* }
    POOL="tt-k${k}m${m}"
    info "Creating pool $POOL (k=$k m=$m)"
    make_pool "$POOL" "$k" "$m"

    for label in 4K 64K 1M 4M; do
        test_write_read "$POOL" "obj_${label}" "$label" "${SIZES[$label]}"
    done

    test_bench "$POOL" $((4*1024*1024)) "4M"
    test_recovery "$POOL" "$k" "$m"
    sleep 5
done

# ─────────────────────────────────────────────────────────────────────────────
banner "PHASE 1b: k=4,m=2  (8 OSDs — needs k+m+1=7 for recovery)"
start_cluster 8

for km in "4 2"; do
    k=${km% *}; m=${km#* }
    POOL="tt-k${k}m${m}"
    info "Creating pool $POOL (k=$k m=$m)"
    make_pool "$POOL" "$k" "$m"

    for label in 4K 64K 1M 4M; do
        test_write_read "$POOL" "obj_${label}" "$label" "${SIZES[$label]}"
    done

    test_bench "$POOL" $((4*1024*1024)) "4M"
    test_recovery "$POOL" "$k" "$m"
    sleep 5
done

# ─────────────────────────────────────────────────────────────────────────────
banner "PHASE 2: k=6,m=3  (10 OSDs — needs k+m+1=10 for recovery)"
start_cluster 10

POOL="tt-k6m3"
info "Creating pool $POOL (k=6 m=3)"
make_pool "$POOL" 6 3

for label in 4K 64K 1M 4M; do
    test_write_read "$POOL" "obj_${label}" "$label" "${SIZES[$label]}"
done

test_bench "$POOL" $((4*1024*1024)) "4M"
test_recovery "$POOL" 6 3

# =============================================================================
banner "RESULTS"
echo ""
echo -e "  ${GREEN}PASS${NC}: $PASS"
echo -e "  ${RED}FAIL${NC}: $FAIL"
echo ""
[[ $FAIL -eq 0 ]] && echo -e "${GREEN}All tests passed.${NC}" || echo -e "${RED}$FAIL test(s) FAILED.${NC}"
exit $FAIL
