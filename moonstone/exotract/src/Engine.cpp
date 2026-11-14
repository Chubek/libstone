#include <exotract/Exotract.hpp>
#include "Json.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#include <sstream>
#include <set>
#if defined(EXOTRACT_WITH_EXOML)
#include <exoml/Extract.hpp>
#include <exoml/CDecl.hpp>
#include <exoml/Contract.hpp>
#endif
#ifndef EXOTRACT_UCPP_EXECUTABLE
#define EXOTRACT_UCPP_EXECUTABLE ""
#endif
namespace moonstone::exotract {
namespace {
std::string arr(const std::vector<std::string> &v) {
    std::string s="[";for(auto &x:v){if(s.size()>1)s+=',';s+=json_quote(x);}return s+"]";
}
std::string hex_hash(std::string_view text) {
    // Fast deterministic content fingerprint, not a cryptographic attestation.
    std::uint64_t hash = 14695981039346656037ull;
    for(unsigned char c:text) {hash^=c;hash*=1099511628211ull;}
    std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return s.str();
}
std::string file_include_path(const fs::path &path) {
    const auto p=path.string();
    if (p.find_first_of("\n\r\0\"", 0, 4) != std::string::npos)
        throw std::invalid_argument("unsafe header path");
    if(p.find('\\')!=std::string::npos) throw std::invalid_argument("backslash in include path");
    return p;
}
std::string profile_args(const Config &c) {
    // Length framing avoids ambiguous concatenations of user-controlled fields.
    std::string s;
    auto add=[&](const std::string &v){s+=std::to_string(v.size())+":"+v;};
    for(const auto &v:{c.cstd,c.compiler,c.exoml_bind,c.clang_resource_dir,c.target,c.revision})add(v);
    for(const auto &x:c.include_directories)add("I"+x.string());
    for(const auto &x:c.defines)add("D"+x);
    for(const auto &x:c.undefines)add("U"+x);
    for(const auto &x:c.required_symbols)add("R"+x);
    for(const auto &x:c.compiler_arguments)add("A"+x);
    for(const auto &x:c.provenance_files)add("P"+x.string());
    return s;
}
std::vector<std::string> cc_args(const Config &c) {
    std::vector<std::string> args={"-std=c11"};
    if(!c.target.empty())args.push_back("--target="+c.target);
    args.insert(args.end(),c.compiler_arguments.begin(),c.compiler_arguments.end());
    for(auto &x:c.include_directories){args.push_back("-I");args.push_back(x.string());}
    for(auto &x:c.defines)args.push_back("-D"+x);
    for(auto &x:c.undefines)args.push_back("-U"+x);
    return args;
}
std::string result_key(const Config &c,const Result &r) {
    return "cdecl/"+hex_hash("Exotract-v2\n"+c.path.string()+"\n"+r.preprocessed+"\n"+r.fingerprint+"\n"+profile_args(c)+"\n"+r.cdecl_json);
}
void push_error(Result &r,std::string code,std::string msg) {
    r.diagnostics.push_back({Diagnostic::Severity::Error,std::move(code),std::move(msg)});
}
std::string reason(const CommandResult &r) {
    if(r.timed_out)return "timed out";
    if(r.overflow)return "output budget exceeded";
    return "exit " + std::to_string(r.exit_code) + ": " + r.err.substr(0,8192);
}
#if !defined(__unix__) && !defined(__APPLE__)
std::atomic<unsigned> temp_serial{0};
#endif
struct TemporaryFile {
    fs::path path;
    explicit TemporaryFile(std::string_view suffix) {
#if defined(__unix__) || defined(__APPLE__)
        // mkstemp reserves a private file atomically; .suffix is metadata only.
        (void)suffix;
        std::string pattern=(fs::temp_directory_path()/"exotract-XXXXXXXX").string();
        std::vector<char> buffer(pattern.begin(),pattern.end());
        buffer.push_back('\0');
        const int fd=::mkstemp(buffer.data());
        if(fd<0)throw std::runtime_error("cannot create secure temporary file");
        ::close(fd);
        path=buffer.data();
#else
        path=fs::temp_directory_path()/("exotract-"+
              std::to_string(temp_serial.fetch_add(1))+std::string(suffix));
#endif
    }
    ~TemporaryFile(){std::error_code error;fs::remove(path,error);}
};
void ingest_cdecl(Result &r,std::string text) {
    r.declarations_complete=false;
    r.symbols.clear();
    r.cdecl_json.clear();
    try {
        const auto root=detail::JsonParser(text).parse();
        using J=detail::Json;
        const auto &version=root.at("schema_version");
        if(version.type!=J::Type::Number || version.value!="2")
            throw std::runtime_error("unsupported CDecl schema, expected integer 2");
        if(root.at("extractor_version").str()!="exoml-c11-3")
            throw std::runtime_error("unsupported ExoML extractor version");
        if(root.at("fingerprint").str().empty())throw std::runtime_error("CDecl missing source fingerprint");
        const auto &target=root.at("target");
        if(target.at("compiler").str().empty() || target.at("triple").str().empty())
            throw std::runtime_error("CDecl missing compiler/target identity");
        const auto &bits=target.at("pointer_bits");
        if(bits.type!=J::Type::Number || (bits.value!="16" && bits.value!="32" && bits.value!="64" && bits.value!="128"))
            throw std::runtime_error("invalid target pointer width");
        auto array=[](const J &value)->const std::vector<J>& {
            if(value.type!=J::Type::Array)throw std::runtime_error("expected CDecl array");
            return value.array;
        };
        const auto &types=array(root.at("types"));
        std::map<std::string,const J*> ids;
        const std::set<std::string> kinds={"unknown","void","integer","floating","pointer","array","function","typedef","record","enum","elaborated"};
        for(const auto &type:types){
            const auto id=type.at("id").str();
            if(id.empty() || !ids.emplace(id,&type).second)throw std::runtime_error("empty or duplicate type id");
            if(!kinds.contains(type.at("kind").str()))throw std::runtime_error("unknown CDecl type kind");
            (void)type.at("spelling").str();
        }
        auto ref=[&](const J &value,bool optional=false){
            const auto id=value.str();
            if(optional && id.empty())return;
            if(!ids.contains(id))throw std::runtime_error("unresolved ExoML type id: "+id);
        };
        for(const auto &type:types){
            ref(type.at("canonical_id"));
            ref(type.at("element"),true);ref(type.at("result"),true);
            for(const auto &arg:array(type.at("arguments")))ref(arg);
            for(const auto &field:array(type.at("fields")))ref(field.at("type"));
            const auto kind=type.at("kind").str();
            if(kind=="function")ref(type.at("result"));
            if(kind=="pointer" || kind=="array" || kind=="typedef" || kind=="elaborated")ref(type.at("element"));
        }
        std::map<std::string,std::pair<std::string,std::string>> declarations;
        for(const auto &decl:array(root.at("declarations"))){
            const auto id=decl.at("id").str(), kind=decl.at("kind").str();
            if(id.empty())throw std::runtime_error("empty declaration id");
            const auto signature=std::make_pair(kind,decl.at("type").str());
            auto [previous,inserted]=declarations.emplace(id,signature);
            if(!inserted && previous->second!=signature)throw std::runtime_error("conflicting declaration id");
            if(kind!="function" && kind!="typedef" && kind!="record" && kind!="enum" && kind!="variable" && kind!="macro" && kind!="unsupported")
                throw std::runtime_error("unknown CDecl declaration kind");
            ref(decl.at("type"),kind=="macro" || kind=="unsupported");
            const auto &parameters=array(decl.at("parameters"));
            for(const auto &param:parameters)ref(param.at("type"));
            if(kind=="function"){
                const auto &type=*ids.at(decl.at("type").str());
                if(type.at("kind").str()!="function")throw std::runtime_error("function declaration has non-function type");
                const auto &args=array(type.at("arguments"));
                if(args.size()!=parameters.size())throw std::runtime_error("function parameter count mismatch");
                for(std::size_t i=0;i<args.size();++i)
                    if(args[i].str()!=parameters[i].at("type").str())throw std::runtime_error("function parameter type mismatch");
            }
        }
        for(const auto &diag:array(root.at("diagnostics"))){
            const auto severity=diag.at("severity").str();
            if(severity!="error" && severity!="warning" && severity!="note" && severity!="fatal")
                throw std::runtime_error("unknown diagnostic severity");
            r.diagnostics.push_back({severity=="error" || severity=="fatal" ? Diagnostic::Severity::Error :
                severity=="warning" ? Diagnostic::Severity::Warning : Diagnostic::Severity::Note,
                diag.at("id").str(),diag.at("message").str()});
        }
        if(!root.at("complete").boolean()) {
            push_error(r,"EXOTRACT-CDECL-PARTIAL","ExoML returned incomplete declarations; refusing bindings");
            return;
        }
        auto symbols=detail::symbols_from_cdecl(root);
        r.symbols=std::move(symbols);
        r.cdecl_json=std::move(text);
        r.declarations_complete=true;
    } catch(const std::exception &e) {
        r.symbols.clear();r.cdecl_json.clear();r.declarations_complete=false;
        push_error(r,"EXOTRACT-CDECL-INVALID",e.what());
    }

}
}
bool Result::ok() const {
    return preprocessed_ok && syntax_checked && declarations_complete &&
           std::none_of(diagnostics.begin(),diagnostics.end(),[](const auto &d){return d.severity==Diagnostic::Severity::Error;});
}
Engine::Engine(Config config,std::shared_ptr<Store> store,std::shared_ptr<GlobalRegistry> globals)
    :config_(std::move(config)),store_(std::move(store)),globals_(std::move(globals)){

    if (!store_) store_ = std::make_shared<Store>(config_.cache ? config_.cache_dir : fs::path{});
    if (!globals_) globals_=std::make_shared<GlobalRegistry>(store_);
    // Resolve configuration values as whole values via the host-owned registry,
    // never as shell snippets. The header path itself is resolved on use.
    auto expand=[&](std::string &s){s=globals_->expand(s);};
    if(config_.path.empty())throw std::invalid_argument("missing header path");
    config_.path=fs::absolute(globals_->expand(config_.path.string())).lexically_normal();
    expand(config_.documentation);expand(config_.exoml_bind);
    expand(config_.clang_resource_dir);expand(config_.compiler);
    expand(config_.target);expand(config_.revision);
    for(auto &p:config_.provenance_files)p=globals_->expand(p.string());
    for(auto &p:config_.include_directories)p=globals_->expand(p.string());
    for(auto &s:config_.defines)expand(s);
    for(auto &s:config_.undefines)expand(s);
    for(auto &s:config_.header_only.dependencies)expand(s);
    for(auto &s:config_.required_symbols)expand(s);
    validate_config(config_);
    if (config_.cache && !store_->persistent())
        throw std::invalid_argument("cache=true requires a persistent LMDB Store and cache_dir");
}
std::string Engine::expanded_path() const {
    return config_.path.string();
}
Result Engine::preprocess() const {
    Result r;
    const auto header=expanded_path();
    try { (void)read_file(header,config_.max_output_bytes); }
    catch(const std::exception &e){push_error(r,"EXOTRACT-HEADER",e.what());return r;}
    auto args=cc_args(config_);
    const auto version=run_command(Command{{config_.compiler,"--version"}},config_);
    const auto machine=run_command(Command{{config_.compiler,"-dumpmachine"}},config_);
    if(!version.ok() || !machine.ok()){
        push_error(r,"EXOTRACT-COMPILER-IDENTITY",reason(!version.ok()?version:machine));return r;
    }
    r.compiler_identity=version.out+machine.out;
    const std::string vendor_ucpp=EXOTRACT_UCPP_EXECUTABLE;
    std::vector<Command> candidates;
    if(config_.preprocessor.main=="builtin"){
        if(!vendor_ucpp.empty()){
            Command u;u.argv={vendor_ucpp};
            for(auto &x:config_.include_directories)u.argv.push_back("-I"+x.string());
            for(auto &x:config_.defines)u.argv.push_back("-D"+x);
            for(auto &x:config_.undefines)u.argv.push_back("-U"+x);
            u.argv.push_back(header);candidates.push_back(std::move(u));
        }
        // C11 compiler is authoritative when the C99-only ucpp cannot process
        // a C11 profile. Do not silently recover syntax errors.
        Command cpp;cpp.argv={config_.compiler,"-E","-P","-x","c"};
        cpp.argv.insert(cpp.argv.end(),args.begin(),args.end());cpp.argv.push_back(header);
        candidates.push_back(std::move(cpp));
    } else if(config_.preprocessor.main=="compiler"){
        Command cpp;cpp.argv={config_.compiler,"-E","-P","-x","c"};
        cpp.argv.insert(cpp.argv.end(),args.begin(),args.end());cpp.argv.push_back(header);
        candidates.push_back(std::move(cpp));
    } else {
        push_error(r,"EXOTRACT-BACKEND","unknown preprocessor: "+config_.preprocessor.main);return r;
    }
    for(auto c:config_.preprocessor.fallback)candidates.push_back(std::move(c));
    for(std::size_t i=0;i<candidates.size();++i){
        auto cr=run_command(candidates[i],config_,{{"%file",header}});
        const bool optional_ucpp=(i==0&&!vendor_ucpp.empty()&&config_.preprocessor.main=="builtin");
        if(optional_ucpp && !cr.timed_out && !cr.overflow){
            r.diagnostics.push_back({cr.ok()?Diagnostic::Severity::Note:Diagnostic::Severity::Warning,
                cr.ok()?"EXOTRACT-UCPP-PROBE":"EXOTRACT-UCPP-PARTIAL",
                cr.ok()?"ucpp probe succeeded; C11 compiler supplies authoritative preprocessing":reason(cr)});
            continue;
        }
        if(cr.ok()){
            r.preprocessed=std::move(cr.out);
            r.preprocessed_ok=true;
            r.preprocessor_used=candidates[i].argv[0];
            r.fingerprint=hex_hash(r.preprocessed+profile_args(config_)+r.compiler_identity);
            break;
        }
        // Fallback only on unavailable backend, never on parse errors, resource
        // failures or compiler failures. ucpp is an optional C99 probe.
        const bool unavailable=cr.err.starts_with("spawn failed:") || cr.exit_code==127;
        if((unavailable||optional_ucpp)&&!cr.timed_out&&!cr.overflow&&i+1<candidates.size())continue;
        push_error(r,"EXOTRACT-PREPROCESS",reason(cr));break;
    }
    return r;
}
Result Engine::parse(Result r) const {
    r=preprocess();
    if(!r.preprocessed_ok)return r;
    const auto header=expanded_path();
    auto args=cc_args(config_);
    Command syntax;syntax.argv={config_.compiler,"-fsyntax-only","-x","c"};
    syntax.argv.insert(syntax.argv.end(),args.begin(),args.end());syntax.argv.push_back(header);
    auto check=run_command(syntax,config_);
    if(!check.ok()) {push_error(r,"EXOTRACT-SYNTAX",reason(check));return r;}
    r.syntax_checked=true;
    const std::string probe_binary =
#ifdef EXOTRACT_CPARSER_EXECUTABLE
        EXOTRACT_CPARSER_EXECUTABLE;
#else
        "";
#endif
    if(!probe_binary.empty() && config_.parser.main=="builtin"){
        TemporaryFile preprocessed(".i"), normalized(".c");
        write_file_atomic(preprocessed.path,r.preprocessed);
        const auto probe=run_command(Command{{probe_binary,preprocessed.path.string(),normalized.path.string()}},config_);
        if(!probe.ok())r.diagnostics.push_back({Diagnostic::Severity::Warning,
            "EXOTRACT-CPARSER-PARTIAL","vendored CParser could not parse this preprocessed C11 profile: "+
                reason(probe)+probe.out.substr(0,8192)});
        else r.diagnostics.push_back({Diagnostic::Severity::Note,"EXOTRACT-CPARSER-PROBE",
            "CParser compatibility probe succeeded; ExoML still supplies typed declarations"});
        // CParser is a secondary diagnostic tool. Its AST does not supply ABI
        // authority and its incomplete C11 support cannot invalidate compiler
        // verified declarations.
    }
    if(config_.parser.main!="builtin" && config_.parser.main!="compiler" &&
       config_.parser.main!="external"){
        push_error(r,"EXOTRACT-PARSER","unsupported parser configuration: "+config_.parser.main);
        return r;
    }
    std::string json;
#if defined(EXOTRACT_WITH_EXOML)
    if(config_.parser.main=="builtin"){
        try {
            exoml::cdecl::ExtractOptions opts;
            opts.header=header;
            opts.target=config_.target;opts.revision=config_.revision;
            opts.compiler_arguments=config_.compiler_arguments;
            for(const auto &path:config_.provenance_files)opts.provenance_files.push_back(path.string());
            opts.resource_directory=config_.clang_resource_dir;
            for(const auto &dir:config_.include_directories)opts.include_directories.push_back(dir.string());
            opts.defines=config_.defines;opts.undefines=config_.undefines;
            opts.limits.input_bytes=config_.max_output_bytes;
            const auto extracted=exoml::cdecl::extract_c11(opts);
            json=exoml::cdecl::describe_translation_unit(extracted);
            r.parser_used="exoml/libclang";
        }catch(const std::exception &e){push_error(r,"EXOTRACT-EXOML",e.what());return r;}
    }
#endif
    if(json.empty()){
        if(config_.exoml_bind.empty()){
            for(const auto &fallback:config_.parser.fallback){
                const auto cr=run_command(fallback,config_,{{"%file",header}});
                if(cr.ok()){
                    r.parser_used="external CDecl JSON fallback";
                    r.externally_imported=true;
                    ingest_cdecl(r,cr.out);
                    if(r.declarations_complete)
                      for(const auto &name:config_.required_symbols)
                        if(std::none_of(r.symbols.begin(),r.symbols.end(),
                              [&](const Symbol &s){return s.name==name;}))
                          push_error(r,"EXOTRACT-REQUIRED-MISSING","required symbol absent from CDecl: "+name);
                    if(r.declarations_complete)r.diagnostics.push_back({Diagnostic::Severity::Warning,
                        "EXOTRACT-EXTERNAL-TRUST","external parser requires independent source provenance validation"});
                    return r;
                }
                if(cr.timed_out || cr.overflow || (!cr.err.starts_with("spawn failed:") && cr.exit_code!=127)){
                    push_error(r,"EXOTRACT-EXTERNAL-PARSER",reason(cr));return r;
                }
            }
            r.diagnostics.push_back({Diagnostic::Severity::Warning,"EXOTRACT-NO-CDECL",
                "syntax verified; ExoML C11 extraction unavailable; no typed FFI bindings emitted"});
            return r;
        }
        Command command;command.argv={config_.exoml_bind,"extract","--header",header};
        if(!config_.clang_resource_dir.empty()){
            command.argv.insert(command.argv.end(),{"--resource-dir",config_.clang_resource_dir});
        }
        if(!config_.target.empty())command.argv.insert(command.argv.end(),{"--target",config_.target});
        if(!config_.revision.empty())command.argv.insert(command.argv.end(),{"--revision",config_.revision});
        for(const auto &arg:config_.compiler_arguments)command.argv.insert(command.argv.end(),{"--compile-arg",arg});
        for(const auto &path:config_.provenance_files)command.argv.insert(command.argv.end(),{"--provenance",path.string()});
        for(const auto &dir:config_.include_directories){command.argv.push_back("--include");command.argv.push_back(dir.string());}
        for(const auto &define:config_.defines){command.argv.push_back("--define");command.argv.push_back(define);}
        for(const auto &undefine:config_.undefines){command.argv.push_back("--undefine");command.argv.push_back(undefine);}
        auto cr=run_command(command,config_);
        if(!cr.ok()) {
            if(!cr.out.empty())ingest_cdecl(r,cr.out);
            push_error(r,"EXOTRACT-EXOML",reason(cr));return r;
        }
        json=std::move(cr.out);r.parser_used="exoml-bind";
    }
    ingest_cdecl(r,std::move(json));
    if(r.declarations_complete)
      for(const auto &name:config_.required_symbols)
        if(std::none_of(r.symbols.begin(),r.symbols.end(),
              [&](const Symbol &s){return s.name==name;}))
          push_error(r,"EXOTRACT-REQUIRED-MISSING","required symbol absent from CDecl: "+name);
    return r;
}
Result Engine::import_cdecl(Result previous, std::string json) const {
    if(!previous.preprocessed_ok || !previous.syntax_checked)
        throw std::invalid_argument("import requires preprocessed and syntax-checked input");
    previous.diagnostics.erase(std::remove_if(previous.diagnostics.begin(),previous.diagnostics.end(),
        [](const Diagnostic &d){ return d.code=="EXOTRACT-NO-CDECL"; }),previous.diagnostics.end());
    ingest_cdecl(previous,std::move(json));
    for(const auto &name:config_.required_symbols)
        if(std::none_of(previous.symbols.begin(),previous.symbols.end(),
              [&](const Symbol &s){return s.name==name;}))
            push_error(previous,"EXOTRACT-REQUIRED-MISSING","required symbol absent from CDecl: "+name);
    previous.externally_imported=true;
    previous.parser_used="externally supplied ExoML CDecl (provenance not authenticated)";
    previous.diagnostics.push_back({Diagnostic::Severity::Warning,"EXOTRACT-IMPORT-TRUST",
        "external CDecl JSON import requires independent source fingerprint verification; unsafe exports only"});
    return previous;
}
Result Engine::inspect() const {
    auto r=parse();
    if(config_.cache && r.ok() && !r.externally_imported) {
        const auto key=result_key(config_,r);
        const auto hit=store_->get(key);
        if(hit && *hit==r.cdecl_json) r.cache_hit=true;
        else {
            if(hit)r.diagnostics.push_back({Diagnostic::Severity::Warning,"EXOTRACT-CACHE-REBUILT",
                "cached declarations differ from fresh compiler evidence; replaced"});
            cache(r);
        }
    }
    return r;
}
bool Engine::cache(const Result &r) const {
    if(!config_.cache)return false;
    if(!r.ok() || r.externally_imported)return false;
    store_->put(result_key(config_,r),r.cdecl_json);
    return true;
}
std::string Engine::implementation_source() const {
    const auto path=file_include_path(expanded_path());
    std::string s="/* Generated by Exotract; compile exactly once as C11. */\n";
    for(const auto &macro:config_.header_only.implementation_defines){
        if(macro.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos ||
            macro.empty() || (macro[0]>='0'&&macro[0]<='9'))
            throw std::invalid_argument("invalid implementation define: "+macro);
        s+="#ifndef "+macro+"\n#define "+macro+"\n#endif\n";
    }
    for(const auto &dependency:config_.header_only.dependencies)
        s+="#include \""+file_include_path(dependency)+"\"\n";
    s+="#include \""+path+"\"\n";
    return s;
}
std::string Engine::manifest_json(const Result &r) const {
    std::string s="{\"schema\":\"moonstone.exotract/2\",\"name\":"+json_quote(config_.name)+
       ",\"header\":"+json_quote(expanded_path())+",\"cstd\":\"C11\",\"summary\":"+
       json_quote(config_.summary)+",\"required_symbols\":"+arr(config_.required_symbols)+
       ",\"documentation\":"+json_quote(config_.documentation)+
       ",\"compiler_identity\":"+json_quote(r.compiler_identity)+",\"revision\":"+json_quote(config_.revision)+
       ",\"source_fingerprint\":"+json_quote(r.fingerprint)+",\"complete\":"+
       std::string(r.ok()?"true":"false")+",\"safe_export\":false,\"unknown_effects\":true"+
       ",\"syntax_checked\":"+(r.syntax_checked?"true":"false")+
       ",\"cache_hit\":"+(r.cache_hit?"true":"false")+
       ",\"externally_imported\":"+(r.externally_imported?"true":"false")+
       ",\"preprocessor\":"+json_quote(r.preprocessor_used)+",\"parser\":"+json_quote(r.parser_used)+
       ",\"header_only\":{\"style\":"+json_quote(config_.header_only.style)+
       ",\"declaration_guard\":"+json_quote(config_.header_only.declaration_guard)+
       ",\"implementation_guard\":"+json_quote(config_.header_only.implementation_guard)+
       ",\"implementation_defines\":"+arr(config_.header_only.implementation_defines)+
       ",\"dependencies\":"+arr(config_.header_only.dependencies)+
       ",\"implementation_translation_units\":1},\"symbols\":[";
    for(std::size_t i=0;i<r.symbols.size();++i){
        const auto &x=r.symbols[i];if(i)s+=',';
        s+="{\"name\":"+json_quote(x.name)+",\"kind\":"+json_quote(x.kind)+
           ",\"type\":"+json_quote(x.type)+",\"linkage\":"+json_quote(x.linkage)+
           ",\"supported\":"+(x.supported?"true":"false")+"}";
    }
    s+="],\"diagnostics\":[";
    for(std::size_t i=0;i<r.diagnostics.size();++i){if(i)s+=',';s+=diagnostic_json(r.diagnostics[i]);}
    return s+"]}\n";
}
std::string Engine::on1x_manifest_json(const Result &r) const {
    if(!r.ok())throw std::runtime_error("On1x binding requires complete ExoML CDecl and syntax verification");
    // No fabricated On1x source grammar; this is a typed, explicitly unsafe
    // interface manifest for the On1x compiler to consume and review.
    std::string s="{\"schema\":\"moonstone.exotract.on1x/2\",\"library\":"+json_quote(config_.name)+
        ",\"requires_review\":true,\"unsafe_by_default\":true,\"imports\":[";
    bool first=true;
    for(auto &x:r.symbols){if(x.kind!="function" || !x.supported)continue;if(!first)s+=',';first=false;
        s+="{\"c_name\":"+json_quote(x.name)+",\"c_type_id\":"+json_quote(x.type)+
            ",\"effects\":\"unknown\",\"safe\":false}";
    }
    return s+"],\"cdecl\":"+r.cdecl_json+"}\n";
}
std::string Engine::validate_contract(const Result &r,const fs::path &sidecar) const {
    if(!r.ok())throw std::runtime_error("cannot validate without complete CDecl");
#if defined(EXOTRACT_WITH_EXOML)
    const auto unit=exoml::cdecl::parse_translation_unit_json(r.cdecl_json);
    const auto bundle=exoml::contract::parse_contract_sidecar(read_file(sidecar,config_.max_output_bytes));
    const auto diagnostics=exoml::contract::validate_contract(bundle,unit);
    if(!diagnostics.empty()){
        std::string message="ExoML contract validation failed:";
        for(const auto &diagnostic:diagnostics)message+='\n'+moonstone::format(diagnostic);
        throw std::runtime_error(message);
    }
    // Preserve the supplied trust state; structural validation is not review
    // authentication, exact ABI verification or a safe-export authorization.
    return exoml::contract::serialize_contract(bundle);
#else
    if(config_.exoml_bind.empty())throw std::runtime_error("ExoML validator unavailable; configure exoml_bind");
    TemporaryFile temp(".cdecl.json"), normalized(".contract.json");
    write_file_atomic(temp.path,r.cdecl_json);
    Command command{{config_.exoml_bind,"validate","--decl",temp.path.string(),"--contract",sidecar.string(),"--out",normalized.path.string()}};
    auto result=run_command(command,config_);
    if(!result.ok())throw std::runtime_error("ExoML contract validation failed: "+reason(result));
    return read_file(normalized.path,config_.max_output_bytes);
#endif
}
} // namespace moonstone::exotract
