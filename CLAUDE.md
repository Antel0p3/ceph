# Ceph — TwoTone EC Branch (squid_tt)

This branch adds the **TwoTone erasure-code plugin** to the Ceph squid release.
TwoTone is a shift-based XOR code where parity shards are physically larger than
data shards (`parity_size = data_chunk_size + max_shift * 16 bytes`).

---

## Build

### First-time CMake configuration (from repo root)

```bash
mkdir build && cd build
cmake -GNinja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_TESTS=ON \
  ..
```

> The existing `build/` directory was configured with `-DCMAKE_BUILD_TYPE=Debug`.
> Use `RelWithDebInfo` or `Release` for performance work.

### Build only the TwoTone plugin + benchmark tool

```bash
cd build
ninja ec_twotone ceph_erasure_code_benchmark
```

### Build all erasure-code plugins

```bash
cd build
ninja erasure_code_plugins
```

### Build the TwoTone unit test

```bash
cd build
ninja unittest_erasure_code_twotone
```

---

## Unit Tests

### Run TwoTone unit tests

```bash
cd build
./bin/unittest_erasure_code_twotone
```

### Run with verbose output (show each test case)

```bash
cd build
./bin/unittest_erasure_code_twotone --gtest_verbose
```

### Run all erasure-code unit tests (ctest)

```bash
cd build
ctest -R erasure --output-on-failure
```

---

## Performance Benchmarks

All benchmark commands run from `build/`.  The benchmark binary loads plugins
from `./lib` — no cluster or `ninja install` required.

### Quick comparison: TwoTone vs ISA-L vs Jerasure

```bash
cd build
# Usage: bash ec_compare_new.sh [k] [m] [chunk_kb] [iters] [max_data_erasures]
# Defaults:                       3   2    2048       1000    1

# Default (k=3, m=2, 2048 KB chunks)
bash ec_compare_new.sh

# k=4, m=2
bash ec_compare_new.sh 4 2

# k=6, m=3
bash ec_compare_new.sh 6 3

# k=10, m=4
bash ec_compare_new.sh 10 4

# Include 2-data-chunk erasure cases (slow for TwoTone — O(n²) peeling decoder)
bash ec_compare_new.sh 3 2 2048 200 2
```

Output sections: **ENCODING**, **DECODING per erasure case**, **SUMMARY**.

### Run the low-level benchmark directly

```bash
cd build

# Encode: TwoTone k=3 m=2, 2 MB stripe, 1000 iterations
./bin/ceph_erasure_code_benchmark \
  --plugin twotone \
  --workload encode \
  --iterations 1000 \
  --size $((2048*3*1024)) \
  --erasure-code-dir ./lib \
  --parameter k=3 \
  --parameter m=2

# Decode with 1 data erasure: ISA-L for comparison
./bin/ceph_erasure_code_benchmark \
  --plugin isa \
  --workload decode \
  --iterations 1000 \
  --size $((2048*3*1024)) \
  --erasure-code-dir ./lib \
  --parameter k=3 \
  --parameter m=2 \
  --parameter technique=reed_sol_van \
  --erasures 1 \
  --erased 0
```

Output format: `<seconds>\t<total_KB>` — throughput = `total_KB / 1024 / seconds` MB/s.

### Benchmark across all k/m combinations

```bash
cd build
for km in "3 2" "4 2" "6 3" "10 4"; do
  bash ec_compare_new.sh $km
done
```

---

## Key Source Files

| File | Purpose |
|------|---------|
| `src/erasure-code/twotone/ErasureCodeTwotone.h` | Class declarations, `TwotoneLayout` struct |
| `src/erasure-code/twotone/ErasureCodeTwotone.cc` | Encode/decode implementation (AVX2 optimised) |
| `src/erasure-code/twotone/ErasureCodePluginTwotone.cc` | Plugin entry point |
| `src/erasure-code/twotone/CMakeLists.txt` | Build rules (compiles with `-O3 -march=native`) |
| `src/erasure-code/ErasureCodeInterface.h` | Added `get_parity_chunk_size()` and `supports_variable_parity_len()` |
| `src/erasure-code/ErasureCode.h` | Default `supports_variable_parity_len()` → `false` |
| `src/osd/ECUtil.cc` | Decode/encode updated to handle variable parity shard sizes |
| `src/osd/ECBackend.cc` | Recovery and rollback paths updated for per-shard chunk sizes |
| `src/test/erasure-code/TestErasureCodeTwotone.cc` | Unit tests |
| `build/ec_compare_new.sh` | Multi-plugin benchmark script |

---

## TwoTone Design Notes

- **Shift formula**: `twotone_shift(p, d, k, m)` — every parity `p` has exactly
  one `d*` with shift=0, used as the init seed in encode/decode.
- **Parity size**: `data_chunk_size + max_shift[p] * 16` bytes per parity shard.
- **Single data erasure**: O(chunk_size) tile-based decode, competitive with ISA-L.
- **Multiple data erasures**: O(cell_num²) iterative peeling decoder — avoid in
  benchmarks with `max_data_erasures=1` (default).
