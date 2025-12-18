// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 * 
 */

#include <errno.h>
#include <stdlib.h>

#include "crush/CrushWrapper.h"
#include "include/stringify.h"
#include "erasure-code/twotone/ErasureCodeTwotone.h"
#include "global/global_context.h"
#include "common/config.h"
#include "gtest/gtest.h"

using namespace std;

template <typename T>
class ErasureCodeTest : public ::testing::Test {
 public:
};

typedef ::testing::Types<
  ErasureCodeTwotoneImpl
> TwotoneTypes;
TYPED_TEST_SUITE(ErasureCodeTest, TwotoneTypes);

TYPED_TEST(ErasureCodeTest, sanity_check_k)
{
  TypeParam twotone;
  twotone.prepare();
  // ErasureCodeProfile profile;
  // profile["k"] = "1";
  // profile["m"] = "1";
  // profile["packetsize"] = "8";
  // ostringstream errors;
  // EXPECT_EQ(-EINVAL, twotone.init(profile, &errors));
  // EXPECT_NE(std::string::npos, errors.str().find("must be >= 2"));
}

TYPED_TEST(ErasureCodeTest, encode33)
{
  TypeParam twotone;
  ErasureCodeProfile profile;
  profile["k"] = "3";
  profile["m"] = "3";
  twotone.init(profile, &cerr);

#define LARGE_ENOUGH 2048
  bufferptr in_ptr(buffer::create_page_aligned(LARGE_ENOUGH));
    in_ptr.zero();
    in_ptr.set_length(0);
    const char *payload =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    in_ptr.append(payload, strlen(payload));
    bufferlist in;
    in.push_back(in_ptr);
    int want_to_encode[] = { 0, 1, 2, 3, 4, 5 };
    map<int, bufferlist> encoded;
    EXPECT_EQ(0, twotone.encode(set<int>(want_to_encode, want_to_encode+6),
				 in,
				 &encoded));
}

TYPED_TEST(ErasureCodeTest, encode32)
{
  TypeParam twotone;
  ErasureCodeProfile profile;
  profile["k"] = "3";
  profile["m"] = "2";
  twotone.init(profile, &cerr);

#define LARGE_ENOUGH 2048
  bufferptr in_ptr(buffer::create_page_aligned(LARGE_ENOUGH));
    in_ptr.zero();
    in_ptr.set_length(0);
    const char *payload =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    in_ptr.append(payload, strlen(payload));
    bufferlist in;
    in.push_back(in_ptr);
    int want_to_encode[] = { 0, 1, 2, 3, 4 };
    map<int, bufferlist> encoded;
    EXPECT_EQ(0, twotone.encode(set<int>(want_to_encode, want_to_encode+6),
				 in,
				 &encoded));
}

TYPED_TEST(ErasureCodeTest, encode23)
{
  TypeParam twotone;
  ErasureCodeProfile profile;
  profile["k"] = "2";
  profile["m"] = "3";
  twotone.init(profile, &cerr);

#define LARGE_ENOUGH 2048
  bufferptr in_ptr(buffer::create_page_aligned(LARGE_ENOUGH));
    in_ptr.zero();
    in_ptr.set_length(0);
    const char *payload =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    in_ptr.append(payload, strlen(payload));
    bufferlist in;
    in.push_back(in_ptr);
    int want_to_encode[] = { 0, 1, 2, 3, 4 };
    map<int, bufferlist> encoded;
    EXPECT_EQ(0, twotone.encode(set<int>(want_to_encode, want_to_encode+6),
				 in,
				 &encoded));
}

/*
TYPED_TEST(ErasureCodeTest, encode_decode)
{
  TypeParam twotone;
  ErasureCodeProfile profile;
  profile["k"] = "3";
  twotone.init(profile, &cerr);

#define LARGE_ENOUGH 2048
  bufferptr in_ptr(buffer::create_page_aligned(LARGE_ENOUGH));
    in_ptr.zero();
    in_ptr.set_length(0);
    const char *payload =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    in_ptr.append(payload, strlen(payload));
    bufferlist in;
    in.push_back(in_ptr);
    int want_to_encode[] = { 0, 1, 2, 3, 4, 5 };
    map<int, bufferlist> encoded;

    EXPECT_EQ(0, twotone.encode(set<int>(want_to_encode, want_to_encode+6),
				 in,
				 &encoded));

    unsigned length = encoded[0].length();
    EXPECT_EQ(6u, encoded.size());
    
    EXPECT_EQ(0, memcmp(encoded[0].c_str(), in.c_str(), length));
    EXPECT_EQ(0, memcmp(encoded[1].c_str(), in.c_str() + length, length));
    EXPECT_EQ(0, memcmp(encoded[2].c_str(), in.c_str() + 2 * length,
			in.length() - 2 * length));
    
    // all data chunks missing 
    {
      map<int, bufferlist> degraded = encoded;
      degraded.erase(0);
      degraded.erase(1);
      degraded.erase(2);
      EXPECT_EQ(3u, degraded.size());
      int want_to_decode[] = { 0, 1, 2 };
      map<int, bufferlist> decoded;
      EXPECT_EQ(0, twotone._decode(set<int>(want_to_decode, want_to_decode+3),
				    degraded,
				    &decoded));
      // always decode all, regardless of want_to_decode
      EXPECT_EQ(6u, decoded.size()); 
      EXPECT_EQ(length, decoded[0].length());
      EXPECT_EQ(0, memcmp(decoded[0].c_str(), in.c_str(), length));
      EXPECT_EQ(0, memcmp(decoded[1].c_str(), in.c_str() + length, length));
      EXPECT_EQ(0, memcmp(decoded[2].c_str(), in.c_str() + 2 * length,
			  in.length() - 2 * length));
    }
}
*/