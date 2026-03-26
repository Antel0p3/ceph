// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#include <errno.h>
#include "include/encoding.h"
#include "ECUtil.h"

using namespace std;
using ceph::bufferlist;
using ceph::ErasureCodeInterfaceRef;
using ceph::Formatter;

int ECUtil::decode(
  const stripe_info_t &sinfo,
  ErasureCodeInterfaceRef &ec_impl,
  map<int, bufferlist> &to_decode,
  bufferlist *out) {
  ceph_assert(to_decode.size());
  ceph_assert(out);
  ceph_assert(out->length() == 0);

  const int k = ec_impl->get_data_chunk_count();
  const uint64_t data_chunk_size = sinfo.get_chunk_size();
  const uint64_t stripe_width = sinfo.get_stripe_width();

  // Determine the number of stripes from the first available data shard.
  // Fall back to parity shards (using their per-shard chunk size) when all
  // data shards are absent.
  uint64_t n_stripes = 0;
  for (auto& [id, bl] : to_decode) {
    if (id < k) {
      ceph_assert(bl.length() % data_chunk_size == 0);
      n_stripes = bl.length() / data_chunk_size;
      break;
    }
  }
  if (n_stripes == 0) {
    for (auto& [id, bl] : to_decode) {
      uint64_t par_size = ec_impl->get_parity_chunk_size(stripe_width, id - k);
      ceph_assert(par_size > 0);
      n_stripes = bl.length() / par_size;
      break;
    }
  }

  if (n_stripes == 0)
    return 0;

  for (uint64_t s = 0; s < n_stripes; ++s) {
    map<int, bufferlist> chunks;
    for (auto& [id, bl] : to_decode) {
      uint64_t chunk_size = (id < k)
        ? data_chunk_size
        : ec_impl->get_parity_chunk_size(stripe_width, id - k);
      chunks[id].substr_of(bl, s * chunk_size, chunk_size);
    }
    bufferlist bl;
    int r = ec_impl->decode_concat(chunks, &bl);
    ceph_assert(r == 0);
    ceph_assert(bl.length() == stripe_width);
    out->claim_append(bl);
  }
  return 0;
}

int ECUtil::decode(
  const stripe_info_t &sinfo,
  ErasureCodeInterfaceRef &ec_impl,
  map<int, bufferlist> &to_decode,
  map<int, bufferlist*> &out) {

  ceph_assert(to_decode.size());

  for (auto &&i : to_decode) {
    if(i.second.length() == 0)
      return 0;
  }

  set<int> need;
  for (map<int, bufferlist*>::iterator i = out.begin();
       i != out.end();
       ++i) {
    ceph_assert(i->second);
    ceph_assert(i->second->length() == 0);
    need.insert(i->first);
  }

  set<int> avail;
  for (auto &&i : to_decode) {
    ceph_assert(i.second.length() != 0);
    avail.insert(i.first);
  }

  map<int, vector<pair<int, int>>> min;
  int r = ec_impl->minimum_to_decode(need, avail, &min);
  ceph_assert(r == 0);

  const int k = ec_impl->get_data_chunk_count();
  const uint64_t data_chunk_size = sinfo.get_chunk_size();
  const uint64_t stripe_width = sinfo.get_stripe_width();
  int subchunk_size = data_chunk_size / ec_impl->get_sub_chunk_count();

  // repair_data_per_chunk is the per-stripe stride for DATA shards, taking
  // sub-chunk repair (e.g. CLAY) into account.  For codes with one sub-chunk
  // per chunk this equals data_chunk_size.
  int repair_data_per_chunk = 0;
  for (auto& [id, subchunk_list] : min) {
    int repair_subchunk_count = 0;
    for (auto& subchunks : subchunk_list)
      repair_subchunk_count += subchunks.second;
    repair_data_per_chunk = repair_subchunk_count * subchunk_size;
    break;
  }

  // Derive n_stripes from the first DATA shard found in min; fall back to
  // parity shards (using their own per-stripe size) if no data shard is used.
  int chunks_count = 0;
  for (auto& [id, bl] : to_decode) {
    if (!min.count(id)) continue;
    if (id < k) {
      chunks_count = (int)bl.length() / repair_data_per_chunk;
      break;
    }
  }
  if (chunks_count == 0) {
    for (auto& [id, bl] : to_decode) {
      if (!min.count(id)) continue;
      uint64_t par_size = ec_impl->get_parity_chunk_size(stripe_width, id - k);
      chunks_count = (int)bl.length() / (int)par_size;
      break;
    }
  }

  for (int s = 0; s < chunks_count; s++) {
    map<int, bufferlist> chunks;
    for (auto& [id, bl] : to_decode) {
      if (!min.count(id)) continue;
      uint64_t sz  = (id < k)
        ? (uint64_t)repair_data_per_chunk
        : ec_impl->get_parity_chunk_size(stripe_width, id - k);
      chunks[id].substr_of(bl, s * sz, sz);
    }
    map<int, bufferlist> out_bls;
    r = ec_impl->decode(need, chunks, &out_bls, data_chunk_size);
    ceph_assert(r == 0);
    for (auto& [id, target] : out) {
      ceph_assert(out_bls.count(id));
      uint64_t expected = (id < k)
        ? data_chunk_size
        : ec_impl->get_parity_chunk_size(stripe_width, id - k);
      ceph_assert(out_bls[id].length() == expected);
      target->claim_append(out_bls[id]);
    }
  }
  for (auto& [id, target] : out) {
    uint64_t expected = (id < k)
      ? (uint64_t)chunks_count * data_chunk_size
      : (uint64_t)chunks_count * ec_impl->get_parity_chunk_size(stripe_width, id - k);
    ceph_assert(target->length() == expected);
  }
  return 0;
}

int ECUtil::encode(
  const stripe_info_t &sinfo,
  ErasureCodeInterfaceRef &ec_impl,
  bufferlist &in,
  const set<int> &want,
  map<int, bufferlist> *out) {

  uint64_t logical_size = in.length();

  ceph_assert(logical_size % sinfo.get_stripe_width() == 0);
  ceph_assert(out);
  ceph_assert(out->empty());

  if (logical_size == 0)
    return 0;

  const int k = ec_impl->get_data_chunk_count();
  const uint64_t stripe_width = sinfo.get_stripe_width();

  for (uint64_t i = 0; i < logical_size; i += stripe_width) {
    map<int, bufferlist> encoded;
    bufferlist buf;
    buf.substr_of(in, i, stripe_width);
    int r = ec_impl->encode(want, buf, &encoded);
    ceph_assert(r == 0);
    for (auto& [id, bl] : encoded) {
      uint64_t expected = (id < k)
        ? sinfo.get_chunk_size()
        : ec_impl->get_parity_chunk_size(stripe_width, id - k);
      ceph_assert(bl.length() == expected);
      (*out)[id].claim_append(bl);
    }
  }

  for (auto& [id, bl] : *out) {
    uint64_t chunk_size = (id < k)
      ? sinfo.get_chunk_size()
      : ec_impl->get_parity_chunk_size(stripe_width, id - k);
    ceph_assert(bl.length() % chunk_size == 0);
  }
  return 0;
}

void ECUtil::HashInfo::append(uint64_t old_size,
			      map<int, bufferlist> &to_append) {
  ceph_assert(old_size == total_chunk_size);
  uint64_t size_to_append = to_append.begin()->second.length();
  if (has_chunk_hash()) {
    ceph_assert(to_append.size() == cumulative_shard_hashes.size());
    for (map<int, bufferlist>::iterator i = to_append.begin();
	 i != to_append.end();
	 ++i) {
      // Parity shards may be larger than data shards for variable-parity codes;
      // each shard's CRC covers its full actual length.
      ceph_assert((unsigned)i->first < cumulative_shard_hashes.size());
      uint32_t new_hash = i->second.crc32c(cumulative_shard_hashes[i->first]);
      cumulative_shard_hashes[i->first] = new_hash;
    }
  }
  // size_to_append comes from the first (lowest-id) shard, which is always a
  // data shard (shard 0).  total_chunk_size tracks data-shard bytes so that
  // get_total_logical_size() = total_chunk_size * k remains correct.
  total_chunk_size += size_to_append;
}

void ECUtil::HashInfo::encode(bufferlist &bl) const
{
  ENCODE_START(1, 1, bl);
  encode(total_chunk_size, bl);
  encode(cumulative_shard_hashes, bl);
  ENCODE_FINISH(bl);
}

void ECUtil::HashInfo::decode(bufferlist::const_iterator &bl)
{
  DECODE_START(1, bl);
  decode(total_chunk_size, bl);
  decode(cumulative_shard_hashes, bl);
  projected_total_chunk_size = total_chunk_size;
  DECODE_FINISH(bl);
}

void ECUtil::HashInfo::dump(Formatter *f) const
{
  f->dump_unsigned("total_chunk_size", total_chunk_size);
  f->open_array_section("cumulative_shard_hashes");
  for (unsigned i = 0; i != cumulative_shard_hashes.size(); ++i) {
    f->open_object_section("hash");
    f->dump_unsigned("shard", i);
    f->dump_unsigned("hash", cumulative_shard_hashes[i]);
    f->close_section();
  }
  f->close_section();
}

namespace ECUtil {
std::ostream& operator<<(std::ostream& out, const HashInfo& hi)
{
  ostringstream hashes;
  for (auto hash: hi.cumulative_shard_hashes)
    hashes << " " << hex << hash;
  return out << "tcs=" << hi.total_chunk_size << hashes.str();
}
}

void ECUtil::HashInfo::generate_test_instances(list<HashInfo*>& o)
{
  o.push_back(new HashInfo(3));
  {
    bufferlist bl;
    bl.append_zero(20);
    map<int, bufferlist> buffers;
    buffers[0] = bl;
    buffers[1] = bl;
    buffers[2] = bl;
    o.back()->append(0, buffers);
    o.back()->append(20, buffers);
  }
  o.push_back(new HashInfo(4));
}

const string HINFO_KEY = "hinfo_key";

bool ECUtil::is_hinfo_key_string(const string &key)
{
  return key == HINFO_KEY;
}

const string &ECUtil::get_hinfo_key()
{
  return HINFO_KEY;
}
