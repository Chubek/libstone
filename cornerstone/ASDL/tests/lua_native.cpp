#include "asdl/lua.hpp"
#include "asdl/schema.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

template<class F>
void throws(F fn, std::string_view message) {
    try {
        fn();
    } catch (const std::runtime_error& e) {
        if (std::string_view(e.what()).find(message) != std::string_view::npos) return;
        throw;
    }
    throw std::runtime_error("expected failure containing: " + std::string(message));
}

asdl::Field field(std::string type, std::string name, char qualifier = 0) {
    asdl::Field f;
    f.type = std::move(type);
    f.name = std::move(name);
    f.qualifier = qualifier;
    return f;
}

asdl::Schema schema() {
    asdl::Schema schema;
    schema.module = "NativeTest";
    schema.options["native"] = "yes";
    asdl::Definition expr;
    expr.name = "expr";
    expr.kind = asdl::DefinitionKind::sum;
    expr.attributes = {field("int", "attr", '?')};
    asdl::Constructor lit;
    lit.name = "Literal";
    lit.tag = 1;
    lit.fields = {field("int", "number"), field("bool", "flag", '?'), field("string", "name")};
    asdl::Constructor branch;
    branch.name = "Branch";
    branch.tag = 2;
    branch.fields = {field("expr", "lhs", '?'), field("expr", "rhs", '+')};
    expr.constructors = {lit, branch};
    asdl::Definition root;
    root.name = "root";
    root.fields = {field("expr", "child"), field("string", "notes", '*')};
    schema.definitions = {expr, root};
    return schema;
}

int install(lua_State* l) {
    if (!lua_istable(l, 1) || !lua_islightuserdata(l, 2)) return luaL_error(l, "bad installer frame");
    lua_pushboolean(l, *static_cast<bool*>(lua_touserdata(l, 2)));
    lua_setfield(l, 1, "extension_installed");
    return 0;
}

int broken_install(lua_State* l) { return luaL_error(l, "installer failed"); }
int throwing_install(lua_State*) { throw std::runtime_error("installer threw"); }

void native_api() {
    asdl::LuaOptions options;
    options.values = {{"profile", "testing"}};
    options.arguments = {"fixture", "input"};
    asdl::LuaSession lua(schema(), options);
    const int top = lua_gettop(lua.state());
    bool yes = true;
    lua.extend(install, &yes);
    lua.check("error('not executed during check')");
    throws([&] { lua.check("for"); }, "expected");
    throws([&] { lua.extend(broken_install); }, "installer failed");
    throws([&] { lua.extend(throwing_install); }, "installer threw");
    lua.run(R"lua(
        local a = asdl
        assert(a.extension_installed and a.option('native')=='yes' and a.option('profile')=='testing')
        assert(a.option('missing')==nil and arg[1]=='fixture' and arg[2]=='input')
        assert(io==nil and os==nil and package==nil and debug==nil and coroutine==nil and load==nil)
        local meta = a.schema()
        assert(meta.module=='NativeTest' and #meta.definitions==2)
        assert(a.describe('expr','Branch').fields[2].cardinality=='+')
        assert(a.describe('expr').attributes[1].attribute)
        assert(a.constructors('expr')[1].name=='Literal' and #a.constructors('root')==0)
        meta.definitions[1].name='broken'
        assert(a.schema().definitions[1].name=='expr')
        local location = {file='test.asdl', begin={line=7,column=2}, ['end']={line=7,column=3}}
        local lit = a.make('expr','Literal',{number=42, name='a\0b'},location)
        location.begin.line=100
        assert(type(lit)=='userdata' and a.isnode(lit) and not a.isnode({}) and not a.isnode(nil))
        assert(not a.isnode(false) and not a.isnode("asdl.node") and getmetatable(lit)=='ASDL node')
        assert(a.type(lit)=='expr' and a.tag(lit)=='Literal' and a.kind(lit)=='sum' and a.is_sum(lit))
        local values = table.pack(a.destruct(lit))
        assert(values.n==4 and values[1]==42 and values[2]==nil and values[3]=='a\0b' and values[4]==nil)
        local fields = a.fields(lit)
        assert(#fields==4 and fields[2].name=='flag' and fields[2].cardinality=='?' and fields[2].value==nil)
        assert(fields[4].attribute and a.has(lit,'number') and not a.has(lit,'flag') and not a.has(lit,'missing'))
        assert(a.get(lit,'number')==42 and a.get(lit,'flag')==nil and #a.children(lit)==0)
        a.set(lit,'flag',false)
        assert(a.has(lit,'flag') and a.get(lit,'flag')==false)
        a.set(lit,'flag',nil)
        assert(not a.has(lit,'flag') and a.location(lit).begin.line==7)
        local loc=a.location(lit)
        loc.begin.line=100
        assert(a.location(lit).begin.line==7)
        local raw={lit,lit}
        local branch=a.make('expr','Branch',{rhs=raw},lit)
        raw[1]=false
        local out=a.get(branch,'rhs')
        out[2]=false
        assert(a.children(branch)[1]==lit and a.children(branch)[2]==lit)
        assert(a.provenance(branch).origin==a.id(lit) and a.provenance(branch).location.begin.line==7)
        local root=a.construct('root',nil,branch)
        assert(a.tag(root)==nil and a.kind(root)=='product' and a.is_product(root))
        assert(#a.get(root,'notes')==0 and a.children(root)[1]==branch)
        assert(a.same(lit,lit) and not a.same(lit,branch))
        assert(tostring(lit):match('^asdl%.expr%.Literal#'))
        a.annotate(lit,'memo',{count=8})
        a.annotate(lit,'owner',lit)
        assert(a.annotation(lit,'memo').count==8 and a.annotation(lit,'owner')==lit)
        local anns=a.annotations(lit)
        anns.memo=nil
        assert(a.annotation(lit,'memo').count==8)
        assert(a.annotation(lit)==nil and a.annotation(lit,'missing')==nil)
        a.annotate(lit,'owner',nil)
        assert(a.annotation(lit,'owner')==nil)
        local shallow=a.copy(branch)
        local deep=a.clone(branch)
        assert(a.equal(branch,shallow) and a.equal(branch,deep))
        assert(a.hash(branch)==a.hash(shallow) and a.hash(branch)==a.hash(deep))
        assert(not a.same(branch,shallow) and not a.same(branch,deep))
        assert(a.get(shallow,'rhs')[1]==lit and a.get(deep,'rhs')[1]~=lit)
        assert(a.get(deep,'rhs')[1]==a.get(deep,'rhs')[2] and a.annotation(a.get(deep,'rhs')[1],'memo')==nil)
        local fresh=a.replace(lit,'number',43)
        assert(a.get(fresh,'number')==43 and a.get(lit,'number')==42 and not a.equal(fresh,lit))
        assert(a.annotation(fresh,'memo')==nil and a.provenance(fresh).origin==a.id(lit))
        local observed={}
        local mapped=a.map_children(branch,function(n)
            observed[#observed+1]=n
            if #observed==2 then return fresh end
        end)
        assert(a.children(mapped)[1]==lit and a.children(mapped)[2]==fresh and #observed==2)
        assert(a.map_children(branch,function(n) return n end)==branch)
        local function bad(needle,fn,...)
            local ok,err=pcall(fn,...)
            assert(not ok and tostring(err):find(needle,1,true),tostring(err))
        end
        local function map_failed() error("callback failure") end
        bad("callback failure",a.map_children,branch,map_failed)
        bad('different AST type',a.map_children,branch,function() return root end)
        bad('cycle',a.set,branch,'lhs',branch)
        bad('cycle',a.set,branch,'rhs',{shallow,branch})
        bad('nonempty sequence',a.set,branch,'rhs',{})
        bad('dense sequence',a.set,branch,'rhs',{[2]=lit})
        bad('dense sequence',a.set,branch,'rhs',{lit, surprise=true})
        bad('integer',a.set,lit,'number','99')
        bad('missing required',a.set,lit,'name',nil)
        bad('unknown AST field',a.make,'expr','Literal',{unknown=1})
        bad('invalid constructor',a.make,'root','Literal')
        bad('expected node of ASDL type',a.set,branch,'rhs',{root})
        bad('live node',a.type,{})
        assert(a.get(branch,'lhs')==nil and #a.get(branch,'rhs')==2)
        assert(a.validate(root))
        a.freeze(root)
        assert(a.is_frozen(root) and a.is_frozen(branch) and a.is_frozen(lit))
        bad('frozen',a.set,lit,'number',0)
        assert(not a.is_frozen(a.copy(lit)))
        a.annotate(lit,'frozen-structure-only',true)
        assert(a.annotation(lit,'frozen-structure-only'))
        local diagnostics=a.diagnostic
        diagnostics.emit{severity='warning',code='W',message='message',location=a.location(lit)}
        diagnostics.emit{severity='error',message='error'}
        bad('invalid diagnostic severity',diagnostics.emit,{severity='unknown',message='invalid'})
        assert(diagnostics.count('warning')==1 and diagnostics.count('info')==0 and diagnostics.count()==2)
        local ds=diagnostics.records()
        ds[1].location.begin.line=100
        assert(diagnostics.records()[1].location.begin.line==7)
        local weak=setmetatable({},{__mode='v'})
        do
            local tmp=a.clone(root)
            a.annotate(tmp,'self',tmp)
            weak[1]=tmp
        end
        collectgarbage('collect')
        collectgarbage('collect')
        assert(not weak[1], 'userdata/annotation cycle was not collectible')
        saved=root
    )lua", "@native.lua");
    require(lua.diagnostic_count("warning") == 1 && lua.diagnostic_count() == 2,
            "native diagnostic counts");
    throws([&] { lua.run("local function nested() error('boom') end; nested()", "@trace.lua"); },
           "stack traceback");
    lua.run("assert(asdl.isnode(saved))");
    require(lua_gettop(lua.state()) == top, "stack balance after errors/success");
    asdl::LuaSession other(schema());
    other.run("assert(saved==nil and asdl.diagnostic.count()==0)");
    // Simulate a broken native adapter that copied a handle across arenas.
    // Ordinary Lua code cannot copy userdata payloads or change metatables.
    lua_getglobal(lua.state(), "saved");
    const auto bytes = lua_rawlen(lua.state(), -1);
    void* foreign = lua_newuserdatauv(other.state(), bytes, 0);
    std::memcpy(foreign, lua_touserdata(lua.state(), -1), bytes);
    luaL_setmetatable(other.state(), "asdl.node");
    lua_setglobal(other.state(), "foreign");
    lua_pop(lua.state(), 1);
    other.run("assert(not asdl.isnode(foreign) and not pcall(asdl.type, foreign))");
}

void limits() {
    asdl::LuaOptions bounded;
    bounded.memory_limit = 128 * 1024;
    bounded.instruction_limit = 10'000;
    bounded.traversal_limit = 3;
    asdl::LuaSession lua(schema(), bounded);
    throws([&] { lua.run("while true do end"); }, "instruction limit");
    throws([&] { lua.run("while true do pcall(function() while true do end end) end"); }, "instruction limit");
    throws([&] { lua.run("local x = string.rep('x', 1024*1024)"); }, "memory");
    lua.run(R"lua(
        local a=asdl
        local edge=a.construct('expr','Literal',1,nil,'x')
        for i=1,4 do edge=a.construct('expr','Branch',nil,{edge}) end
        local ok,err=pcall(a.freeze,edge)
        assert(not ok and err:find('traversal limit',1,true))
        assert(not a.is_frozen(edge))
    )lua");
    asdl::LuaOptions tiny;
    tiny.memory_limit = 10;
    throws([&] { asdl::LuaSession impossible(schema(), tiny); }, "allocate");
}

} // namespace

int main() {
    try {
        native_api();
        limits();
        std::cout << "native ASDL Lua checks passed (" << LUA_VERSION << ")\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
