#if defined(_WIN32)
#  if defined(TDYNLOAD_FIXTURE_BUILD)
#    define TDYNLOAD_FIXTURE_API __declspec(dllexport)
#  else
#    define TDYNLOAD_FIXTURE_API __declspec(dllimport)
#  endif
#else
#  define TDYNLOAD_FIXTURE_API __attribute__((visibility("default")))
#endif

TDYNLOAD_FIXTURE_API int tdynload_fixture_add(int a, int b) {
  return a + b;
}

TDYNLOAD_FIXTURE_API int tdynload_fixture_value = 42;

TDYNLOAD_FIXTURE_API const char* tdynload_fixture_name(void) {
  return "tdynload-fixture";
}

#if defined(_WIN32)
/* Exported by ordinal without a name, through fixture.def. */
int tdynload_fixture_ordinal(void) { return 17; }
#endif

#if defined(TDYNLOAD_TEST_ELF_VERSIONS)
int tdynload_fixture_old(void) { return 1; }
int tdynload_fixture_new(void) { return 2; }
__asm__(".symver tdynload_fixture_old,tdynload_fixture_versioned@TDYN_1");
__asm__(".symver tdynload_fixture_new,tdynload_fixture_versioned@@TDYN_2");

TDYNLOAD_FIXTURE_API _Thread_local int tdynload_fixture_tls = 123;
TDYNLOAD_FIXTURE_API int* tdynload_fixture_tls_pointer(void) {
  return &tdynload_fixture_tls;
}
#endif
