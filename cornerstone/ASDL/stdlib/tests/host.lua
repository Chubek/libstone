-- Test-only host adapter. It is not the native C/Perl ASDL implementation.
local A={lib={},diagnostic={}}
local cache, loading, libraries, diagnostics, hooks, defs = {},{},{},{},{},{}
local root='cornerstone/ASDL/stdlib'
local function paths()
    local r={};for p in (os.getenv('LIBSTONE_ASDL_PATH') or ''):gmatch('[^:]+') do r[#r+1]=p end
    r[#r+1]=root;return r
end
function A.lib.searchpath() return paths() end
local function normalize(p)
    local a={};for x in p:gmatch('[^/]+') do if x=='..' then if #a>0 and a[#a]~='..' then table.remove(a) else a[#a+1]=x end elseif x~='.' then a[#a+1]=x end end
    return (p:sub(1,1)=='/' and '/' or '')..table.concat(a,'/')
end
function A.lib.resolve(name)
    local rel=name:match('%.lua$') and name or name:gsub('%.','/')..'.lua'
    local function exists(p) local f=io.open(p,'rb');if f then f:close();return normalize(p) end end
    if rel:sub(1,1)=='/' or rel:sub(1,2)=='./' or rel:sub(1,3)=='../' then return assert(exists(rel),'missing library '..name) end
    for _,p in ipairs(paths()) do local f=exists(p..'/'..rel);if f then return f end end
    error('missing library '..name,0)
end
function A.lib.new(name)
    assert(type(name)=='string' and name~='');local proxy={};libraries[proxy]={name=name,exports=proxy,signed=false};return proxy
end
function A.signlib(lib)
    local info=assert(libraries[lib],'not a library');assert(not info.signed,'already signed')
    local exports={};for k,v in pairs(lib) do assert(type(k)=='string','invalid export');exports[k]=v end
    for k in pairs(exports) do lib[k]=nil end
    info.exports=exports;info.signed=true
    setmetatable(lib,{__index=exports,__newindex=function()error('signed library is sealed',2)end,__pairs=function()return pairs(exports)end,__metatable='signed library'})
    return lib
end
function A.lib.issigned(l) return libraries[l] and libraries[l].signed or false end
function A.lib.exports(l) local r={} for k in pairs(l) do r[#r+1]=k end;table.sort(r);return r end
function A.lib.describe(l) local x=assert(libraries[l]);return {name=x.name,signed=x.signed,exports=A.lib.exports(l)} end
function A.lib.loaded() local r={} for p in pairs(cache) do r[#r+1]=p end;table.sort(r);return r end
function A.lib.require(name)
    local path=A.lib.resolve(name);if cache[path] then return cache[path] end
    assert(not loading[path],'circular library dependency: '..path);loading[path]=true
    local env=setmetatable({asdl=A},{__index=_G});env._G=env
    local chunk,err=loadfile(path,'t',env);local ok,res
    if chunk then ok,res=pcall(chunk) else ok,res=false,err end
    loading[path]=nil;if not ok then error(res,0) end
    assert(res==env.LIB and A.lib.issigned(res),'library must return signed LIB: '..path)
    cache[path]=res;return res
end
function A.lib.derive(path)
    local parent=A.lib.require(path);local lib=A.lib.new(path..':derived');for k,v in pairs(parent) do lib[k]=v end;return lib
end
function A.define(ty,tag,fields) defs[ty..':'..(tag or '')]=fields end
local node_mt={};local identities=0
function A.isnode(n) return type(n)=='table' and getmetatable(n)==node_mt end
function A.make(ty,tag,fields,origin)
    local ds=assert(defs[ty..':'..(tag or '')],'unknown constructor '..ty..':'..tostring(tag));local vs,known={},{}
    for _,d in ipairs(ds) do known[d.name]=true;local v=fields[d.name];if d.cardinality=='*' or d.cardinality=='+' then assert(type(v)=='table','expected sequence '..d.name);if d.cardinality=='+' then assert(#v>0,'empty nonempty sequence') end;local copy={} for i,x in ipairs(v) do copy[i]=x end;v=copy elseif d.cardinality~='?' then assert(v~=nil,'missing field '..d.name) end;vs[d.name]=v end
    for k in pairs(fields) do assert(known[k],'unknown field '..k) end
    identities=identities+1;return setmetatable({ty=ty,tag=tag,values=vs,annotations={},id=identities,span=origin and origin.span},node_mt)
end
function A.type(n) return n.ty end
function A.tag(n) return n.tag end
function A.id(n) return n.id end
function A.fields(n) local r={} for i,d in ipairs(defs[n.ty..':'..(n.tag or '')]) do r[i]={name=d.name,cardinality=d.cardinality,value=n.values[d.name]} end;return r end
function A.destruct(n) local fs=A.fields(n);local v={} for i,d in ipairs(fs) do v[i]=d.value end;return table.unpack(v,1,#fs) end
function A.get(n,k) return n.values[k] end
function A.children(n) local a={} for _,f in ipairs(A.fields(n)) do if f.cardinality=='*' or f.cardinality=='+' then for _,v in ipairs(f.value) do if A.isnode(v) then a[#a+1]=v end end elseif A.isnode(f.value) then a[#a+1]=f.value end end;return a end
function A.map_children(n,fn) local fields,changed={},false;local function visit(x) if not A.isnode(x) then return x end;local y=fn(x);assert(A.isnode(y) and A.type(x)==A.type(y),'child changed type');if x~=y then changed=true end;return y end;for _,f in ipairs(A.fields(n)) do local v=f.value;if f.cardinality=='*' or f.cardinality=='+' then local a={} for i,x in ipairs(v) do a[i]=visit(x) end;v=a else v=visit(v) end;fields[f.name]=v end;return changed and A.make(n.ty,n.tag,fields,n) or n end
function A.clone(n) local fs={} for _,f in ipairs(A.fields(n)) do if f.cardinality=='*' or f.cardinality=='+' then local a={} for i,x in ipairs(f.value) do a[i]=A.isnode(x) and A.clone(x) or x end;fs[f.name]=a else fs[f.name]=A.isnode(f.value) and A.clone(f.value) or f.value end end;return A.make(n.ty,n.tag,fs,n) end
function A.annotate(n,k,v) n.annotations[k]=v end
function A.annotation(n,k) return n.annotations[k] end
function A.annotations(n) return n.annotations end
function A.location(n) return n.span end
function A.option(k) return nil end
function A.diagnostic.emit(r) diagnostics[#diagnostics+1]=r;return r end
function A.diagnostic.count(s) local n=0 for _,r in ipairs(diagnostics) do if r.severity==s then n=n+1 end end;return n end
function A.logistics(n,...) return assert(hooks[n.ty],'no hook')(n,...) end
function A.hook(ty,f) hooks[ty]=f end
return A
