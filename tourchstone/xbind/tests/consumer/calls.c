#include <BFDBinABI.h>
#include <dyninvoke/dyninvoke.h>
#include <dynload/dynload.h>
#include <xbind.h>

int calls(const char* isa_dir) {
  tdyn_env_t* env = tdyn_env_new();
  if (!env) return 1;
  tdyn_env_destroy(env);
  size_t gpr = 0, fpr = 0;
  return dinv_isa_regcounts(DINV_ABI_SYSV_AMD64, isa_dir, &gpr, &fpr) != DINV_OK ||
         gpr == 0 || fpr == 0 ||
         xbind_target_count() != 9 ||
         dinv_type_size(&dinv_type_i32) != 4 ||
         archbfd_abi_version() != ARCHBFD_ABI_VERSION;
}
