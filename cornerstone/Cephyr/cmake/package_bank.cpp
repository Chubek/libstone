// Build only the assembler records of the reviewed SymAS input. The full
// SDK additionally reads backend metadata, unnecessary for ELF assembly.
#include <domsymascc/records.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <cstdio>
#include <memory>

int main(int argc, char** argv) try {
    if (argc != 3) return 2;
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
    auto* l = state.get();
    if (!l) return 1;
    luaL_openlibs(l);
    // This packaging tool runs in the reviewed stdspec directory. Keep its
    // module search local and collect the emitted document in the Lua state.
    if (luaL_dostring(l, "package.path='./?.lua'; package.cpath=''; package.loaded.backend=require('dsl');"
                          "function print(s) assert(not output); output=s end") || luaL_dofile(l,argv[1]))
        throw std::runtime_error(lua_tostring(l,-1));
    lua_getglobal(l,"output");
    size_t n = 0;
    const char* text = lua_tolstring(l,-1,&n);
    if (!text) throw std::runtime_error("spec did not emit assembler records");
    domsymas::Builder builder;
    builder.set_compression(domsymas::Compression::None);
    auto result = domsymas::cc::compile_assembler_records(std::string(text,n), builder);
    if (!result.ok) throw std::runtime_error(result.error);
    if (auto error = builder.save_file(argv[2]); !error.ok()) throw std::runtime_error(error.message);
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr,"package bank: %s\n",e.what()); return 1; }
