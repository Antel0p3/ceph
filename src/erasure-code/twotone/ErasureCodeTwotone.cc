#include "common/debug.h"
#include "ErasureCodeTwotone.h"

#define LARGEST_VECTOR_WORDSIZE 16

// ---------------------------------------------------------------------------
// xor_buf: XOR src into dst for `bytes` bytes.
// Dispatches to the widest SIMD available at compile time, with a uint64_t
// fallback that GCC/Clang reliably auto-vectorize under -O3.
// All callers guarantee bytes is a multiple of 16 (BYTE_PERCELL).
// ---------------------------------------------------------------------------
#if defined(__AVX2__)
#include <immintrin.h>
static inline void xor_buf(void* __restrict__ dst,
                            const void* __restrict__ src, size_t bytes)
{
    char *d = (char*)dst;  const char *s = (const char*)src;
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32)
        _mm256_storeu_si256((__m256i*)(d+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d+i)),
                             _mm256_loadu_si256((const __m256i*)(s+i))));
    for (; i + 16 <= bytes; i += 16)
        _mm_storeu_si128((__m128i*)(d+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d+i)),
                          _mm_loadu_si128((const __m128i*)(s+i))));
}
#elif defined(__SSE2__)
#include <emmintrin.h>
static inline void xor_buf(void* __restrict__ dst,
                            const void* __restrict__ src, size_t bytes)
{
    char *d = (char*)dst;  const char *s = (const char*)src;
    for (size_t i = 0; i < bytes; i += 16)
        _mm_storeu_si128((__m128i*)(d+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d+i)),
                          _mm_loadu_si128((const __m128i*)(s+i))));
}
#else
static inline void xor_buf(void* __restrict__ dst,
                            const void* __restrict__ src, size_t bytes)
{
    uint64_t *d = (uint64_t*)dst;  const uint64_t *s = (const uint64_t*)src;
    for (size_t i = 0, n = bytes / 8; i < n; ++i) d[i] ^= s[i];
}
#endif

// ---------------------------------------------------------------------------
// xor_buf3: XOR `src` into three destinations in a single pass.
// `src` is loaded once per 16/32 bytes and reused for all three stores,
// eliminating 2/3 of the data-register traffic vs three separate xor_buf calls.
// Used by the k=6,m=3 specialized encode/reconstruct path.
// ---------------------------------------------------------------------------
#if defined(__AVX2__)
static inline void xor_buf3(char* d0, char* d1, char* d2,
                             const char* src, size_t bytes)
{
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src+i));
        _mm256_storeu_si256((__m256i*)(d0+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d0+i)), v));
        _mm256_storeu_si256((__m256i*)(d1+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d1+i)), v));
        _mm256_storeu_si256((__m256i*)(d2+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d2+i)), v));
    }
    // 16-byte tail: bytes is always a multiple of BYTE_PERCELL (16) but may not be 32-aligned
    for (; i + 16 <= bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src+i));
        _mm_storeu_si128((__m128i*)(d0+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d0+i)), v));
        _mm_storeu_si128((__m128i*)(d1+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d1+i)), v));
        _mm_storeu_si128((__m128i*)(d2+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d2+i)), v));
    }
}
#elif defined(__SSE2__)
static inline void xor_buf3(char* d0, char* d1, char* d2,
                             const char* src, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src+i));
        _mm_storeu_si128((__m128i*)(d0+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d0+i)), v));
        _mm_storeu_si128((__m128i*)(d1+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d1+i)), v));
        _mm_storeu_si128((__m128i*)(d2+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d2+i)), v));
    }
}
#else
static inline void xor_buf3(char* d0, char* d1, char* d2,
                             const char* src, size_t bytes)
{
    xor_buf(d0, src, bytes);
    xor_buf(d1, src, bytes);
    xor_buf(d2, src, bytes);
}
#endif

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define UPTO_K(num, K) (((num + (K) - 1LL) / (K)) * (K))
#define CELLSIZE (128LL)
#define BYTESIZE (8)
#define BYTE_PERCELL (CELLSIZE / BYTESIZE)

using std::ostream;
using std::map;
using std::set;

using ceph::bufferlist;
using ceph::ErasureCodeProfile;

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeTwotone: ";
}

bool ErasureCodeTwotone::supports_variable_parity_len() const { return true; }

int ErasureCodeTwotone::init(ErasureCodeProfile& profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  if (m > k) {
    err |= -EINVAL;
    *ss << "m must be <= k for decoding correctness" << std::endl;
  }
  if (err)
    return err;
  prepare();
  return ErasureCode::init(profile, ss);
}

int ErasureCodeTwotone::parse(ErasureCodeProfile &profile,
			       ostream *ss)
{
  int err = ErasureCode::parse(profile, ss);
  err |= to_int("k", profile, &k, DEFAULT_K, ss);
  err |= to_int("m", profile, &m, DEFAULT_M, ss);
  // err |= to_int("w", profile, &w, DEFAULT_W, ss);
  dout(10) << "k set to " << k << "m set to " << m << dendl;
  
  err |= sanity_check_k_m(k, m, ss);
  return err;
}

unsigned int ErasureCodeTwotone::get_chunk_size(unsigned int object_size) const
{
  // unsigned alignment = get_alignment();
  unsigned cell_num = UPTO_K(object_size * BYTESIZE, CELLSIZE) / CELLSIZE;  // round obj size to the multiple of 16bytes (each line)
  unsigned all_cell_num = UPTO_K(cell_num, k);     // round cell_num to be the multiple of k so all k chunks have the same size
  unsigned padded_length = all_cell_num * BYTE_PERCELL;
  ceph_assert(padded_length % k == 0);
  return padded_length / k;    // bits per chunk
}

unsigned int ErasureCodeTwotone::get_parity_chunk_size(unsigned int object_size,
                                                        int parity_idx) const
{
  ceph_assert(parity_idx >= 0 && parity_idx < m);
  return get_chunk_size(object_size) + layout.max_shift[parity_idx] * BYTE_PERCELL;
}

static size_t twotone_shift(int p, int d, int k, int m) {
    int dd = (m % 2 == 1) ? (m + 1) / 2 : (m / 2);
    int pp = m - 1 - p;
    if (pp < dd) {
        return (dd - 1 - pp) * (k - 1 - d);
    } else {
        return (pp - dd + 1) * d;
    }
}

static TwotoneLayout build_layout(int k, int m) {
  TwotoneLayout L;
  L.max_shift.resize(m, 0);
  L.shift.resize(k * m);

  for (int p = 0; p < m; ++p) {
    for (int d = 0; d < k; ++d) {
      size_t s = twotone_shift(p, d, k, m);
      L.shift[p * k + d] = s;
      L.max_shift[p] = std::max(L.max_shift[p], s);
    }
  }
  return L;
}

int ErasureCodeTwotone::encode_chunks(const set<int> &want_to_encode,
                                      map<int, bufferlist> *encoded)
{
  size_t base_block_size = (*encoded)[0].length();
  size_t cell_num  = base_block_size / BYTE_PERCELL;

  char* chunks[k + m];
  for (int i = 0; i < k; ++i)
    chunks[i] = (*encoded)[i].c_str();

  for (int p = 0; p < m; ++p) {
    size_t cells = cell_num + layout.max_shift[p];
    size_t bytes = cells * BYTE_PERCELL;

    (*encoded)[k + p].clear();
    (*encoded)[k + p].push_back(buffer::create_aligned(bytes, SIMD_ALIGN));

    chunks[k + p] = (*encoded)[k + p].c_str();
  }

  twotone_encode(chunks, chunks + k, base_block_size);
  return 0;
}

int ErasureCodeTwotone::decode_chunks(const set<int> &want_to_read,
				       const map<int, bufferlist> &chunks,
				       map<int, bufferlist> *decoded)
{
  int erasures[k + m + 1];
  int erasures_count = 0;

  char* data[k];
  char* coding[m];

  /* -------------------------------------------------
   * Determine correct data blocksize = smallest data shard size present
   * ------------------------------------------------- */
  unsigned blocksize = UINT_MAX;
  for (int i = 0; i < k+m; ++i) {
    auto it = chunks.find(i);
    if (it != chunks.end()) {
      blocksize = (blocksize < it->second.length()) ? blocksize : it->second.length();
    }
  }

  ceph_assert(blocksize != UINT_MAX);
  // printf("[decode] blocksize: %d\n", blocksize);


  /* -------------------------------------------------
   *  Prepare data + coding pointers and erasures.
   *
   *  _decode() has already built decoded[i] for every chunk:
   *    present chunks  → aligned reference/copy of the input (data ready)
   *    missing chunks  → create_aligned(blocksize) uninitialized buffer
   *
   *  We reuse those buffers directly for present chunks (no copy needed).
   *  Missing data chunks just need zeroing.
   *  Missing parity chunks need a fresh parity-sized allocation because
   *  _decode() only allocated blocksize bytes (may be smaller than parity_size).
   * ------------------------------------------------- */
  for (int i = 0; i < k + m; ++i) {
    bool missing = (chunks.find(i) == chunks.end());
    if (missing) erasures[erasures_count++] = i;

    if (i < k) {
      data[i] = (*decoded)[i].c_str();   // _decode() already set this up
      if (missing) memset(data[i], 0, blocksize);
    } else {
      int p = i - k;
      if (missing) {
        unsigned parity_size =
            (blocksize / BYTE_PERCELL + layout.max_shift[p]) * BYTE_PERCELL;
        (*decoded)[i].clear();
        (*decoded)[i].push_back(buffer::create_aligned(parity_size, SIMD_ALIGN));
        coding[p] = (*decoded)[i].c_str();
        memset(coding[p], 0, parity_size);
      } else {
        coding[p] = (*decoded)[i].c_str();   // aligned ref from _decode(), no copy
      }
    }
  }

  erasures[erasures_count] = -1;

  ceph_assert(erasures_count > 0);

  int res = twotone_decode(erasures, data, coding, blocksize);
  // printf("\n\n[decode_chunks] after decoding:::\n\n");
  // for (int i = 0; i < k + m; ++i) {
  //     printf("%d: length=%u\n", i, (*decoded)[i].length());
  //     for (unsigned j = 0; j < (*decoded)[i].length(); ++j) {
  //         printf("%02x ", (unsigned char)(*decoded)[i].c_str()[j]);
  //         if ((j+1) % 16 == 0) {
  //           printf("\n");
  //         }
  //     }
  //     printf("\n\n");
  // }
  return res;
}


void ErasureCodeTwotoneImpl::twotone_encode(char **data, char **coding, int blocksize)
{
  const size_t bytes = (size_t)blocksize;
  constexpr size_t TILE_BYTES = 4096;

  // ── Optimisation 1: init each parity from the data chunk with shift=0 ────
  //
  // twotone_shift() guarantees that for every parity p there exists at least
  // one data chunk d* where shift[p][d*] = 0 (data and parity are aligned).
  // Copying data[d*] directly into coding[p] is equivalent to
  //   memset(coding[p], 0, ...) followed by xor(coding[p], data[d*], ...)
  // but does it in a single pass, eliminating a full chunk-sized zero-write.
  // Only the small tail extension (max_shift[p] * BYTE_PERCELL bytes) needs
  // explicit zeroing afterwards.
  //
  // twotone_shift() cases:
  //   pp >= dd  →  shift[p][d] = (pp-dd+1)*d  →  d=0 gives shift=0
  //   pp <  dd  →  shift[p][d] = (dd-1-pp)*(k-1-d)  →  d=k-1 gives shift=0
  int init_d[m]; // init_d[p]: which data chunk initialised coding[p]
  for (int p = 0; p < m; ++p) {
    init_d[p] = -1;
    for (int d = 0; d < k; ++d) {
      if (layout.shift[p * k + d] == 0) { init_d[p] = d; break; }
    }
    ceph_assert(init_d[p] >= 0);
    memcpy(coding[p], data[init_d[p]], bytes);
    if (layout.max_shift[p] > 0)
      memset(coding[p] + bytes, 0, layout.max_shift[p] * BYTE_PERCELL);
  }

  // ── Optimisation 2: fused tile XOR — load data[d] once, XOR all parities ─
  //
  // For each data chunk d, only the parities NOT initialised from d need an
  // XOR contribution.  Loading the data tile into a SIMD register once and
  // writing it to all npp active parities reduces data-register traffic by
  // up to m× versus m separate xor_buf calls.
  for (int d = 0; d < k; ++d) {
    // Collect base pointers (already shifted) of parities that need data[d].
    char* pp[m];
    int npp = 0;
    for (int p = 0; p < m; ++p) {
      if (init_d[p] != d)
        pp[npp++] = coding[p] + layout.shift[p * k + d] * BYTE_PERCELL;
    }
    if (npp == 0) continue;

    const char* src = data[d];
    for (size_t tile_off = 0; tile_off < bytes; tile_off += TILE_BYTES) {
      const size_t tlen = std::min(TILE_BYTES, bytes - tile_off);
#if defined(__AVX2__)
      size_t i = 0;
      // 4-way unrolled: 4 independent accumulators keep the load/XOR/store
      // pipeline full and reduce loop-overhead by 4×.
      for (; i + 128 <= tlen; i += 128) {
        __m256i v0 = _mm256_loadu_si256((const __m256i*)(src + tile_off + i +  0));
        __m256i v1 = _mm256_loadu_si256((const __m256i*)(src + tile_off + i + 32));
        __m256i v2 = _mm256_loadu_si256((const __m256i*)(src + tile_off + i + 64));
        __m256i v3 = _mm256_loadu_si256((const __m256i*)(src + tile_off + i + 96));
        for (int j = 0; j < npp; ++j) {
          char* dst = pp[j] + tile_off + i;
          _mm256_storeu_si256((__m256i*)(dst +  0), _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(dst +  0)), v0));
          _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(dst + 32)), v1));
          _mm256_storeu_si256((__m256i*)(dst + 64), _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(dst + 64)), v2));
          _mm256_storeu_si256((__m256i*)(dst + 96), _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(dst + 96)), v3));
        }
      }
      for (; i + 32 <= tlen; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src + tile_off + i));
        for (int j = 0; j < npp; ++j) {
          char* dst = pp[j] + tile_off + i;
          _mm256_storeu_si256((__m256i*)dst,
              _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)dst), v));
        }
      }
      for (; i + 16 <= tlen; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src + tile_off + i));
        for (int j = 0; j < npp; ++j) {
          char* dst = pp[j] + tile_off + i;
          _mm_storeu_si128((__m128i*)dst,
              _mm_xor_si128(_mm_loadu_si128((const __m128i*)dst), v));
        }
      }
#else
      for (int j = 0; j < npp; ++j)
        xor_buf(pp[j] + tile_off, src + tile_off, tlen);
#endif
    }
  }
}

// Reconstruct missing parity chunks.
// Uses init-from-d_zero (same trick as twotone_encode) to avoid a full
// memset pass: the data chunk whose shift is 0 is memcpy'd as the seed,
// then the remaining chunks are XOR'd in — saves one chunk-sized write.
static void reconstruct_parity(int k, int m,
                                const TwotoneLayout &layout,
                                size_t chunk_bytes,
                                char **D, char **P,
                                const bool *missing_chunk)
{
    for (int p = 0; p < m; ++p) {
        if (!missing_chunk[k + p]) continue;
        const size_t parity_bytes __attribute__((unused)) = chunk_bytes + layout.max_shift[p] * BYTE_PERCELL;
        char* parity = P[p];

        // Find d* where shift[p][d*] == 0 (guaranteed by twotone_shift()).
        int d_zero = -1;
        for (int d = 0; d < k; ++d) {
            if (layout.shift[p * k + d] == 0) { d_zero = d; break; }
        }
        // Init from d_zero (equiv. to memset-0 + xor, but in one pass).
        memcpy(parity, D[d_zero], chunk_bytes);
        if (layout.max_shift[p] > 0)
            memset(parity + chunk_bytes, 0, layout.max_shift[p] * BYTE_PERCELL);

        for (int d = 0; d < k; ++d) {
            if (d == d_zero) continue;
            const size_t shift_bytes = layout.shift[p * k + d] * BYTE_PERCELL;
            xor_buf(parity + shift_bytes, D[d], chunk_bytes);
        }
    }
}

int ErasureCodeTwotoneImpl::twotone_decode(int *erasures, char **data, char **coding, int blocksize)
{
    const size_t cell_num = blocksize / BYTE_PERCELL;

    bool missing_chunk[k + m] = {};
    for (int i = 0; erasures[i] != -1; ++i)
        missing_chunk[erasures[i]] = true;

    // Count missing data chunks
    int missing_data_count = 0;
    for (int d = 0; d < k; ++d)
        if (missing_chunk[d]) ++missing_data_count;

    const size_t chunk_bytes = (size_t)blocksize;

    // ------------------------------------------------------------------
    // Fast path: single missing data chunk — tile-based column-major decode.
    //
    // For each output tile [tile_off, tile_off+tlen):
    //   D[d_miss][tile_off+i] = parity[tile_off+i + shift_miss]
    //                         XOR (XOR of D[d][tile_off+i + delta_d] for valid d)
    //   where delta_d = shift_miss - shift_d  (signed bytes)
    //
    // The tile accumulator stays in L1 cache. Each data chunk and the parity
    // are read exactly once → memory traffic = (k+1) × chunk_bytes, same as ISA-L.
    // ------------------------------------------------------------------
    if (missing_data_count == 1) {
        int d_miss = -1;
        for (int d = 0; d < k; ++d)
            if (missing_chunk[d]) { d_miss = d; break; }

        // Prefer the parity with the smallest max_shift — zero-shift parities
        // allow the boundary-free fast path below and minimise decode complexity.
        int p_use = -1;
        for (int p = 0; p < m; ++p) {
            if (!missing_chunk[k + p]) {
                if (p_use < 0 || layout.max_shift[p] < layout.max_shift[p_use])
                    p_use = p;
            }
        }
        ceph_assert(p_use >= 0);

        const size_t shift_miss_bytes = layout.shift[p_use * k + d_miss] * BYTE_PERCELL;

        constexpr size_t TILE_BYTES = 4096;
        if (scratch_buf.size() < TILE_BYTES)
            scratch_buf.resize(TILE_BYTES);
        char* tile = scratch_buf.data();

        // ------------------------------------------------------------------
        // Zero-delta fast path: when max_shift[p_use]==0 every shift for this
        // parity is zero, so shift_miss_bytes==0 and all delta[d]==0.
        // No boundary clipping ever occurs — inner loop is plain k-source XOR.
        // For k=6,m=3 this activates whenever p=1 is available (max_shift[1]=0).
        //
        // Optimised: accumulate all k sources in registers per tile position,
        // then non-temporal-store directly to data[d_miss].  Eliminates the
        // scratch-buffer round-trip and avoids polluting caches with the output.
        // ------------------------------------------------------------------
        if (layout.max_shift[p_use] == 0) {
#if defined(__AVX2__)
            char* out = data[d_miss];
            const char* par = coding[p_use];
            for (size_t tile_off = 0; tile_off < chunk_bytes; tile_off += TILE_BYTES) {
                const size_t tlen = std::min(TILE_BYTES, chunk_bytes - tile_off);
                size_t i = 0;
                for (; i + 128 <= tlen; i += 128) {
                    __m256i r0 = _mm256_loadu_si256((const __m256i*)(par + tile_off + i +  0));
                    __m256i r1 = _mm256_loadu_si256((const __m256i*)(par + tile_off + i + 32));
                    __m256i r2 = _mm256_loadu_si256((const __m256i*)(par + tile_off + i + 64));
                    __m256i r3 = _mm256_loadu_si256((const __m256i*)(par + tile_off + i + 96));
                    for (int d = 0; d < k; ++d) {
                        if (d == d_miss) continue;
                        const char* s = data[d] + tile_off;
                        r0 = _mm256_xor_si256(r0, _mm256_loadu_si256((const __m256i*)(s + i +  0)));
                        r1 = _mm256_xor_si256(r1, _mm256_loadu_si256((const __m256i*)(s + i + 32)));
                        r2 = _mm256_xor_si256(r2, _mm256_loadu_si256((const __m256i*)(s + i + 64)));
                        r3 = _mm256_xor_si256(r3, _mm256_loadu_si256((const __m256i*)(s + i + 96)));
                    }
                    _mm256_stream_si256((__m256i*)(out + tile_off + i +  0), r0);
                    _mm256_stream_si256((__m256i*)(out + tile_off + i + 32), r1);
                    _mm256_stream_si256((__m256i*)(out + tile_off + i + 64), r2);
                    _mm256_stream_si256((__m256i*)(out + tile_off + i + 96), r3);
                }
                for (; i + 32 <= tlen; i += 32) {
                    __m256i r = _mm256_loadu_si256((const __m256i*)(par + tile_off + i));
                    for (int d = 0; d < k; ++d) {
                        if (d == d_miss) continue;
                        r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(data[d] + tile_off + i)));
                    }
                    _mm256_stream_si256((__m256i*)(out + tile_off + i), r);
                }
                for (; i + 16 <= tlen; i += 16) {
                    __m128i r = _mm_loadu_si128((const __m128i*)(par + tile_off + i));
                    for (int d = 0; d < k; ++d) {
                        if (d == d_miss) continue;
                        r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(data[d] + tile_off + i)));
                    }
                    _mm_stream_si128((__m128i*)(out + tile_off + i), r);
                }
            }
            _mm_sfence();
#else
            if (scratch_buf.size() < TILE_BYTES)
                scratch_buf.resize(TILE_BYTES);
            char* tile = scratch_buf.data();
            for (size_t tile_off = 0; tile_off < chunk_bytes; tile_off += TILE_BYTES) {
                const size_t tlen = std::min(TILE_BYTES, chunk_bytes - tile_off);
                memcpy(tile, coding[p_use] + tile_off, tlen);
                for (int d = 0; d < k; ++d) {
                    if (d == d_miss) continue;
                    xor_buf(tile, data[d] + tile_off, tlen);
                }
                memcpy(data[d_miss] + tile_off, tile, tlen);
            }
#endif
            reconstruct_parity(k, m, layout, chunk_bytes, data, coding, missing_chunk);
            return 0;
        }

        // General tile path: handles non-zero shifts with boundary clamping.
        ssize_t delta[k];
        for (int d = 0; d < k; ++d)
            delta[d] = (ssize_t)shift_miss_bytes
                     - (ssize_t)(layout.shift[p_use * k + d] * BYTE_PERCELL);

        for (size_t tile_off = 0; tile_off < chunk_bytes; tile_off += TILE_BYTES) {
            const size_t tlen = std::min(TILE_BYTES, chunk_bytes - tile_off);

            memcpy(tile, coding[p_use] + shift_miss_bytes + tile_off, tlen);

            for (int d = 0; d < k; ++d) {
                if (d == d_miss) continue;

                const ssize_t src_start = (ssize_t)tile_off + delta[d];
                const ssize_t src_end   = src_start + (ssize_t)tlen;

                if (src_end <= 0 || src_start >= (ssize_t)chunk_bytes) continue;

                const ssize_t cstart   = std::max(src_start, (ssize_t)0);
                const ssize_t cend     = std::min(src_end,   (ssize_t)chunk_bytes);
                const size_t  vlen     = (size_t)(cend - cstart);
                const size_t  tile_dst = (size_t)(cstart - src_start);

                xor_buf(tile + tile_dst, data[d] + cstart, vlen);
            }

            memcpy(data[d_miss] + tile_off, tile, tlen);
        }

        reconstruct_parity(k, m, layout, chunk_bytes, data, coding, missing_chunk);
        return 0;
    }

    // ------------------------------------------------------------------
    // General case: iterative peeling decoder (cell-level).
    // Uses a flat uint8_t array instead of vector<vector<bool>> to avoid
    // bit-manipulation overhead and improve cache behaviour.
    // __uint128_t** casts are needed here for individual cell access.
    // ------------------------------------------------------------------
    __uint128_t** D = reinterpret_cast<__uint128_t**>(data);
    __uint128_t** P = reinterpret_cast<__uint128_t**>(coding);

    // known_flat[d * cell_num + off] = 1 if D[d][off] is known
    std::vector<uint8_t> known_flat(k * cell_num, 1);
    for (int d = 0; d < k; ++d)
        if (missing_chunk[d])
            memset(&known_flat[d * cell_num], 0, cell_num);

    bool progress = true;
    while (progress) {
        progress = false;
        for (int p = 0; p < m; ++p) {
            if (missing_chunk[k + p]) continue;
            const size_t parity_cells = cell_num + layout.max_shift[p];
            __uint128_t* parity = P[p];

            for (size_t t = 0; t < parity_cells; ++t) {
                __uint128_t val = parity[t];
                int unknown_cnt = 0;
                int u_data = -1;
                size_t u_off = 0;

                for (int d = 0; d < k; ++d) {
                    const size_t shift_d = layout.shift[p * k + d];
                    if (t < shift_d) continue;
                    const size_t off = t - shift_d;
                    if (off >= cell_num) continue;

                    if (known_flat[d * cell_num + off]) {
                        val ^= D[d][off];
                    } else {
                        ++unknown_cnt;
                        u_data = d;
                        u_off  = off;
                    }
                }

                if (unknown_cnt == 1) {
                    D[u_data][u_off]                      = val;
                    known_flat[u_data * cell_num + u_off] = 1;
                    progress = true;
                }
            }
        }
    }

    // Check all data cells recovered
    for (int d = 0; d < k; ++d)
        for (size_t i = 0; i < cell_num; ++i)
            if (!known_flat[d * cell_num + i])
                return -EIO;

    // reconstruct_parity(k, m, layout, chunk_bytes, data, coding, missing_chunk);
    return 0;
}

unsigned ErasureCodeTwotoneImpl::get_alignment() const {
  return k * CELLSIZE;
}

void ErasureCodeTwotoneImpl::prepare()
{
  layout = build_layout(k, m);
}

int ErasureCodeTwotoneImpl::parse(ErasureCodeProfile &profile,
						     ostream *ss)
{
  int err = 0;
  err |= ErasureCodeTwotone::parse(profile, ss);
  return err;
}
