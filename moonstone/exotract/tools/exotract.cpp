#include <exotract/Exotract.hpp>
#include <iostream>
#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#if defined(EXOTRACT_WITH_LUA)
namespace moonstone::exotract { int run_lua_file(const std::string &, const std::string &,
                                                   const std::vector<std::string> &); }
#endif
using namespace moonstone::exotract;
namespace {
template<class T> T integer(const std::string &value) {
    T result{};
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result);
    if(error!=std::errc{} || end!=value.data()+value.size())throw std::invalid_argument("invalid integer: "+value);
    return result;
}
void help() {
    std::cout << "exotract <inspect|emit-source|on1x|validate|lua> [flags]\n"
       "  --name NAME --header FILE --include DIR --define MACRO --undefine MACRO\n"
       "  --impl-define MACRO --dependency HEADER --require-symbol NAME --exoml-bind EXE\n"
       "  --resource-dir DIR --cdecl-json FILE --contract FILE --out FILE\n"
       "  --cache-dir DIR --compiler EXE --max-bytes N --timeout-ms N\n"
       "  --preprocessor builtin|compiler --parser builtin|compiler|external\n"
       "  --target TRIPLE --revision REV --compile-arg FLAG --provenance FILE\n"
       "  lua FILE.lua [--cache-dir DIR] [--global NAME=VALUE]\n"
       "When ExoML extraction is unavailable, inspect validates syntax but marks\n"
       "the declaration graph incomplete (no trusted bindings).\n";
}
}
int main(int argc, char **argv) {
    if(argc<2 || std::string_view(argv[1])=="--help"){help();return argc<2?2:0;}
    try {
        const std::string op=argv[1];
        if(op=="lua") {
            if(argc<3)throw std::invalid_argument("lua requires FILE.lua");
            std::string dir;std::vector<std::string> globals;
            for(int i=3;i<argc;i++){
                const std::string f=argv[i];
                if(i+1>=argc)throw std::invalid_argument("missing value after "+f);
                const std::string v=argv[++i];
                if(f=="--cache-dir")dir=v;
                else if(f=="--global")globals.push_back(v);
                else throw std::invalid_argument("unknown lua flag: "+f);
            }
#if defined(EXOTRACT_WITH_LUA)
            return run_lua_file(argv[2],dir,globals);
#else
            (void)dir;(void)globals;
            throw std::runtime_error("Lua support not built: configure with vendored Lua/Kaguya");
#endif
        }
        Config cfg;std::string output,cdecl_path,contract_path;
        for(int i=2;i<argc;i++){
            std::string f=argv[i];
            if(i+1>=argc)throw std::invalid_argument("missing value after "+f);
            std::string v=argv[++i];
            if(f=="--name")cfg.name=v;
            else if(f=="--header")cfg.path=v;
            else if(f=="--include")cfg.include_directories.emplace_back(v);
            else if(f=="--define")cfg.defines.push_back(v);
            else if(f=="--undefine")cfg.undefines.push_back(v);
            else if(f=="--impl-define")cfg.header_only.implementation_defines.push_back(v);
            else if(f=="--dependency")cfg.header_only.dependencies.push_back(v);
            else if(f=="--require-symbol")cfg.required_symbols.push_back(v);
            else if(f=="--exoml-bind")cfg.exoml_bind=v;
            else if(f=="--target")cfg.target=v;
            else if(f=="--revision")cfg.revision=v;
            else if(f=="--compile-arg")cfg.compiler_arguments.push_back(v);
            else if(f=="--provenance")cfg.provenance_files.emplace_back(v);
            else if(f=="--resource-dir")cfg.clang_resource_dir=v;
            else if(f=="--cdecl-json")cdecl_path=v;
            else if(f=="--contract")contract_path=v;
            else if(f=="--compiler")cfg.compiler=v;
            else if(f=="--preprocessor")cfg.preprocessor.main=v;
            else if(f=="--parser")cfg.parser.main=v;
            else if(f=="--out")output=v;
            else if(f=="--cache-dir"){cfg.cache_dir=v;cfg.cache=true;}
            else if(f=="--max-bytes")cfg.max_output_bytes=integer<std::size_t>(v);
            else if(f=="--timeout-ms")cfg.command_timeout_ms=integer<unsigned>(v);
            else throw std::invalid_argument("unknown flag: "+f);
        }
        if(cfg.name.empty())cfg.name=cfg.path.stem().string();
        Engine engine(cfg);
        if(op=="emit-source"){
            auto s=engine.implementation_source();
            if(output.empty())std::cout<<s;else write_file_atomic(output,s);
            return 0;
        }
        auto r=engine.inspect();
        if(!cdecl_path.empty() && r.syntax_checked)r=engine.import_cdecl(std::move(r),read_file(cdecl_path));
        std::string content;
        if(op=="inspect")content=engine.manifest_json(r);
        else if(op=="on1x")content=engine.on1x_manifest_json(r);
        else if(op=="validate"){
            if(contract_path.empty())throw std::invalid_argument("validate requires --contract");
            content=engine.validate_contract(r,contract_path);
        } else throw std::invalid_argument("unknown operation: "+op);
        const bool failed=std::any_of(r.diagnostics.begin(),r.diagnostics.end(),[](const Diagnostic &d){return d.severity==Diagnostic::Severity::Error;});
        const bool success=!failed && ((op=="inspect" && r.syntax_checked) || r.ok());
        if(output.empty())std::cout<<content;
        else if(success)write_file_atomic(output,content);
        if(!std::cout)throw std::runtime_error("output write failed");
        return success?0:3;
    }catch(const std::exception &e){std::cerr<<"exotract: "<<e.what()<<'\n';return 2;}
}
