#include "common/debug.h"
#include "ErasureCodeTwotone.h"

#define LARGEST_VECTOR_WORDSIZE 16

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

  bool parity_has_base_size = false;
  // Optional: debug print after encoding
  // printf("\n\n[encode_chunks] after encoding:::\n\n");
  for (int i = 0; i < k + m; ++i) {
      // printf("%d: length=%u\n", i, (*encoded)[i].length());
      // for (unsigned j = 0; j < (*encoded)[i].length(); ++j) {
      //     printf("%02x ", (unsigned char)(*encoded)[i].c_str()[j]);
      //     if ((j+1) % 16 == 0) {
      //       printf("\n");
      //     }
      // }
      // printf("\n\n");
      if (i >= k && (*encoded)[i].length() == base_block_size) {
        parity_has_base_size = true;
      }
  }

  ceph_assert(parity_has_base_size);
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
   *  Allocate decoded buffers for ALL data shards
   * ------------------------------------------------- */
  for (int i = 0; i < k; ++i) {
    (*decoded)[i].clear();
    (*decoded)[i].push_back(buffer::create_aligned(blocksize, SIMD_ALIGN));
  }

  /* -------------------------------------------------
   *  Prepare data + coding pointers and erasures
   * ------------------------------------------------- */
  for (int i = 0; i < k + m; ++i) {
    bool missing = (chunks.find(i) == chunks.end());

    if (missing) {
      erasures[erasures_count++] = i;
    }

    if (i < k) {
      // data shard
      data[i] = (*decoded)[i].c_str();

      if (!missing) {
        // copy existing shard
        // chunks.find(i)->second.copy_out(0, blocksize, data[i]);
        bufferlist::const_iterator it = chunks.find(i)->second.begin();
        it.copy(blocksize, data[i]);
      } else {
        memset(data[i], 0, blocksize);
      }
    } else {
      // parity shard
      int p = i - k;

      unsigned parity_size = 0;
      if (!missing)
        parity_size = chunks.find(i)->second.length();
      else
        parity_size = (blocksize / BYTE_PERCELL + layout.max_shift[p]) * BYTE_PERCELL;

      (*decoded)[i].clear();
      (*decoded)[i].push_back(buffer::create_aligned(parity_size, SIMD_ALIGN));
      // printf("[decode_chunks] chunk %d, size: %d\n", i, parity_size);

      coding[p] = (*decoded)[i].c_str();

      if (!missing) {
        // copy 
        bufferlist::const_iterator it = chunks.find(i)->second.begin();
        it.copy(parity_size, coding[p]);
      } else {
        memset(coding[p], 0, parity_size);
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

void puthex(void* con, int size) {
  for (int i = 0; i < size; i++)
  {
    printf("%02x ", ((char*)con)[i]);
  }
  printf("\n");
}

void ErasureCodeTwotoneImpl::twotone_encode(char **data, char **coding, int blocksize)
{
  const size_t cell_num = blocksize / BYTE_PERCELL;

  for (int p = 0; p < m; ++p) {
    size_t out_cells = cell_num + layout.max_shift[p];
    __uint128_t* out = reinterpret_cast<__uint128_t*>(coding[p]);

    memset(out, 0, out_cells * sizeof(__uint128_t));

    for (int d = 0; d < k; ++d) {
      size_t shift = layout.shift[p * k + d];
      __uint128_t* src = reinterpret_cast<__uint128_t*>(data[d]);
      __uint128_t* dst = out + shift;

      // tight XOR loop, vectorizable
      for (size_t i = 0; i < cell_num; ++i) {
        dst[i] ^= src[i];
      }
    }
  }
}

// Reconstruct missing parity chunks using the same encode-style tight loops.
static void reconstruct_parity(int k, int m,
                                const TwotoneLayout &layout,
                                size_t cell_num,
                                __uint128_t **D, __uint128_t **P,
                                const std::vector<bool> &missing_chunk)
{
    for (int p = 0; p < m; ++p) {
        if (!missing_chunk[k + p]) continue;
        const size_t parity_cells = cell_num + layout.max_shift[p];
        __uint128_t* parity = P[p];
        memset(parity, 0, parity_cells * sizeof(__uint128_t));
        for (int d = 0; d < k; ++d) {
            const size_t shift_d = layout.shift[p * k + d];
            __uint128_t* src = D[d];
            __uint128_t* dst = parity + shift_d;
            for (size_t i = 0; i < cell_num; ++i)   // tight XOR, vectorizable
                dst[i] ^= src[i];
        }
    }
}

int ErasureCodeTwotoneImpl::twotone_decode(int *erasures, char **data, char **coding, int blocksize)
{
    const size_t cell_num = blocksize / BYTE_PERCELL;

    std::vector<bool> missing_chunk(k + m, false);
    for (int i = 0; erasures[i] != -1; ++i)
        missing_chunk[erasures[i]] = true;

    __uint128_t** D = reinterpret_cast<__uint128_t**>(data);
    __uint128_t** P = reinterpret_cast<__uint128_t**>(coding);

    // Count missing data chunks
    int missing_data_count = 0;
    for (int d = 0; d < k; ++d)
        if (missing_chunk[d]) ++missing_data_count;

    // ------------------------------------------------------------------
    // Fast path: single missing data chunk (the common case).
    //
    // Invert the encode directly:
    //   1. Copy parity into a residual buffer.
    //   2. XOR out each known data chunk's contribution (encode-style,
    //      tight dst[i]^=src[i] loops — SIMD-vectorizable).
    //   3. The residual at offset shift_miss IS the missing chunk.
    //
    // No known-tracking, no iteration, identical inner loop to encode.
    // ------------------------------------------------------------------
    if (missing_data_count == 1) {
        int d_miss = -1;
        for (int d = 0; d < k; ++d)
            if (missing_chunk[d]) { d_miss = d; break; }

        int p_use = -1;
        for (int p = 0; p < m; ++p)
            if (!missing_chunk[k + p]) { p_use = p; break; }

        ceph_assert(p_use >= 0);

        const size_t parity_cells = cell_num + layout.max_shift[p_use];
        const size_t shift_miss   = layout.shift[p_use * k + d_miss];

        // Residual = copy of the chosen parity chunk
        std::vector<__uint128_t> residual(parity_cells);
        memcpy(residual.data(), P[p_use], parity_cells * sizeof(__uint128_t));

        // XOR out all known data chunks (encode-style, tight vectorizable loops)
        for (int d = 0; d < k; ++d) {
            if (d == d_miss) continue;
            const size_t shift_d = layout.shift[p_use * k + d];
            __uint128_t* src = D[d];
            __uint128_t* dst = residual.data() + shift_d;
            for (size_t i = 0; i < cell_num; ++i)   // tight XOR, vectorizable
                dst[i] ^= src[i];
        }

        // Missing chunk sits at offset shift_miss in the residual
        memcpy(D[d_miss], residual.data() + shift_miss,
               cell_num * sizeof(__uint128_t));

        reconstruct_parity(k, m, layout, cell_num, D, P, missing_chunk);
        return 0;
    }

    // ------------------------------------------------------------------
    // General case: iterative peeling decoder (cell-level).
    // Uses a flat uint8_t array instead of vector<vector<bool>> to avoid
    // bit-manipulation overhead and improve cache behaviour.
    // ------------------------------------------------------------------

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
                    D[u_data][u_off]                        = val;
                    known_flat[u_data * cell_num + u_off]   = 1;
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

    // reconstruct_parity(k, m, layout, cell_num, D, P, missing_chunk);
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