"""Exercise qobjld-produced ELF images with the real Linux runtime loader."""
import os
import ctypes
import pathlib
import shutil
import subprocess
import sys
import tempfile

ld, cc, api, c_api = map(os.path.abspath, sys.argv[1:])


def run(*args, expected=0, **kwargs):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, **kwargs)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


with tempfile.TemporaryDirectory(prefix="qld-dynamic-") as tmp:
    root = pathlib.Path(tmp)
    os.environ["TMPDIR"] = tmp
    src = root / "provider.c"
    src.write_text("int answer = 41; int value(void) { return answer + 1; }\n")
    obj = root / "provider.o"
    run(cc, "-fPIC", "-c", src, "-o", obj)
    lib = root / "libprovider.so"
    run(ld, "--shared", "--soname=libprovider.so", "--hash-style=both", obj, "-o", lib)
    info = run("readelf", "-d", lib).stdout
    assert "SONAME" in info and "libprovider.so" in info and "HASH" in info
    src.write_text("extern int value(void); int main(void) { return value() != 42; }\n")
    exe = root / "consumer"
    run(cc, src, "-L" + tmp, "-lprovider", "-Wl,-rpath,$ORIGIN", "-o", exe)
    run(exe)

    # GCC/Clang compiler-driver integration: collect2/clang finds this ld and
    # passes startup files, libc's GROUP script, plugins and ordered arguments.
    tools = root / "tools"
    tools.mkdir()
    (tools / "ld").symlink_to(ld)
    driver = "-B" + str(tools) + "/"
    src.write_text(r'''
__thread int tls = 5;
static __thread int local_tls __attribute__((tls_model("local-dynamic"))) = 8;
__thread int initial_tls __attribute__((tls_model("initial-exec"))) = 9;
int answer = 40;
__attribute__((constructor)) static void init(void) { answer += 2; }
int value(void) { return answer; }
int *tls_address(void) { return &tls; }
int local_next(void) { return ++local_tls; }
int initial_next(void) { return ++initial_tls; }
__attribute__((weak)) int overridable(void) { return 4; }
int interposed(void) { return overridable(); }
__attribute__((visibility("hidden"))) int hidden_value(void) { return 99; }
__attribute__((visibility("protected"))) int protected_value(void) { return 17; }
static int implementation(void) { return 73; }
static int (*resolver(void))(void) { return implementation; }
int indirect(void) __attribute__((ifunc("resolver")));
int api_old(void) { return 11; }
int api_new(void) { return 22; }
__asm__(".symver api_old,api@V1");
__asm__(".symver api_new,api@@V2");
''')
    versions = root / "versions.ld"
    versions.write_text("V1 { global: api; }; V2 { global: *; local: api_old; api_new; } V1;\n")
    run(cc, driver, "-fPIC", "-shared", src, "-Wl,-soname,libprovider.so",
        "-Wl,--version-script=" + str(versions), "-Wl,--hash-style=both", "-o", lib)
    symbols = run("readelf", "--dyn-syms", "--wide", lib).stdout
    assert "api@@V2" in symbols and "api@V1" in symbols and "IFUNC" in symbols
    assert "PROTECTED" in symbols and "hidden_value" not in symbols and "api_old" not in symbols
    relocs = run("readelf", "-rW", lib).stdout
    assert "DTPMOD64" in relocs and "TPOFF64" in relocs, relocs

    # dlopen/dlclose callbacks verify constructors, destructors and host exports.
    plugin_src = root / "plugin.c"
    plugin_src.write_text('''extern void record(int);
__attribute__((constructor)) static void init(void) { record(1); }
__attribute__((destructor)) static void fini(void) { record(2); }
int plugin_value(void) { return 63; }
''')
    plugin = root / "plugin.so"
    run(cc, driver, "-shared", "-fPIC", plugin_src, "-o", plugin)
    main = root / "main.c"
    main.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
extern int answer, value(void), api(void), indirect(void), interposed(void);
extern int local_next(void), initial_next(void), *tls_address(void);
extern __thread int tls;
extern int old_api(void);
__asm__(".symver old_api,api@V1");
static __thread int executable_tls = 30;
static int events;
void record(int n) { events = events * 10 + n; }
int overridable(void) { return 91; }
static pthread_barrier_t barrier;
static void *addresses[2];
static void *worker(void *p) {
  long i = (long)p;
  int bad = tls != 5 || local_next() != 9 || initial_next() != 10 || executable_tls != 30;
  tls = 20 + i; executable_tls = 50 + i;
  addresses[i] = tls_address();
  pthread_barrier_wait(&barrier);
  return bad || tls != 20 + i || executable_tls != 50 + i ? (void *)1 : 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
  CHECK(argc == 2);
  CHECK(answer == 42 && value() == 42);
  answer = 44; CHECK(value() == 44);
  CHECK(api() == 22 && old_api() == 11 && indirect() == 73 && interposed() == 91);
  CHECK(tls == 5 && *tls_address() == 5);
  tls = 100; CHECK(local_next() == 9 && initial_next() == 10);
  pthread_t a, b; void *ra, *rb;
  CHECK(!pthread_barrier_init(&barrier, 0, 2));
  CHECK(!pthread_create(&a, 0, worker, (void *)0));
  CHECK(!pthread_create(&b, 0, worker, (void *)1));
  CHECK(!pthread_join(a, &ra) && !pthread_join(b, &rb) && !ra && !rb);
  CHECK(addresses[0] != addresses[1] && tls == 100 && executable_tls == 30);
  CHECK(!pthread_barrier_destroy(&barrier));
  void *h = dlopen("libprovider.so", RTLD_NOW); CHECK(h);
  int (*v1)(void) = (int (*)(void))dlvsym(h, "api", "V1");
  int (*v2)(void) = (int (*)(void))dlvsym(h, "api", "V2");
  CHECK(v1 && v2 && v1() == 11 && v2() == 22);
  CHECK(!dlsym(h, "hidden_value")); CHECK(!dlclose(h));
  h = dlopen(argv[1], RTLD_LAZY); CHECK(h && events == 1);
  int (*f)(void) = (int (*)(void))dlsym(h, "plugin_value"); CHECK(f && f() == 63);
  CHECK(!dlclose(h) && events == 12);
  return 0;
}
''')
    common = [driver, main, "-L" + tmp, "-lprovider", "-pthread", "-ldl",
              "-rdynamic", "-Wl,-rpath,$ORIGIN"]
    run(cc, *common, "-fPIC", "-pie", "-Wl,-z,now,-z,relro", "-o", exe)
    run(exe, plugin)
    headers = run("readelf", "-lW", exe).stdout
    tags = run("readelf", "-dW", exe).stdout
    assert "INTERP" in headers and "GNU_RELRO" in headers
    assert "RUNPATH" in tags and "NOW" in tags and "libprovider.so" in tags
    assert "V1" in run("readelf", "-VW", exe).stdout
    # Lazy PLT, eager binding, no-PLT calls, and ET_EXEC copy relocations.
    run(cc, *common, "-fPIC", "-pie", "-Wl,-z,lazy", "-o", exe)
    run(exe, plugin)
    run(exe, plugin, env={**os.environ, "LD_BIND_NOW": "1"})
    run(cc, *common, "-fPIC", "-fno-plt", "-pie", "-o", exe)
    run(exe, plugin)
    run(cc, *common, "-fno-pie", "-no-pie", "-Wl,--disable-new-dtags", "-o", exe)
    assert "COPY" in run("readelf", "-rW", exe).stdout
    assert "RPATH" in run("readelf", "-dW", exe).stdout
    run(exe, plugin)

    # Input order controls --as-needed. Both archives are required to resolve
    # a cyclic group, and --whole-archive must retain an unreferenced export.
    small = root / "small.c"
    small.write_text("int standalone(void) { return 7; }\n")
    run(cc, "-c", "-fPIC", small, "-o", obj)
    out = root / "ordered.so"
    run(ld, "--shared", "--as-needed", lib, obj, "-o", out)
    assert "NEEDED" not in run("readelf", "-dW", out).stdout
    run(ld, "--shared", "--no-as-needed", lib, obj, "-o", out)
    assert "libprovider.so" in run("readelf", "-dW", out).stdout
    for name, code in {"a": "extern int b(void); int a(void){return b();}",
                       "b": "extern int c(void); int b(void){return c();}",
                       "c": "int c(void){return 37;}"}.items():
        small.write_text(code)
        run(cc, "-c", "-fPIC", small, "-o", root / (name + ".o"))
    run("ar", "rcs", root / "liba.a", root / "a.o", root / "c.o")
    run("ar", "rcs", root / "libb.a", root / "b.o")
    run(ld, "--shared", "-u", "a", "-z", "defs", "--start-group", root / "liba.a",
        root / "libb.a", "--end-group", "-o", out)
    assert " c" in run("readelf", "--dyn-syms", out).stdout
    run(ld, "--shared", "--whole-archive", root / "liba.a", root / "libb.a",
        "--no-whole-archive", "-z", "defs", "-o", out)

    # C and C++ APIs: in-memory results, copied settings, per-invocation SDK.
    run(api, obj, ld)
    c_lib = root / "c-sdk.so"
    run(c_api, obj, c_lib)
    assert ctypes.CDLL(str(c_lib)).standalone() == 7
    # Lua's side effects happen once; INSERT retains the toolchain's dynamic
    # layout (including PT_DYNAMIC/TLS/RELRO) and supports full GNU expressions.
    count = root / "lua-count"
    lua = root / "layout.lua"
    lua.write_text(f'''local f = assert(io.open({str(count)!r}, 'a')); f:write('x'); f:close()
qld.emit('SECTIONS {{ .sdk : {{ LONG(42) }} }} INSERT AFTER .data;')
''')
    run(ld, "--shared", "-T", lua, obj, "-o", out)
    assert count.read_text() == "x"
    assert "2a000000" in run("readelf", "-x", ".sdk", out).stdout

    # Response files, nested quoting, in-place option order and literal paths.
    strange = root / "space ' ; $file.so"
    nested = root / "nested.rsp"
    nested.write_text('"' + str(obj) + '" -o "' + str(strange) + '"')
    response = root / "link.rsp"
    response.write_text('--shared --soname=response.so @"' + str(nested) + '"')
    run(ld, "@" + str(response))
    assert "response.so" in run("readelf", "-d", strange).stdout

    def fail(*args, contains):
        out.write_bytes(b"preserve-existing-output")
        p = subprocess.run([ld, *map(str, args), "-o", str(out)], capture_output=True, text=True)
        assert p.returncode and contains in p.stderr, (args, p.returncode, p.stderr)
        assert out.read_bytes() == b"preserve-existing-output"

    fail("--shared", "-z", "defs", root / "a.o", contains="undefined reference")
    fail("--shared", "--elf-linker=/nonexistent/qld-linker", obj, contains="cannot execute")
    fail("--shared", "--elf-linker=" + ld, obj, contains="recursive")
    fail("--shared", "--version-script=" + str(root / "absent"), obj, contains="absent")
    fail("--dynamic", "-m", "nonexistent-emulation", obj, contains="emulation")
    fail("--shared", "--oformat=binary", obj, contains="no valid ELF")
    response.write_text('@"' + str(response) + '"')
    fail("@" + str(response), contains="response file cycle")
    response.write_text('"unterminated')
    fail("@" + str(response), contains="unterminated quote")
    fail("--shared", obj, "-T", root / "missing.lua", contains="cannot read")
    lua.write_text('error("intentional-lua-failure")')
    fail("--shared", obj, "-T", lua, contains="intentional-lua-failure")
    fake = root / "backend with spaces"
    fake.write_text(f"#!{sys.executable}\nimport os, signal\nos.kill(os.getpid(), signal.SIGTERM)\n")
    fake.chmod(0o755)
    fail("--shared", "--elf-linker=" + str(fake), obj, contains="signal 15")
    fake.write_text(f"#!{sys.executable}\nprint('backend-returned-no-image')\n")
    fail("--shared", "--elf-linker=" + str(fake), obj, contains="no valid ELF image")
    assert not list(root.glob("qobjld-*")), "backend workspace leaked"

print("dynamic ELF loader regressions passed")
