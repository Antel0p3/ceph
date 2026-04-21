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

#if defined(__AVX2__)
static inline void xor_buf2(char* d0, char* d1,
                            const char* src, size_t bytes)
{
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src+i));
        _mm256_storeu_si256((__m256i*)(d0+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d0+i)), v));
        _mm256_storeu_si256((__m256i*)(d1+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d1+i)), v));
    }
    for (; i + 16 <= bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src+i));
        _mm_storeu_si128((__m128i*)(d0+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d0+i)), v));
        _mm_storeu_si128((__m128i*)(d1+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d1+i)), v));
    }
}
#elif defined(__SSE2__)
static inline void xor_buf2(char* d0, char* d1,
                            const char* src, size_t bytes)
{
    for (size_t i = 0; i < bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src+i));
        _mm_storeu_si128((__m128i*)(d0+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d0+i)), v));
        _mm_storeu_si128((__m128i*)(d1+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d1+i)), v));
    }
}
#else
static inline void xor_buf2(char* d0, char* d1,
                            const char* src, size_t bytes)
{
    xor_buf(d0, src, bytes);
    xor_buf(d1, src, bytes);
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

#if defined(__AVX2__)
static inline void xor_buf4(char* d0, char* d1, char* d2, char* d3,
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
        _mm256_storeu_si256((__m256i*)(d3+i),
            _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(d3+i)), v));
    }
    for (; i + 16 <= bytes; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i*)(src+i));
        _mm_storeu_si128((__m128i*)(d0+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d0+i)), v));
        _mm_storeu_si128((__m128i*)(d1+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d1+i)), v));
        _mm_storeu_si128((__m128i*)(d2+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d2+i)), v));
        _mm_storeu_si128((__m128i*)(d3+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d3+i)), v));
    }
}
#elif defined(__SSE2__)
static inline void xor_buf4(char* d0, char* d1, char* d2, char* d3,
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
        _mm_storeu_si128((__m128i*)(d3+i),
            _mm_xor_si128(_mm_loadu_si128((const __m128i*)(d3+i)), v));
    }
}
#else
static inline void xor_buf4(char* d0, char* d1, char* d2, char* d3,
                            const char* src, size_t bytes)
{
    xor_buf(d0, src, bytes);
    xor_buf(d1, src, bytes);
    xor_buf(d2, src, bytes);
    xor_buf(d3, src, bytes);
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
  dout(10) << "k set to " << k << "m set to " << m << dendl;

  err |= sanity_check_k_m(k, m, ss);
  return err;
}

unsigned int ErasureCodeTwotone::get_chunk_size(unsigned int object_size) const
{
  unsigned cell_num = UPTO_K(object_size * BYTESIZE, CELLSIZE) / CELLSIZE;
  unsigned all_cell_num = UPTO_K(cell_num, k);
  unsigned padded_length = all_cell_num * BYTE_PERCELL;
  ceph_assert(padded_length % k == 0);
  return padded_length / k;
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
  L.shift_bytes.resize(k * m);
  L.init_data.resize(m, -1);
  L.data_plan_off.resize(k + 1, 0);
  std::vector<unsigned> data_counts(k, 0);

  for (int p = 0; p < m; ++p) {
    for (int d = 0; d < k; ++d) {
      size_t s = twotone_shift(p, d, k, m);
      L.shift[p * k + d] = s;
      L.shift_bytes[p * k + d] = s * BYTE_PERCELL;
      L.max_shift[p] = std::max(L.max_shift[p], s);
      if (s == 0 && L.init_data[p] < 0) {
        L.init_data[p] = d;
      }
    }
    ceph_assert(L.init_data[p] >= 0);
    if (L.max_shift[p] == 0 && L.zero_shift_parity < 0) {
      L.zero_shift_parity = p;
    }
  }

  for (int d = 0; d < k; ++d) {
    for (int p = 0; p < m; ++p) {
      if (L.init_data[p] != d) {
        ++data_counts[d];
      }
    }
  }

  unsigned off = 0;
  for (int d = 0; d < k; ++d) {
    L.data_plan_off[d] = off;
    off += data_counts[d];
  }
  L.data_plan_off[k] = off;
  L.data_plan_parity.resize(off);
  L.data_plan_shift_bytes.resize(off);

  std::vector<unsigned> cursor = L.data_plan_off;
  for (int d = 0; d < k; ++d) {
    for (int p = 0; p < m; ++p) {
      if (L.init_data[p] == d) {
        continue;
      }
      const unsigned idx = cursor[d]++;
      L.data_plan_parity[idx] = p;
      L.data_plan_shift_bytes[idx] = L.shift_bytes[p * k + d];
    }
  }
  return L;
}

static inline void xor_into_many(char** dsts,
                                 int ndst,
                                 const char* src,
                                 size_t bytes)
{
  switch (ndst) {
    case 0:
      return;
    case 1:
      xor_buf(dsts[0], src, bytes);
      return;
    case 2:
      xor_buf2(dsts[0], dsts[1], src, bytes);
      return;
    case 3:
      xor_buf3(dsts[0], dsts[1], dsts[2], src, bytes);
      return;
    case 4:
      xor_buf4(dsts[0], dsts[1], dsts[2], dsts[3], src, bytes);
      return;
    default:
      for (int i = 0; i < ndst; ++i) {
        xor_buf(dsts[i], src, bytes);
      }
      return;
  }
}

static void encode_selected_parities(const TwotoneLayout& layout,
                                     int k,
                                     int m,
                                     char **data,
                                     char **coding,
                                     size_t bytes,
                                     const bool *selected_parity = nullptr)
{
  for (int p = 0; p < m; ++p) {
    if (selected_parity && !selected_parity[p]) {
      continue;
    }
    const int init_d = layout.init_data[p];
    memcpy(coding[p], data[init_d], bytes);
    if (layout.max_shift[p] > 0) {
      memset(coding[p] + bytes, 0, layout.max_shift[p] * BYTE_PERCELL);
    }
  }

  char* dsts[m];
  for (int d = 0; d < k; ++d) {
    int ndst = 0;
    for (unsigned idx = layout.data_plan_off[d];
         idx < layout.data_plan_off[d + 1];
         ++idx) {
      const int p = layout.data_plan_parity[idx];
      if (selected_parity && !selected_parity[p]) {
        continue;
      }
      dsts[ndst++] = coding[p] + layout.data_plan_shift_bytes[idx];
    }
    xor_into_many(dsts, ndst, data[d], bytes);
  }
}

static inline void xor_sources_generic(const char* const* srcs,
                                       int nsrc,
                                       char* dst,
                                       size_t bytes)
{
  ceph_assert(nsrc > 0);
  memcpy(dst, srcs[0], bytes);
  for (int i = 1; i < nsrc; ++i) {
    xor_buf(dst, srcs[i], bytes);
  }
}

#if defined(__AVX2__)
static inline void xor_sources_3_avx2(const char* s0,
                                      const char* s1,
                                      const char* s2,
                                      char* dst,
                                      size_t bytes)
{
  size_t i = 0;
  for (; i + 32 <= bytes; i += 32) {
    __m256i r = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(s0 + i)),
                                 _mm256_loadu_si256((const __m256i*)(s1 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s2 + i)));
    _mm256_storeu_si256((__m256i*)(dst + i), r);
  }
  for (; i + 16 <= bytes; i += 16) {
    __m128i r = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(s0 + i)),
                              _mm_loadu_si128((const __m128i*)(s1 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s2 + i)));
    _mm_storeu_si128((__m128i*)(dst + i), r);
  }
}

static inline void xor_sources_4_avx2(const char* s0,
                                      const char* s1,
                                      const char* s2,
                                      const char* s3,
                                      char* dst,
                                      size_t bytes)
{
  size_t i = 0;
  for (; i + 32 <= bytes; i += 32) {
    __m256i r = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(s0 + i)),
                                 _mm256_loadu_si256((const __m256i*)(s1 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s2 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s3 + i)));
    _mm256_storeu_si256((__m256i*)(dst + i), r);
  }
  for (; i + 16 <= bytes; i += 16) {
    __m128i r = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(s0 + i)),
                              _mm_loadu_si128((const __m128i*)(s1 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s2 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s3 + i)));
    _mm_storeu_si128((__m128i*)(dst + i), r);
  }
}

static inline void xor_sources_6_avx2(const char* s0,
                                      const char* s1,
                                      const char* s2,
                                      const char* s3,
                                      const char* s4,
                                      const char* s5,
                                      char* dst,
                                      size_t bytes)
{
  size_t i = 0;
  for (; i + 32 <= bytes; i += 32) {
    __m256i r = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(s0 + i)),
                                 _mm256_loadu_si256((const __m256i*)(s1 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s2 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s3 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s4 + i)));
    r = _mm256_xor_si256(r, _mm256_loadu_si256((const __m256i*)(s5 + i)));
    _mm256_storeu_si256((__m256i*)(dst + i), r);
  }
  for (; i + 16 <= bytes; i += 16) {
    __m128i r = _mm_xor_si128(_mm_loadu_si128((const __m128i*)(s0 + i)),
                              _mm_loadu_si128((const __m128i*)(s1 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s2 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s3 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s4 + i)));
    r = _mm_xor_si128(r, _mm_loadu_si128((const __m128i*)(s5 + i)));
    _mm_storeu_si128((__m128i*)(dst + i), r);
  }
}
#endif

static inline void xor_sources_to_dst(const char* const* srcs,
                                      int nsrc,
                                      char* dst,
                                      size_t bytes)
{
  switch (nsrc) {
    case 1:
      memcpy(dst, srcs[0], bytes);
      return;
#if defined(__AVX2__)
    case 3:
      xor_sources_3_avx2(srcs[0], srcs[1], srcs[2], dst, bytes);
      return;
    case 4:
      xor_sources_4_avx2(srcs[0], srcs[1], srcs[2], srcs[3], dst, bytes);
      return;
    case 6:
      xor_sources_6_avx2(srcs[0], srcs[1], srcs[2], srcs[3], srcs[4], srcs[5],
                         dst, bytes);
      return;
#endif
    default:
      xor_sources_generic(srcs, nsrc, dst, bytes);
      return;
  }
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

  /* Determine correct data blocksize = smallest data shard size present */
  unsigned blocksize = UINT_MAX;
  for (int i = 0; i < k+m; ++i) {
    auto it = chunks.find(i);
    if (it != chunks.end()) {
      blocksize = (blocksize < it->second.length()) ? blocksize : it->second.length();
    }
  }

  ceph_assert(blocksize != UINT_MAX);

  /* Prepare data + coding pointers and erasures.
   *
   * _decode() has already built decoded[i] for every chunk:
   *   present chunks  → aligned reference/copy of the input (data ready)
   *   missing chunks  → create_aligned(blocksize) uninitialized buffer
   *
   * Missing parity chunks need a fresh parity-sized allocation because
   * _decode() only allocated blocksize bytes (may be smaller than parity_size).
   */
  for (int i = 0; i < k + m; ++i) {
    bool missing = (chunks.find(i) == chunks.end());
    if (missing) erasures[erasures_count++] = i;

    if (i < k) {
      data[i] = (*decoded)[i].c_str();
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
        coding[p] = (*decoded)[i].c_str();
      }
    }
  }

  erasures[erasures_count] = -1;

  ceph_assert(erasures_count > 0);

  int res = twotone_decode(erasures, data, coding, blocksize);
  return res;
}


void ErasureCodeTwotoneImpl::twotone_encode(char **data, char **coding, int blocksize)
{
  encode_selected_parities(layout, k, m, data, coding, (size_t)blocksize);
}

static void reconstruct_parity(int k, int m,
                                const TwotoneLayout &layout,
                                size_t chunk_bytes,
                                char **D, char **P,
                                const bool *missing_chunk)
{
    bool selected_parity[m];
    bool any_missing = false;
    for (int p = 0; p < m; ++p) {
        selected_parity[p] = missing_chunk[k + p];
        any_missing = any_missing || selected_parity[p];
    }
    if (any_missing) {
        encode_selected_parities(layout, k, m, D, P, chunk_bytes, selected_parity);
    }
}

int ErasureCodeTwotoneImpl::twotone_decode(int *erasures, char **data, char **coding, int blocksize)
{
    const size_t cell_num = blocksize / BYTE_PERCELL;

    bool missing_chunk[k + m] = {};
    for (int i = 0; erasures[i] != -1; ++i)
        missing_chunk[erasures[i]] = true;

    int missing_data_count = 0;
    for (int d = 0; d < k; ++d)
        if (missing_chunk[d]) ++missing_data_count;

    const size_t chunk_bytes = (size_t)blocksize;

    if (missing_data_count == 0) {
        reconstruct_parity(k, m, layout, chunk_bytes, data, coding, missing_chunk);
        return 0;
    }

    // Fast path: single missing data chunk.
    if (missing_data_count == 1) {
        int d_miss = -1;
        for (int d = 0; d < k; ++d)
            if (missing_chunk[d]) { d_miss = d; break; }

        int p_use = -1;
        if (layout.zero_shift_parity >= 0 &&
            !missing_chunk[k + layout.zero_shift_parity]) {
            p_use = layout.zero_shift_parity;
        }
        for (int p = 0; p < m && p_use < 0; ++p) {
            if (!missing_chunk[k + p] &&
                (p_use < 0 || layout.max_shift[p] < layout.max_shift[p_use])) {
                p_use = p;
            }
        }
        ceph_assert(p_use >= 0);

        const size_t shift_miss_bytes = layout.shift[p_use * k + d_miss] * BYTE_PERCELL;

        constexpr size_t TILE_BYTES = 4096;
        if (scratch_buf.size() < TILE_BYTES)
            scratch_buf.resize(TILE_BYTES);
        char* tile = scratch_buf.data();

        // Zero-delta fast path: when max_shift[p_use]==0 every shift is zero.
        // Accumulate all k sources in registers, then non-temporal-store to output.
        if (layout.max_shift[p_use] == 0) {
            const char* srcs[k];
            int nsrc = 0;
            srcs[nsrc++] = coding[p_use];
            for (int d = 0; d < k; ++d) {
                if (d != d_miss) {
                    srcs[nsrc++] = data[d];
                }
            }
            ceph_assert(nsrc == k);
            xor_sources_to_dst(srcs, nsrc, data[d_miss], chunk_bytes);
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

    // General case: iterative peeling decoder (cell-level).
    __uint128_t** D = reinterpret_cast<__uint128_t**>(data);
    __uint128_t** P = reinterpret_cast<__uint128_t**>(coding);

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

    for (int d = 0; d < k; ++d)
        for (size_t i = 0; i < cell_num; ++i)
            if (!known_flat[d * cell_num + i])
                return -EIO;

    reconstruct_parity(k, m, layout, chunk_bytes, data, coding, missing_chunk);
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
