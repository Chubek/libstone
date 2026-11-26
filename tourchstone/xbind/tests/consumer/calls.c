#include <BFDBinABI.h>
#include <dyninvoke/dyninvoke.h>
#include <dynload/dynload.h>
#include <xbind.h>

int calls(void) {
  tdyn_env_t* env = tdyn_env_new();
  if (!env) return 1;
  tdyn_env_destroy(env);
  return xbind_target_count() != 9 ||
         dinv_type_size(&dinv_type_i32) != 4 ||
         archbfd_abi_version() != ARCHBFD_ABI_VERSION;
}
