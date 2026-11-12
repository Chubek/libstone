/** @file test_binary_hardening.cpp @brief ELF layout, relocation and native-payload regressions. */
#include "Jiterati-Native.hpp"
#include "Jiterati-Bytecode.hpp"
#include "src/Elf.hpp"
#include "src/TemporaryDirectory.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#if defined(JITERATI_BINUTILS_REFERENCE)
#include "ansidecl.h"
#include "elf/external.h"
static_assert(sizeof(Elf32_External_Ehdr) == 52 && sizeof(Elf64_External_Ehdr) == 64);
static_assert(offsetof(Elf64_External_Ehdr, e_shoff) == 40);
static_assert(offsetof(Elf64_External_Ehdr, e_ehsize) == 52);
static_assert(sizeof(Elf32_External_Phdr) == 32 && sizeof(Elf64_External_Phdr) == 56);
static_assert(offsetof(Elf32_External_Phdr, p_flags) == 24);
static_assert(offsetof(Elf64_External_Phdr, p_flags) == 4);
static_assert(sizeof(Elf32_External_Shdr) == 40 && sizeof(Elf64_External_Shdr) == 64);
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
using Bytes = std::vector<std::uint8_t>;
template<class F> void rejects(F f) {
    bool threw = false;
    try { f(); } catch (std::exception const&) { threw = true; }
    CHECK(threw);
}
static void put(Bytes& bytes, std::size_t off, std::uint64_t value, unsigned width, bool little) {
    CHECK(off <= bytes.size() && width <= bytes.size() - off);
    for (unsigned i = 0; i < width; ++i)
        bytes[off+i] = static_cast<std::uint8_t>(value >> (8 * (little ? i : width-1-i)));
}

static Bytes object(bool wide, bool little, bool extended = false, unsigned count = 3) {
    unsigned eh = wide ? 64 : 52, sh = wide ? 64 : 40;
    auto payload = eh + count * sh;
    std::string names("\0.text\0.shstrtab\0",17);
    Bytes bytes(payload + 2 + names.size());
    bytes[0]=0x7f; bytes[1]='E'; bytes[2]='L'; bytes[3]='F';
    bytes[4]=wide?2:1; bytes[5]=little?1:2; bytes[6]=1;
    auto p = [&](std::size_t off, std::uint64_t value, unsigned size) { put(bytes,off,value,size,little); };
    p(16,1,2); p(18,8,2); p(20,1,4); // MIPS ET_REL
    p(wide?40:32,eh,wide?8:4); p(wide?52:40,eh,2);
    p(wide?58:46,sh,2); p(wide?60:48,extended?0:count,2); p(wide?62:50,extended?0xffff:2,2);
    if (extended) { p(eh+(wide?32:20),count,wide?8:4); p(eh+(wide?40:24),2,4); }
    auto text = eh + sh, strings = eh + sh*2;
    p(text,1,4); p(text+4,1,4); p(text+8,6,wide?8:4);
    p(text+(wide?24:16),payload,wide?8:4); p(text+(wide?32:20),2,wide?8:4);
    p(strings,7,4); p(strings+4,3,4); p(strings+(wide?24:16),payload+2,wide?8:4);
    p(strings+(wide?32:20),names.size(),wide?8:4);
    bytes[payload]=0x90; bytes[payload+1]=0xc3;
    std::copy(names.begin(),names.end(),bytes.begin()+payload+2);
    return bytes;
}

static void readers() {
    for (bool wide : {false,true}) for (bool little : {false,true}) for (bool extended : {false,true}) {
        unsigned eh=wide?64:52, sh=wide?64:40;
        auto bytes=object(wide,little,extended);
        CHECK(jiterati::native::extract_text_section(bytes)==Bytes({0x90,0xc3}));
        for (std::size_t i=0; i<bytes.size(); ++i)
            CHECK(jiterati::native::extract_text_section(Bytes(bytes.begin(),bytes.begin()+i)).empty());
        auto bad=bytes;
        put(bad,wide?62:50,3,2,little);
        CHECK(jiterati::native::extract_text_section(bad).empty());
        bad=bytes;
        put(bad,eh+sh+(wide?24:16),wide?UINT64_MAX-1:UINT32_MAX-1,wide?8:4,little);
        CHECK(jiterati::native::extract_text_section(bad).empty());
        bad=bytes; put(bad,eh+sh+4,8,4,little); // SHT_NOBITS is not file code
        CHECK(jiterati::native::extract_text_section(bad).empty());
        bad=bytes; put(bad,eh+sh+8,0x806,wide?8:4,little); // SHF_COMPRESSED
        CHECK(jiterati::native::extract_text_section(bad).empty());
        bad=bytes;
        std::fill(bad.begin()+eh+3*sh+2,bad.end(),'x');
        CHECK(jiterati::native::extract_text_section(bad).empty());
        CHECK(jiterati::native::extract_text_section(object(wide,little,true,300))==Bytes({0x90,0xc3}));
        bad=object(wide,little,true);
        put(bad,eh+(wide?32:20),wide?UINT64_MAX:UINT32_MAX,wide?8:4,little);
        CHECK(jiterati::native::extract_text_section(bad).empty());
    }
}

static void writers() {
    CHECK(jiterati::asmlink::ks_mode_for_target("arm32") == 1);
    CHECK(jiterati::asmlink::ks_mode_for_target("x86") == 4);
    CHECK(jiterati::asmlink::ks_arch_for_target("i386") == jiterati::asmlink::KsArch::X86);
    CHECK(jiterati::asmlink::ks_mode_for_target("ppc32") == ((1<<30)|4));
    CHECK(jiterati::asmlink::ks_mode_for_target("sparc64") == ((1<<30)|8));
    CHECK(jiterati::asmlink::ks_mode_for_target("s390x") == (1<<30));
    struct Target { char const* name; bool wide,little; unsigned machine; };
    for (auto target : {Target{"amd64",true,true,62}, {"x86",false,true,3}, {"arm32",false,true,40},
                        {"mips32",false,true,8}, {"mips64",true,true,8}, {"ppc32",false,false,20},
                        {"ppc64",true,false,21}, {"sparc",false,false,2}, {"sparc64",true,false,43},
                        {"s390x",true,false,22}, {"aarch64",true,true,183}, {"rv32",false,true,243},
                        {"rv64",true,true,243}, {"loongarch64",true,true,258}}) {
        auto bytes=jiterati::asmlink::link_elf(target.name,{0x90,0xc3});
        jiterati::native::detail::ElfImage elf(bytes);
        CHECK(elf.type==2 && elf.machine==target.machine && elf.is64==target.wide && elf.little==target.little);
        CHECK(elf.get(24,target.wide?8:4)==0x401000);
        auto ph=target.wide?64u:52u;
        CHECK(elf.get(target.wide?32:28,target.wide?8:4)==ph);
        CHECK(elf.get(ph+(target.wide?4:24),4)==5);
        CHECK(elf.get(ph+(target.wide?8:4),target.wide?8:4)==0x1000);
        CHECK(bytes.size()==4098 && bytes[4096]==0x90);
    }
    rejects([] { jiterati::asmlink::link_elf("wasm",{1}); });
    rejects([] { jiterati::asmlink::link_elf("typo",{1}); });
    rejects([] { jiterati::asmlink::link_elf("amd64",{}); });
    rejects([] { jiterati::asmlink::link_elf("amd64",{1},1); });
    rejects([] { jiterati::asmlink::link_elf("amd64",{1},UINT64_MAX-0xfff); });
    rejects([] { jiterati::asmlink::link_elf("arm32",{1},UINT64_C(0xfffff000)); });
}

static void linking() {
#if defined(__linux__) && defined(__x86_64__)
    using namespace jiterati;
    if (!native::have_system_toolchain()) return;
    auto report=native::assemble_text("amd64",
        "global _start\n_start:\n mov edi, DWORD PTR [rip+status]\n mov eax,60\n syscall\n.data\nstatus: .long 42\n");
    CHECK(report.ok && report.object_is_elf);
    CHECK(native::extract_text_section(report.object_bytes).empty()); // unapplied R_X86_64_PC32
    auto image=native::link_executable("amd64",report.object_bytes,true);
    jiterati::detail::TemporaryDirectory temporary;
    auto path=temporary.file("exit42"); native::write_file(path,image,true);
    CHECK(native::run_file(path).exit_code==42);
    auto readelf=native::run_file("readelf",{"-h","-l",path});
    if (readelf.exit_code!=127) { CHECK(readelf.exit_code==0); CHECK(readelf.output.find("EXEC")!=std::string::npos); }
    auto objdump=native::run_file("objdump",{"-d",path});
    if (objdump.exit_code!=127) { CHECK(objdump.exit_code==0); CHECK(objdump.output.find("syscall")!=std::string::npos); }
    rejects([&] { native::link_executable("mips32",report.object_bytes,true); });
    rejects([] { native::link_executable("amd64",{0x7f,'E','L','F'},true); });
    report=native::assemble_text("amd64","global answer\nanswer:\n ret\n");
    CHECK(report.ok); rejects([&] { native::link_executable("amd64",report.object_bytes,true); });
    report=native::assemble_text("amd64","global _start\n_start:\n call missing\n");
    CHECK(report.ok); rejects([&] { native::link_executable("amd64",report.object_bytes,true); });
    Bytes start={0xb8,60,0,0,0,0xbf,42,0,0,0,0x0f,0x05};
    native::write_file(path,asmlink::link_elf("amd64",start),true);
    CHECK(native::run_file(path).exit_code==42);
#endif
}

static void records() {
    using namespace jiterati::bytecode;
    BytecodeFunction fn;
    fn.name="entry"; fn.target="amd64"; fn.ir_text="ir\n"; fn.code={0xcc,0xb8,42,0,0,0,0xc3}; fn.entry_offset=1;
    auto record=decode_records(encode_records({fn}));
    CHECK(record.size()==1 && record[0].code==fn.code && record[0].entry_offset==1);
    CHECK(decode_records("old\namd64\n0 1\nx\n")[0].entry_offset==0);
    for (auto bad : {"x", "x\ny\n", "x\ny\n-1 1\nx\n", "x\ny\n0 18446744073709551615\n",
                     "x\ny\n18446744073709551615 2\nx\n", "x\ny\n0 1\nx!", "x\ny\n0 1 1\nx\n"})
        rejects([&] { decode_records(bad); });
    auto encoded=encode_records({fn});
    for (std::size_t n=1;n<encoded.size();++n) rejects([&] { decode_records(encoded.substr(0,n)); });
    rejects([&] { decode_records(encoded+"truncated"); });
    BytecodeModule image; image.target="amd64"; image.entry="entry"; image.functions={fn};
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(execute(image,"entry")==42); // skips the deliberate INT3 prefix
#endif
    image.functions[0].entry_offset=image.functions[0].code.size();
    rejects([&] { execute(image,"entry"); });
    image.functions[0].entry_offset=0; image.target="foreign";
    rejects([&] { execute(image,"entry"); });
    image.target="amd64"; image.functions[0].target="foreign";
    rejects([&] { execute(image,"entry"); });
}

int main() {
    readers(); writers(); linking(); records();
    std::puts("binary hardening passed");
}
