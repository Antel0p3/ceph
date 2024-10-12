#ifndef CEPH_ERASURE_CODE_TWOTONE_H
#define CEPH_ERASURE_CODE_TWOTONE_H

#include "erasure-code/ErasureCode.h"

class ErasureCodeTwotone : public ceph::ErasureCode {
public:
  int k;
  std::string DEFAULT_K;
  int m;
  std::string DEFAULT_M;
  int w;
  std::string DEFAULT_W;

  ErasureCodeTwotone() :
    k(0),
    DEFAULT_K("3"),
    m(0),
    DEFAULT_M("3"),
    w(0),
    DEFAULT_W("128")
  {}

  ~ErasureCodeTwotone() override {}
  
  unsigned int get_chunk_count() const override {
    return k + m;
  }

  unsigned int get_data_chunk_count() const override {
    return k;
  }

  unsigned int get_chunk_size(unsigned int object_size) const override;

  int encode_chunks(const std::set<int> &want_to_encode,
		    std::map<int, ceph::buffer::list> *encoded) override;

  int decode_chunks(const std::set<int> &want_to_read,
		    const std::map<int, ceph::buffer::list> &chunks,
		    std::map<int, ceph::buffer::list> *decoded) override;

  int init(ceph::ErasureCodeProfile &profile, std::ostream *ss) override;

  virtual void twotone_encode(char **data,
                               char **coding,
                               int blocksize) = 0;
  virtual int twotone_decode(int *erasures,
                               char **data,
                               char **coding,
                               int blocksize) = 0;
  virtual unsigned get_alignment() const = 0;
  virtual void prepare() = 0;
  static bool is_prime(int value);
protected:
  virtual int parse(ceph::ErasureCodeProfile &profile, std::ostream *ss);
};

class ErasureCodeTwotoneImpl : public ErasureCodeTwotone {
public:
  ErasureCodeTwotoneImpl(): ErasureCodeTwotone() {
  }

  ~ErasureCodeTwotoneImpl() override {
  }

  void twotone_encode(char **data,
			   char **coding,
			   int blocksize) override;
  int twotone_decode(int *erasures,
                  char **data,
                  char **coding,
                  int blocksize) override;
  unsigned get_alignment() const override;
  void prepare() override;
private:
  int parse(ceph::ErasureCodeProfile &profile, std::ostream *ss) override;
};

// class ErasureCodeTwotoneReedSolomonVandermonde : public ErasureCodeTwotone {
// public:
//   int *matrix;

//   ErasureCodeTwotoneReedSolomonVandermonde() :
//     ErasureCodeTwotone("reed_sol_van"),
//     matrix(0)
//   {
//     DEFAULT_K = "7";
//     DEFAULT_M = "3";
//     DEFAULT_W = "8";
//   }
//   ~ErasureCodeTwotoneReedSolomonVandermonde() override {
//     if (matrix)
//       free(matrix);
//   }

//   void twotone_encode(char **data,
//                                char **coding,
//                                int blocksize) override;
//   int twotone_decode(int *erasures,
//                                char **data,
//                                char **coding,
//                                int blocksize) override;
//   unsigned get_alignment() const override;
//   void prepare() override;
// private:
//   int parse(ceph::ErasureCodeProfile& profile, std::ostream *ss) override;
// };

#endif
