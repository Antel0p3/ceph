#include "common/debug.h"
#include "ErasureCodeTwotone.h"


// extern "C" {
// #include "twotone.h"
// #include "reed_sol.h"
// #include "galois.h"
// #include "cauchy.h"
// #include "liberation.h"
// }

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
  m = k;
  // err |= to_int("m", profile, &m, DEFAULT_M, ss);
  // err |= to_int("w", profile, &w, DEFAULT_W, ss);
  dout(10) << "k set to " << k << ", m set equal to k" << dendl;
  // if (chunk_mapping.size() > 0 && (int)chunk_mapping.size() != k + m) {
  //   *ss << "mapping " << profile.find("mapping")->second
	// << " maps " << chunk_mapping.size() << " chunks instead of"
	// << " the expected " << k + m << " and will be ignored" << std::endl;
  //   chunk_mapping.clear();
  //   err = -EINVAL;
  // }
  // err |= sanity_check_k_m(k, m, ss);
  return err;
}

unsigned int ErasureCodeTwotone::get_chunk_size(unsigned int object_size) const
{
  // unsigned alignment = get_alignment();
  unsigned all_cell_num = UPTO_K(UPTO_K(object_size * BYTESIZE, CELLSIZE) / CELLSIZE, k);
  unsigned padded_length = all_cell_num * BYTE_PERCELL;
  // unsigned line_cell_num = all_cell_num / k;
  // printf("line_cell_num: %u\n", all_cell_num / k);
  // unsigned tail = object_size % alignment;
  // unsigned padded_length = object_size + ( tail ?  ( alignment - tail ) : 0 );
  ceph_assert(padded_length % k == 0);
  return padded_length / k;
}

int ErasureCodeTwotone::encode_chunks(const set<int> &want_to_encode,
				       map<int, bufferlist> *encoded)
{
  // add extra cells to encoded chunks
  unsigned int bytes_per_cell = 16;
  int base_block_size = (*encoded)[0].length();
  int extras[3] = {2, 0, 2};
  for (int i = k; i < k + m; i++) {
    (*encoded)[i].clear();
    (*encoded)[i].push_back(buffer::create_aligned(base_block_size + extras[i-k] * bytes_per_cell, 32));
  }

  char *chunks[k + m];
  for (int i = 0; i < k + m; i++) {
    chunks[i] = (*encoded)[i].c_str();
    printf("%d: %d: \n", i, (*encoded)[i].length());
    for (int j = 0; j < base_block_size; j++)
    {
      printf("%02x ", (*encoded)[i].c_str()[j]);
    }
    printf("\n\n");
  }
  twotone_encode(&chunks[0], &chunks[k], base_block_size);
  printf("\n\n[encode_chunks] after encoding:::\n\n");
  for (int i = k; i < k + m; i++) {
    chunks[i] = (*encoded)[i].c_str();
    printf("%d: %d: \n", i, (*encoded)[i].length());
    for (int j = 0; j < int((*encoded)[i].length()); j++)
    {
      printf("%02x ", (*encoded)[i].c_str()[j]);
    }
    printf("\n\n");
  }
  return 0;
}

int ErasureCodeTwotone::decode_chunks(const set<int> &want_to_read,
				       const map<int, bufferlist> &chunks,
				       map<int, bufferlist> *decoded)
{
  unsigned blocksize = (*chunks.begin()).second.length();
  int erasures[k + m + 1];
  int erasures_count = 0;
  char *data[k];
  char *coding[m];

  // resize
  unsigned int bytes_per_cell = 16;
  int extras[3] = {2, 0, 2};
  blocksize = blocksize - extras[0] * bytes_per_cell;
  for (int i = 0; i < k; i++)
  {
    (*decoded)[i].clear();
    (*decoded)[i].push_back(buffer::create_aligned(blocksize, 32));
  }

  for (int i =  0; i < k + m; i++) {
    printf("%d: %d: \n", i, (*decoded)[i].length());

    if (chunks.find(i) == chunks.end()) {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    if (i < k)
      data[i] = (*decoded)[i].c_str();
    else
      coding[i - k] = (*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;

  ceph_assert(erasures_count > 0);
  return twotone_decode(erasures, data, coding, blocksize);
}

void puthex(void* con, int size) {
  for (int i = 0; i < size; i++)
  {
    printf("%02x ", ((char*)con)[i]);
  }
  printf("\n");
}

void encodek3(long line_cell_num, char **data, char **coding)
{
    __uint128_t *arrayk3[3];
    __uint128_t *res_array;
    arrayk3[0] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    arrayk3[1] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    arrayk3[2] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    res_array = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    // fread(arrayk3[0], sizeof(__uint128_t), line_cell_num, data[0]);
    // fread(arrayk3[1] + 1, sizeof(__uint128_t), line_cell_num, data[1]);
    // fread(arrayk3[2] + 2, sizeof(__uint128_t), line_cell_num, data[2]);
    memcpy(arrayk3[0] + 0, data[0], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[1] + 1, data[1], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[2] + 2, data[2], sizeof(__uint128_t)*line_cell_num);
    for (int i = 0; i < line_cell_num + 2; i++)
        res_array[i] = arrayk3[0][i] ^ arrayk3[1][i] ^ arrayk3[2][i];
    memcpy(coding[0], res_array, (line_cell_num + 2) * BYTE_PERCELL);
    puthex(res_array, (line_cell_num + 2) * BYTE_PERCELL);

    free(arrayk3[0]);
    free(arrayk3[1]);
    free(arrayk3[2]);

    arrayk3[0] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    arrayk3[1] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    arrayk3[2] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    res_array = (__uint128_t *)calloc(line_cell_num, CELLSIZE);

    // fread(arrayk3[0], sizeof(__uint128_t), line_cell_num, data[0]);
    // fread(arrayk3[1], sizeof(__uint128_t), line_cell_num, data[1]);
    // fread(arrayk3[2], sizeof(__uint128_t), line_cell_num, data[2]);
    memcpy(arrayk3[0], data[0], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[1], data[1], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[2], data[2], sizeof(__uint128_t)*line_cell_num);

    for (int i = 0; i < line_cell_num; i++)
        res_array[i] = arrayk3[0][i] ^ arrayk3[1][i] ^ arrayk3[2][i];
    memcpy(coding[1], res_array, line_cell_num * BYTE_PERCELL);
    puthex(res_array, line_cell_num * BYTE_PERCELL);
    // fwrite(res_array, sizeof(__uint128_t), line_cell_num, res_file);
    free(arrayk3[0]);
    free(arrayk3[1]);
    free(arrayk3[2]);
    free(res_array);

    arrayk3[0] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    arrayk3[1] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    arrayk3[2] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    res_array = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    // fread(arrayk3[0] + 2, sizeof(__uint128_t), line_cell_num, data[0]);
    // fread(arrayk3[1] + 1, sizeof(__uint128_t), line_cell_num, data[1]);
    // fread(arrayk3[2], sizeof(__uint128_t), line_cell_num, data[2]);
    memcpy(arrayk3[0] + 2, data[0], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[1] + 1, data[1], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[2] + 0, data[2], sizeof(__uint128_t)*line_cell_num);
    for (int i = 0; i < line_cell_num + 2; i++)
        res_array[i] = arrayk3[0][i] ^ arrayk3[1][i] ^ arrayk3[2][i];
    memcpy(coding[2], res_array, (line_cell_num + 2) * BYTE_PERCELL);
    puthex(res_array, (line_cell_num + 2) * BYTE_PERCELL);
    free(arrayk3[0]);
    free(arrayk3[1]);
    free(arrayk3[2]);
    free(res_array);
}

void decodek3(long line_cell_num, char **data, char **coding)
{
    __uint128_t *arrayk3[3];
    __uint128_t *res_array[3];
    arrayk3[0] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);
    arrayk3[1] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    arrayk3[2] = (__uint128_t *)calloc(line_cell_num + 2, CELLSIZE);

    res_array[0] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    res_array[1] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);
    res_array[2] = (__uint128_t *)calloc(line_cell_num, CELLSIZE);


    memcpy(arrayk3[0], data[0], sizeof(__uint128_t)*(line_cell_num+2));
    memcpy(arrayk3[1], data[1], sizeof(__uint128_t)*line_cell_num);
    memcpy(arrayk3[2], data[2], sizeof(__uint128_t)*(line_cell_num+2));
    // for (int i = 0; i < line_cell_num + 2; i++)
    //     res_array[i] = arrayk3[0][i] ^ arrayk3[1][i] ^ arrayk3[2][i];
    // memcpy(coding[0], res_array, (line_cell_num + 2) * BYTE_PERCELL);

    for (int j = 0; j < line_cell_num; j++)
    {
      __uint128_t val1 = ((j - 1) >= 0) ? res_array[1][j - 1] : 0;
      __uint128_t val2 = ((j - 2) >= 0) ? res_array[2][j - 2] : 0;
      __uint128_t val0 = ((j - 2) >= 0) ? res_array[0][j - 2] : 0;
      res_array[0][j] = arrayk3[0][j] ^ val1 ^ val2;
      res_array[2][j] = arrayk3[2][j] ^ val1 ^ val0;
      res_array[1][j] = arrayk3[1][j] ^ res_array[0][j] ^ res_array[2][j];
    }
    
    memcpy(coding[0], res_array[0], line_cell_num * BYTE_PERCELL);
    memcpy(coding[1], res_array[1], line_cell_num * BYTE_PERCELL);
    memcpy(coding[2], res_array[2], line_cell_num * BYTE_PERCELL);

    // printf("c0: \n");
    // puthex(arrayk3[0], (line_cell_num + 2) * BYTE_PERCELL);
    // printf("c1: \n");
    // puthex(arrayk3[1], (line_cell_num ) * BYTE_PERCELL);
    // printf("c2: \n");
    // puthex(arrayk3[2], (line_cell_num + 2) * BYTE_PERCELL);

    // printf("d0: \n");
    // puthex(res_array[0], (line_cell_num) * BYTE_PERCELL);
    // printf("d1: \n");
    // puthex(res_array[1], (line_cell_num) * BYTE_PERCELL);
    // printf("d2: \n");
    // puthex(res_array[2], (line_cell_num) * BYTE_PERCELL);


    free(arrayk3[0]);
    free(arrayk3[1]);
    free(arrayk3[2]);
    free(res_array[0]);
    free(res_array[1]);
    free(res_array[2]);

}

void ErasureCodeTwotoneImpl::twotone_encode(char **data, char **coding, int blocksize)
{
  encodek3(blocksize / BYTE_PERCELL, data, coding);
}

int ErasureCodeTwotoneImpl::twotone_decode(int *erasures, char **data, char **coding, int blocksize)
{
  decodek3(blocksize / BYTE_PERCELL, coding, data);
  return 0;
}

unsigned ErasureCodeTwotoneImpl::get_alignment() const {
  return k*CELLSIZE;
}

void ErasureCodeTwotoneImpl::prepare()
{
  
}

int ErasureCodeTwotoneImpl::parse(ErasureCodeProfile &profile,
						     ostream *ss)
{
  int err = 0;
  err |= ErasureCodeTwotone::parse(profile, ss);
  return err;
}