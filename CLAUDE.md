# TwoTone EC — Ceph OSD Integration Notes

This file documents the changes made on branch `feat_ttec` to make the TwoTone
erasure-code plugin work end-to-end with `rados put / rados get / rados bench`,
and the methodology for benchmarking EC recovery throughput.

---

## 1. Root Cause: Variable-Length Parity Shards

TwoTone is a shift-based XOR erasure code. Its parity shards are physically
**larger** than data shards for certain k/m combinations:

```
parity_chunk_size(p) = data_chunk_size + layout.max_shift[p] * BYTE_PERCELL
```

| k  | m | max_shift      | extra bytes/stripe per parity      |
|----|---|----------------|------------------------------------|
| 2  | 2 | [1, 0]         | p0: +16 B,  p1: 0                  |
| 3  | 2 | [2, 0]         | p0: +32 B,  p1: 0                  |
| 4  | 2 | [3, 0]         | p0: +48 B,  p1: 0                  |
| 6  | 3 | [5, 0, 5]      | p0: +80 B,  p1: 0,  p2: +80 B     |
| 10 | 4 | [18, 9, 0, 9]  | p0: +288 B, p1: +144 B, p2: 0, p3: +144 B |

**Rule:** there is always exactly one "center" parity with `max_shift=0`
(p = m/2 rounded down, 0-indexed among parities); the remaining parities are
larger by `max_shift[p] * 16` bytes per stripe.  The center parity enables the
zero-shift fast path in decode (no boundary clipping needed).

The OSD layer (`stripe_info_t`, `ECUtil`, `ECBackend`) assumed **all shards have
the same size** (`stripe_width / k`). This caused assertion failures and wrong
slicing whenever a parity shard was touched.

---

## 2. Module Flow & Compatibility Fixes

### Write path (rados put / bench write)

```
Client
  │
  ▼
PrimaryOSD::do_osd_ops()
  │  logical object → EC stripes
  ▼
ECBackend::submit_transaction()
  │
  ▼
ECUtil::encode()                          ← FIX A
  │  calls ec_impl->encode(want, buf, &encoded)
  │  asserts each shard length == expected size
  │
  │  BEFORE: ceph_assert(bl.length() == sinfo.get_chunk_size())
  │            ↳ CRASH for parity shards (larger than data shards)
  │
  │  AFTER:  uint64_t expected = (id < k)
  │            ? sinfo.get_chunk_size()
  │            : ec_impl->get_parity_chunk_size(stripe_width, id - k);
  │          ceph_assert(bl.length() == expected);          ✓
  │
  ▼
HashInfo::append()                        ← FIX B
  │  accumulates per-shard CRC hashes
  │
  │  BEFORE: ceph_assert(size_to_append == i->second.length())
  │            ↳ CRASH: parity shards have different length
  │
  │  AFTER:  assert removed; each shard hashed at its own length
  │          total_chunk_size tracks shard-0 (data) bytes only    ✓
  │
  ▼
Written to OSD store (one file per shard per OSD)
```

### Read path (rados get / bench read)

```
PrimaryOSD
  │  reads k shards from OSDs (may include parity shards)
  ▼
ECUtil::decode() [overload 1: bufferlist* out]    ← FIX C
  │
  │  BEFORE: iterated with fixed step sinfo.get_chunk_size()
  │            ↳ WRONG slice for parity shards (step too small)
  │
  │  AFTER:  n_stripes derived from first DATA shard;
  │          per-shard step = (id < k) ? data_chunk_size
  │                                    : get_parity_chunk_size(…)  ✓
  │
  ▼
ec_impl->decode_concat(chunks, &bl)
  │  reconstructs stripe_width bytes
  ▼
Client receives correct data
```

### Recovery path (OSD failure → reconstruct missing shard)

```
PrimaryOSD detects shard missing
  │
  ▼
ECBackend::start_recovery_op()
  │  reads surviving shards via ECUtil::decode() [overload 2]
  ▼
ECUtil::decode() [overload 2: map<int,bufferlist*> out]   ← FIX D
  │
  │  BEFORE: chunks_count from first shard (could be parity)
  │          slicing used uniform repair_data_per_chunk for ALL shards
  │          output assert: out_bls[id].length() == sinfo.get_chunk_size()
  │            ↳ CRASH when recovering a parity shard
  │
  │  AFTER:  chunks_count from first DATA shard in min{};
  │          parity shards sliced with get_parity_chunk_size()
  │          output assert uses per-shard expected size             ✓
  │
  ▼
ECBackend::handle_recovery_read_complete()
  │  builds PushOp to send reconstructed shard to target OSD
  │
  │  BEFORE: pop.data.length() checked against
  │            aligned_logical_offset_to_chunk_offset(delta)
  │            = n_stripes * data_chunk_size   ← wrong for parity shards
  │          pop.data_included interval also used data_chunk_size
  │            ↳ CRASH (assert) for parity OSD targets
  │
  │  AFTER:  chunk_size_for_shard = (shard_id < k)
  │            ? sinfo.get_chunk_size()
  │            : ec_impl->get_parity_chunk_size(stripe_width, shard_id-k)
  │          assert and interval both use chunk_size_for_shard       ✓
  │
  ▼
ECBackend::handle_sub_read_reply() [attrs retry]         ← FIX E
  │
  │  SCENARIO: shard chosen to provide attrs has hash_info mismatch
  │            → handle_sub_read sets EIO, skips attrs in reply
  │            → errors ignored ("enough copies available")
  │            → attrs never populated → op.xattrs empty
  │            → ceph_assert(op.xattrs.size()) CRASH
  │
  │  AFTER:  when errors ignored but attrs still nullopt,
  │          call send_all_remaining_reads() to retry attrs
  │          from another surviving shard;
  │          if no shards left, set r=-EIO → _failed_push()          ✓
  │
  ▼
ECBackend::rollback_append()                             ← FIX F
  │  truncates shard file on failed append
  │
  │  BEFORE: truncate_at = aligned_logical_offset_to_chunk_offset(old_size)
  │            = n_stripes * data_chunk_size   ← wrong for parity OSD
  │
  │  AFTER:  truncate_at uses get_parity_chunk_size() for parity shards ✓
  │
  ▼
Recovered shard pushed to target OSD; PG becomes active+clean
```

---

## 3. Changed Files Summary

| File | Change |
|------|--------|
| `src/erasure-code/ErasureCodeInterface.h` | Add `get_parity_chunk_size(object_size, parity_idx=0)` virtual method; default returns `get_chunk_size()` — all existing plugins unaffected |
| `src/erasure-code/twotone/ErasureCodeTwotone.h` | Declare override |
| `src/erasure-code/twotone/ErasureCodeTwotone.cc` | Implement: `get_chunk_size(object_size) + layout.max_shift[parity_idx] * BYTE_PERCELL` |
| `src/osd/ECUtil.cc` | Fix both `decode()` overloads (per-shard slicing); fix `encode()` assertions; fix `HashInfo::append()` |
| `src/osd/ECBackend.cc` | Fix recovery push size+interval; fix `rollback_append()` truncation; fix attrs retry on error-ignored reads |

---

## 4. EC Recovery Throughput Test

### Why recovery matters more than write bench for EC comparison

`rados bench write` is disk-I/O bound; EC algorithm time is <1% of latency
(Amdahl's Law). Recovery throughput exposes both **I/O bandwidth** (how much
data is read from surviving shards) and **CPU reconstruction time** (XOR vs GF
multiply), making it a more discriminating EC comparison.

### Test procedure

```bash
cd /root/ceph/build

# 1. Clean start with BlueStore (lower noise than FileStore)
../src/stop.sh; rm -rf out/ dev/
MDS=0 MON=1 MGR=1 OSD=6 ../src/vstart.sh -n -x --without-dashboard --bluestore

# 2. Create pool (repeat for each plugin to test)
PLUGIN=twotone   # or jerasure, isa, clay
./bin/ceph osd erasure-code-profile set ${PLUGIN}-profile \
    plugin=$PLUGIN k=3 m=2 crush-failure-domain=osd \
    "directory=$PWD/lib" \
    technique=reed_sol_van   # jerasure/isa only; omit for twotone/clay
    --force
./bin/ceph osd pool create ${PLUGIN}-pool 64 64 erasure ${PLUGIN}-profile
./bin/ceph osd pool set ${PLUGIN}-pool min_size 3
./bin/ceph osd pool application enable ${PLUGIN}-pool rados

# 3. Remove recovery throttling
./bin/ceph tell 'osd.*' injectargs \
    '--osd-recovery-max-active-hdd 10 --osd-max-backfills 10 \
     --osd-recovery-sleep-hdd 0 --osd-recovery-max-active-ssd 10 \
     --osd-recovery-sleep-ssd 0'

# 4. Write test data (keep objects: --no-cleanup)
./bin/rados bench -p ${PLUGIN}-pool 40 write --no-cleanup --object-size 4M

# 5. Simulate OSD failure:
#    - kill the process  → OSD goes "down" (cluster waits for it to return)
#    - mark it "out"     → cluster remaps PGs, triggers actual backfill
kill $(cat out/osd.0.pid)
sleep 5
./bin/ceph osd out 0    # <-- this is the trigger for recovery

# 6. Monitor recovery bandwidth
START=$(date +%s)
PEAK=0; SAMPLES=0; TOTAL=0
while true; do
    S=$(./bin/ceph -s 2>/dev/null)
    BW=$(echo "$S" | grep -oP '\d+(?= MiB/s)' | head -1)
    DEG=$(echo "$S" | grep -oP '\d+(?=/\d+ objects degraded)' | head -1)
    ELAPSED=$(( $(date +%s) - START ))
    [[ -z "$BW"  ]] && BW=0
    [[ -z "$DEG" ]] && DEG=0
    [[ $BW -gt 0 ]] && { TOTAL=$((TOTAL+BW)); SAMPLES=$((SAMPLES+1)); }
    [[ $BW -gt $PEAK ]] && PEAK=$BW
    printf "t=%3ds  degraded=%-6s  bw=%s MiB/s\n" "$ELAPSED" "$DEG" "$BW"
    echo "$S" | grep -qE "degraded|recovering|undersized|backfill" || break
    sleep 3
done
END=$(( $(date +%s) - START ))
AVG=0; [[ $SAMPLES -gt 0 ]] && AVG=$((TOTAL/SAMPLES))
echo "=== $PLUGIN: time=${END}s  peak=${PEAK} MiB/s  avg=${AVG} MiB/s ==="
```

### Results (k=3 m=2, 4MB objects, ~17 GiB data, BlueStore, single host)

| Plugin | Recovery time | Peak BW | Avg BW |
|--------|--------------|---------|--------|
| twotone | **59 s** | 276 MiB/s | **200 MiB/s** |
| jerasure (RS-van) | 83 s | 203 MiB/s | 153 MiB/s |

TwoTone ~31% faster recovery due to cheaper XOR reconstruction vs GF multiply.
For k=3 m=2 both codes read the same number of bytes from surviving shards
(repair bandwidth is identical); the difference is purely CPU reconstruction time.

> **Note:** For CLAY vs RS comparison the dominant factor is *repair bandwidth*
> (CLAY reads fewer bytes per repair), not CPU. Use the same script with
> `plugin=clay` and `plugin=jerasure` to compare.

---

## 5. Key Invariants Preserved

- `stripe_info_t::chunk_size` still equals `stripe_width / k` (data shard size).
  All helpers (`logical_to_chunk_offset`, etc.) remain correct for data shards.
- `HashInfo::total_chunk_size` tracks **data-shard bytes only**; `get_total_logical_size() = total_chunk_size * k` stays valid.
- Default `get_parity_chunk_size()` returns `get_chunk_size()` → Jerasure / ISA-L / LRC / CLAY are completely unaffected.
