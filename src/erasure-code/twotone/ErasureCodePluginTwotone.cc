#include "ceph_ver.h"
#include "common/debug.h"
#include "ErasureCodeTwotone.h"
#include "ErasureCodePluginTwotone.h"
#include "jerasure_init.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

static std::ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodePluginTwotone: ";
}

int ErasureCodePluginTwotone::factory(const std::string& directory,
				       ceph::ErasureCodeProfile &profile,
				       ceph::ErasureCodeInterfaceRef *erasure_code,
				       std::ostream *ss) {
    ErasureCodeTwotone *interface;
	interface = new ErasureCodeTwotoneImpl();
    
    dout(20) << __func__ << ": " << profile << dendl;
    int r = interface->init(profile, ss);
    if (r) {
      delete interface;
      return r;
    }
    *erasure_code = ceph::ErasureCodeInterfaceRef(interface);
    return 0;
}

const char *__erasure_code_version() { return CEPH_GIT_NICE_VER; }

int __erasure_code_init(char *plugin_name, char *directory)
{
  auto& instance = ceph::ErasureCodePluginRegistry::instance();
  int w[] = { 4, 8, 16, 32 };
  int r = jerasure_init(4, w);
  if (r) {
    return -r;
  }
  return instance.add(plugin_name, new ErasureCodePluginTwotone());
}
