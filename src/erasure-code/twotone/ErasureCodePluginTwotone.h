#ifndef CEPH_ERASURE_CODE_PLUGIN_TWOTONE_H
#define CEPH_ERASURE_CODE_PLUGIN_TWOTONE_H

#include "erasure-code/ErasureCodePlugin.h"

class ErasureCodePluginTwotone : public ceph::ErasureCodePlugin {
public:
  int factory(const std::string& directory,
	      ceph::ErasureCodeProfile &profile,
	      ceph::ErasureCodeInterfaceRef *erasure_code,
	      std::ostream *ss) override;
};

#endif
