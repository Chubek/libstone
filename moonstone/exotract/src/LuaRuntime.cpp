#include <exotract/Exotract.hpp>
#include <iostream>
#include <algorithm>
#include <new>
#include <utility>
#include <vector>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
// Match the vendored Lua 5.5/Kaguya adaptation already used by Prolific.
#if LUA_VERSION_NUM >= 505
inline lua_State *exotract_newstate(lua_Alloc alloc, void *user) {
    return lua_newstate(alloc, user, 0);
}
#define lua_newstate(alloc, user) exotract_newstate(alloc, user)
#define LUA_GCSETPAUSE (-10001)
#define LUA_GCSETSTEPMUL (-10002)
inline int exotract_gc(lua_State *L,int op,int v) {
    if(op==LUA_GCSETPAUSE)return lua_gc(L,LUA_GCPARAM,LUA_GCPPAUSE,v);
    if(op==LUA_GCSETSTEPMUL)return lua_gc(L,LUA_GCPARAM,LUA_GCPSTEPMUL,v);
    if(op==LUA_GCSTEP)return lua_gc(L,op,static_cast<std::size_t>(v>0?v:0)*1024);
    return lua_gc(L,op);
}
#define lua_gc(L,op,v) exotract_gc(L,op,v)
#endif
#include <kaguya/kaguya.hpp>
#if LUA_VERSION_NUM >= 505
#undef lua_newstate
#undef lua_gc
#undef LUA_GCSETPAUSE
#undef LUA_GCSETSTEPMUL
#endif

namespace moonstone::exotract {
namespace {
struct Context {
    std::shared_ptr<Store> store;
    std::shared_ptr<GlobalRegistry> globals;
};
struct Library {
    Context *ctx;
    std::unique_ptr<Engine> engine;
    Result result;
};
Context &context(lua_State *L) {
    return *static_cast<Context *>(lua_touserdata(L,lua_upvalueindex(1)));
}
std::string checked_string(lua_State *L,int pos) {
    size_t len=0;
    if(lua_type(L,pos)!=LUA_TSTRING)throw std::invalid_argument("expected Lua string");
    const char *s=lua_tolstring(L,pos,&len);
    if(len>1024*1024)throw std::length_error("Lua string exceeds 1 MiB");
    return std::string(s,len);
}
std::string field(lua_State *L,int index,const char *key,std::string fallback={}){
    index=lua_absindex(L,index);lua_getfield(L,index,key);
    if(lua_isnil(L,-1)){lua_pop(L,1);return fallback;}
    if(!lua_isstring(L,-1)){lua_pop(L,1);throw std::invalid_argument(std::string("expected string field: ")+key);}
    auto out=checked_string(L,-1);lua_pop(L,1);return out;
}
bool boolfield(lua_State *L,int index,const char *key,bool fallback=false){
    index=lua_absindex(L,index);lua_getfield(L,index,key);
    if(lua_isnil(L,-1)){lua_pop(L,1);return fallback;}
    if(!lua_isboolean(L,-1)){lua_pop(L,1);throw std::invalid_argument(std::string("expected boolean field: ")+key);}
    bool out=lua_toboolean(L,-1)!=0;lua_pop(L,1);return out;
}
std::vector<std::string> strings(lua_State *L,int index,const char *key){
    std::vector<std::string> out;
    index=lua_absindex(L,index);lua_getfield(L,index,key);
    if(lua_isnil(L,-1)){lua_pop(L,1);return out;}
    if(!lua_istable(L,-1)){lua_pop(L,1);throw std::invalid_argument(std::string("expected array: ")+key);}
    auto t=lua_absindex(L,-1);
    const auto count=lua_rawlen(L,t);
    if(count>8192)throw std::length_error("Lua array length budget");
    for(lua_Integer i=1;i<=static_cast<lua_Integer>(count);++i){
        lua_rawgeti(L,t,i);out.push_back(checked_string(L,-1));lua_pop(L,1);
    }
    lua_pop(L,1);return out;
}
void parse_backend(lua_State *L,int config,const char *name,Backend &b){
    config=lua_absindex(L,config);lua_getfield(L,config,name);
    if(lua_isnil(L,-1)){lua_pop(L,1);return;}
    if(!lua_istable(L,-1))throw std::invalid_argument("invalid backend config");
    int t=lua_absindex(L,-1);
    b.main=field(L,t,"main","builtin");
    lua_getfield(L,t,"fallback");
    if(lua_istable(L,-1)){
        const auto n=lua_rawlen(L,-1);
        if(n>16)throw std::length_error("too many fallback backends");
        const int arr=lua_absindex(L,-1);
        for(lua_Integer i=1;i<=static_cast<lua_Integer>(n);++i){
            lua_rawgeti(L,arr,i);
            if(!lua_istable(L,-1))throw std::invalid_argument("fallback must be exotract.command.exec value");
            auto argv=strings(L,-1,"argv");
            if(argv.empty())throw std::invalid_argument("empty fallback argv");
            b.fallback.push_back({std::move(argv)});
            lua_pop(L,1);
        }
    }else if(!lua_isnil(L,-1))throw std::invalid_argument("fallback must be array");
    lua_pop(L,2);
}
Config config_from_lua(lua_State *L,int index,const std::string &name){
    luaL_checktype(L,index,LUA_TTABLE);
    Config c;c.name=name;
    c.path=field(L,index,"path");
    c.summary=field(L,index,"summary");
    c.documentation=field(L,index,"documentation");
    c.cstd=field(L,index,"cstd","C11");
    c.compiler=field(L,index,"compiler","cc");
    c.exoml_bind=field(L,index,"exoml_bind");
    c.clang_resource_dir=field(L,index,"clang_resource_dir");
    c.target=field(L,index,"target");c.revision=field(L,index,"revision");
    c.compiler_arguments=strings(L,index,"compiler_arguments");
    for(auto &path:strings(L,index,"provenance_files"))c.provenance_files.emplace_back(path);
    c.cache=boolfield(L,index,"cache");
    c.cache_dir=field(L,index,"cache_dir");
    c.defines=strings(L,index,"defines");
    c.undefines=strings(L,index,"undefines");
    c.required_symbols=strings(L,index,"required_exports");
    for(auto &x:strings(L,index,"includes"))c.include_directories.emplace_back(x);
    lua_getfield(L,index,"header_only");
    if(lua_istable(L,-1)){
        const int t=lua_absindex(L,-1);
        c.header_only.style=field(L,t,"style","implementation-macro");
        c.header_only.declaration_guard=field(L,t,"declaration_guard");
        c.header_only.implementation_guard=field(L,t,"implementation_guard");
        c.header_only.dependencies=strings(L,t,"dependencies");
        lua_getfield(L,t,"implementation");
        if(lua_istable(L,-1)){
            int impl=lua_absindex(L,-1);
            c.header_only.implementation_defines=strings(L,impl,"defines");
            lua_getfield(L,impl,"translation_units");
            if(!lua_isnil(L,-1)){
                if(!lua_isinteger(L,-1) || lua_tointeger(L,-1)!=1)throw std::invalid_argument("translation_units must be integer 1");
                c.header_only.implementation_translation_units=1;
            }
            lua_pop(L,1);
        }
        lua_pop(L,1);
    }else if(!lua_isnil(L,-1))throw std::invalid_argument("invalid header_only table");
    lua_pop(L,1);
    parse_backend(L,index,"preprocessor",c.preprocessor);
    parse_backend(L,index,"parser",c.parser);
    return c;
}
int fail(lua_State *L,const std::exception &e){return luaL_error(L,"Exotract: %s",e.what());}
Library &lib(lua_State *L){
    return *static_cast<Library *>(luaL_checkudata(L,1,"Exotract.Library"));
}
int library_gc(lua_State *L){lib(L).~Library();return 0;}
int library_new(lua_State *L){
    try {
        auto &ctx=context(L);auto name=checked_string(L,1);
        auto cfg=config_from_lua(L,2,name);
        void *memory=lua_newuserdatauv(L,sizeof(Library),0);
        new (memory) Library{&ctx,std::make_unique<Engine>(std::move(cfg),ctx.store,ctx.globals),{}};
        luaL_getmetatable(L,"Exotract.Library");lua_setmetatable(L,-2);
        return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int library_preprocess(lua_State *L){
    try{auto &obj=lib(L);obj.result=obj.engine->preprocess();
        if(!obj.result.preprocessed_ok)throw std::runtime_error(obj.engine->manifest_json(obj.result));
        lua_settop(L,1);return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int library_parse(lua_State *L){
    try{auto &obj=lib(L);obj.result=obj.engine->parse(std::move(obj.result));
        if(!obj.result.syntax_checked || std::any_of(obj.result.diagnostics.begin(),obj.result.diagnostics.end(),[](const Diagnostic &d){return d.severity==Diagnostic::Severity::Error;}))throw std::runtime_error(obj.engine->manifest_json(obj.result));
        lua_settop(L,1);return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int library_cache(lua_State *L){
    try{auto &obj=lib(L);lua_pushboolean(L,obj.engine->cache(obj.result));return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int library_manifest(lua_State *L){
    try{auto &obj=lib(L);auto s=obj.engine->manifest_json(obj.result);lua_pushlstring(L,s.data(),s.size());return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int library_bind(lua_State *L){
    try{
        auto &obj=lib(L);
        if(!obj.result.ok())throw std::runtime_error("typed ExoML extraction required for Bind()");
        std::string script, contract, policy="unsafe_explicit", engine="on1x";
        if(lua_isstring(L,2))script=checked_string(L,2);
        else if(lua_istable(L,2)){
            script=field(L,2,"script");policy=field(L,2,"safe_exports","unsafe_explicit");
            engine=field(L,2,"engine","on1x");
            contract=field(L,2,"contract");
        }
        if(policy=="reviewed_only")throw std::runtime_error(
            "reviewed_only binding requires reviewed semantic-contract evidence; raw CDecl is not reviewed");
        if(policy!="unsafe_explicit")throw std::invalid_argument("binding policy must be unsafe_explicit or reviewed_only");
        if(engine!="on1x")throw std::invalid_argument("currently implemented binder: on1x");
        if(!contract.empty())contract=obj.engine->validate_contract(obj.result,obj.ctx->globals->expand(contract));
        lua_newtable(L);int binder=lua_absindex(L,-1);
        lua_newtable(L);int internals=lua_absindex(L,-1);
        auto put=[&](const char *name,std::string v){lua_pushlstring(L,v.data(),v.size());lua_setfield(L,internals,name);};
        put("manifest",obj.engine->manifest_json(obj.result));
        put("cdecl",obj.result.cdecl_json);
        put("on1x",obj.engine->on1x_manifest_json(obj.result));
        put("implementation",obj.engine->implementation_source());
        if(!contract.empty())put("contract",std::move(contract));
        lua_pushboolean(L,1);lua_setfield(L,internals,"unsafe");
        if(!script.empty()){
            script=obj.ctx->globals->expand(script);
            if(luaL_loadfile(L,script.c_str())!=LUA_OK)throw std::runtime_error(checked_string(L,-1));
            if(lua_pcall(L,0,1,0)!=LUA_OK)throw std::runtime_error(checked_string(L,-1));
            if(!lua_isfunction(L,-1))throw std::runtime_error("custom binder must return a function(internals)");
            lua_pushvalue(L,internals);
            if(lua_pcall(L,1,1,0)!=LUA_OK)throw std::runtime_error(checked_string(L,-1));
            if(!lua_istable(L,-1))throw std::runtime_error("custom binder must return an Internals table");
            lua_replace(L,internals);internals=lua_absindex(L,-1);
        }
        lua_pushvalue(L,internals);lua_setfield(L,binder,"Internals");
        lua_pop(L,1);
        return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int global_set(lua_State *L){
    try{context(L).globals->set(checked_string(L,1),checked_string(L,2));return 0;
    }catch(const std::exception &e){return fail(L,e);}
}
int global_get(lua_State *L){
    try{auto v=context(L).globals->get(checked_string(L,1));
        if(v)lua_pushlstring(L,v->data(),v->size());else lua_pushnil(L);return 1;
    }catch(const std::exception &e){return fail(L,e);}
}
int command_exec(lua_State *L){
    luaL_checktype(L,1,LUA_TTABLE);
    lua_newtable(L);
    lua_pushstring(L,"exec");lua_setfield(L,-2,"__kind");
    lua_pushvalue(L,1);lua_setfield(L,-2,"argv");
    return 1;
}
void method(lua_State *L,const char *name,lua_CFunction fn) {lua_pushcfunction(L,fn);lua_setfield(L,-2,name);}
void closure(lua_State *L,const char *name,lua_CFunction fn,Context &ctx){
    lua_pushlightuserdata(L,&ctx);lua_pushcclosure(L,fn,1);lua_setfield(L,-2,name);
}
void install(lua_State *L,Context &ctx){
    luaL_newmetatable(L,"Exotract.Library");
    method(L,"__gc",library_gc);
    lua_pushstring(L,"Exotract.Library");lua_setfield(L,-2,"__metatable");
    lua_newtable(L);
    method(L,"Preprocess",library_preprocess);
    method(L,"Parse",library_parse);
    method(L,"Cache",library_cache);
    method(L,"Bind",library_bind);
    method(L,"Manifest",library_manifest);
    lua_setfield(L,-2,"__index");lua_pop(L,1);
    lua_newtable(L);int module=lua_absindex(L,-1);
    lua_newtable(L);closure(L,"new",library_new,ctx);lua_setfield(L,module,"exolib");
    for(const char *which:{"preprocessor","parser"}){
        lua_newtable(L);lua_pushstring(L,"builtin");lua_setfield(L,-2,"BUILTIN");
        lua_pushstring(L,"compiler");lua_setfield(L,-2,"COMPILER");lua_setfield(L,module,which);
    }
    lua_newtable(L);method(L,"exec",command_exec);lua_setfield(L,module,"command");
    lua_newtable(L);closure(L,"set",global_set,ctx);closure(L,"get",global_get,ctx);
    lua_setfield(L,module,"global");
    lua_newtable(L);lua_pushstring(L,"on1x");lua_setfield(L,-2,"ON1X");
    lua_setfield(L,module,"exobind");
    lua_pushvalue(L,module);lua_setglobal(L,"exotract");
    luaL_getsubtable(L,LUA_REGISTRYINDEX,LUA_LOADED_TABLE);
    lua_pushvalue(L,module);lua_setfield(L,-2,"exotract");lua_pop(L,2);
}
} // namespace
int run_lua_file(const std::string &path,const std::string &cache_dir,const std::vector<std::string> &vars) {
    try{
        Context ctx{std::make_shared<Store>(cache_dir),{}};
        ctx.globals=std::make_shared<GlobalRegistry>(ctx.store);
        for(const auto &entry:vars){
            const auto pos=entry.find('=');
            if(pos==std::string::npos)throw std::invalid_argument("--global requires NAME=VALUE");
            ctx.globals->set(entry.substr(0,pos),entry.substr(pos+1));
        }
        kaguya::State state(kaguya::LoadLibs{{"_G",luaopen_base},{"package",luaopen_package},
             {"table",luaopen_table},{"string",luaopen_string},{"math",luaopen_math},
             {"utf8",luaopen_utf8}});
        lua_State *L=state.state();
        if(!L)throw std::runtime_error("cannot allocate Lua state");
        lua_getglobal(L,"package");lua_pushstring(L,"");lua_setfield(L,-2,"path");
        lua_pushstring(L,"");lua_setfield(L,-2,"cpath");lua_pop(L,1);
        for(const char *f:{"dofile","loadfile","load","collectgarbage"}){
            lua_pushnil(L);lua_setglobal(L,f);
        }
        install(L,ctx);
        if(luaL_loadfile(L,path.c_str())!=LUA_OK)throw std::runtime_error((lua_tostring(L,-1)?lua_tostring(L,-1):"Lua raised a non-string error"));
        if(lua_pcall(L,0,1,0)!=LUA_OK)throw std::runtime_error((lua_tostring(L,-1)?lua_tostring(L,-1):"Lua raised a non-string error"));
        if(lua_istable(L,-1)){
            lua_getfield(L,-1,"manifest");
            if(lua_isnil(L,-1)){
                lua_pop(L,1);lua_getfield(L,-1,"Internals");
                if(lua_istable(L,-1))lua_getfield(L,-1,"manifest");
            }
            if(lua_isstring(L,-1))std::cout<<lua_tostring(L,-1);
            else std::cout<<"Exotract Lua script completed\n";
        }else if(lua_isstring(L,-1))std::cout<<lua_tostring(L,-1)<<'\n';
        return 0;
    }catch(const std::exception &e){std::cerr<<"exotract lua: "<<e.what()<<'\n';return 3;}
}
} // namespace moonstone::exotract
