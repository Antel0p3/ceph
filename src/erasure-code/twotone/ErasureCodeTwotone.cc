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

int ErasureCodeTwotone::init(ErasureCodeProfile& profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
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

  // Optional: debug print after encoding
  printf("\n\n[encode_chunks] after encoding:::\n\n");
  for (int i = 0; i < k + m; ++i) {
      printf("%d: length=%u\n", i, (*encoded)[i].length());
      for (unsigned j = 0; j < (*encoded)[i].length(); ++j) {
          printf("%02x ", (unsigned char)(*encoded)[i].c_str()[j]);
          if ((j+1) % 16 == 0) {
            printf("\n");
          }
      }
      printf("\n\n");
  }

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
      if (i < k) {
        blocksize = it->second.length();
      } else {
        unsigned parity_len = it->second.length();
        blocksize = parity_len - layout.max_shift[i-k] * BYTE_PERCELL;
      }
      break;
    }
  }

  ceph_assert(blocksize != UINT_MAX);
  printf("[decode] blocksize: %d\n", blocksize);


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
  printf("\n\n[decode_chunks] after decoding:::\n\n");
  for (int i = 0; i < k + m; ++i) {
      printf("%d: length=%u\n", i, (*decoded)[i].length());
      for (unsigned j = 0; j < (*decoded)[i].length(); ++j) {
          printf("%02x ", (unsigned char)(*decoded)[i].c_str()[j]);
          if ((j+1) % 16 == 0) {
            printf("\n");
          }
      }
      printf("\n\n");
  }
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

int ErasureCodeTwotoneImpl::twotone_decode(int *erasures, char **data, char **coding, int blocksize)
{
    const size_t cell_num = blocksize / BYTE_PERCELL;
    std::vector<bool> missing(k + m, false);

    for (int i = 0; erasures[i] != -1; ++i)
        missing[erasures[i]] = true;

    __uint128_t** D = reinterpret_cast<__uint128_t**>(data);
    __uint128_t** P = reinterpret_cast<__uint128_t**>(coding);

    // Track known cells
    std::vector<std::vector<bool>> known(k, std::vector<bool>(cell_num, true));
    for (int d = 0; d < k; ++d)
        if (missing[d])
            std::fill(known[d].begin(), known[d].end(), false);

    // Iteratively reconstruct data cells
    bool progress = true;
    while (progress) {
        progress = false;
        for (int p = 0; p < m; ++p) {
            if (missing[k + p]) continue;
            const size_t parity_cells = cell_num + layout.max_shift[p];
            __uint128_t* parity = P[p];

            for (size_t t = 0; t < parity_cells; ++t) {
                __uint128_t val = parity[t];
                int unknown_cnt = 0;
                int u_data = -1;
                size_t u_off = 0;

                for (int d = 0; d < k; ++d) {
                    size_t shift = layout.shift[p * k + d];
                    if (t < shift) continue;
                    size_t off = t - shift;
                    if (off >= cell_num) continue;

                    if (known[d][off]) val ^= D[d][off];
                    else { unknown_cnt++; u_data = d; u_off = off; }
                }

                if (unknown_cnt == 1) {
                    D[u_data][u_off] = val;
                    known[u_data][u_off] = true;
                    progress = true;
                }
            }
        }
    }

    // Check all data recovered
    for (int d = 0; d < k; ++d)
      for (size_t i = 0; i < cell_num; ++i)
        if (!known[d][i])
          return -EIO;

    // Reconstruct missing parity chunks
    for (int p = 0; p < m; ++p) {
        if (!missing[k + p]) continue;
        __uint128_t* parity = P[p];
        const size_t parity_cells = cell_num + layout.max_shift[p];

        for (size_t t = 0; t < parity_cells; ++t) {
            __uint128_t val = 0;
            for (int d = 0; d < k; ++d) {
                size_t shift = layout.shift[p * k + d];
                if (t < shift) continue;
                size_t off = t - shift;
                if (off >= cell_num) continue;
                val ^= D[d][off];
            }
            parity[t] = val;
        }
    }

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