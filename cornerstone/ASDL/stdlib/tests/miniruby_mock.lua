-- A minimal host simulator, NOT the production asdl runtime or library loader.
local asdl = {lib={}, diagnostic={}}
local hooks, cache, diagnostics = {}, {}, {}
-- @DEFS@
function asdl.lib.new(name) return {} end
function asdl.signlib(lib) end
function asdl.lib.require(name)
    if cache[name] then return cache[name] end
    local relative = name:gsub('%.','/') .. '.lua'
    local env = setmetatable({asdl=asdl}, {__index=_G})
    local chunk = loadfile('examples/userlib/' .. relative, 't', env) or
                  assert(loadfile('cornerstone/ASDL/stdlib/' .. relative,'t',env))
    local lib = chunk(); cache[name]=lib; return lib
end
function asdl.lib.derive(path)
    local parent = asdl.lib.require((path:gsub('%.lua$',''):gsub('/','.')))
    local lib={}; for k,v in pairs(parent) do lib[k]=v end; return lib
end
function asdl.make(ty, tag, fields, origin)
    assert(defs[ty..':'..(tag or '')], 'invalid constructor '..ty..':'..tostring(tag))
    return {ty=ty, tag=tag, values=fields, annotations={}}
end
function asdl.isnode(n) return type(n)=='table' and n.ty~=nil end
function asdl.type(n) return n.ty end
function asdl.tag(n) return n.tag end
function asdl.fields(n)
    local fs={}
    for i,f in ipairs(assert(defs[n.ty..':'..(n.tag or '')])) do
        fs[i]={name=f.name,cardinality=f.cardinality,value=n.values[f.name]}
    end
    return fs
end
function asdl.destruct(n)
    local fs, vals = asdl.fields(n), {}
    for i,f in ipairs(fs) do vals[i]=f.value end
    return table.unpack(vals,1,#fs)
end
function asdl.children(n)
    local out={}
    for _,f in ipairs(asdl.fields(n)) do
        if f.cardinality=='*' or f.cardinality=='+' then
            for _,v in ipairs(f.value) do if asdl.isnode(v) then out[#out+1]=v end end
        elseif asdl.isnode(f.value) then out[#out+1]=f.value end
    end
    return out
end
function asdl.map_children(n,fn)
    local fields, changed={},false
    for _,f in ipairs(asdl.fields(n)) do
        local v=f.value
        if f.cardinality=='*' or f.cardinality=='+' then
            local new={}
            for i,item in ipairs(v) do
                new[i]=asdl.isnode(item) and fn(item) or item
                if new[i]~=item then changed=true end
            end
            v=new
        elseif asdl.isnode(v) then
            local new=fn(v); if new~=v then changed=true end; v=new
        end
        fields[f.name]=v
    end
    return changed and asdl.make(n.ty,n.tag,fields,n) or n
end
function asdl.annotate(n,k,v) n.annotations[k]=v end
function asdl.location(n) return nil end
function asdl.option(k) return false end
function asdl.diagnostic.emit(d) diagnostics[#diagnostics+1]=d end
function asdl.diagnostic.count(severity)
    local count=0; for _,d in ipairs(diagnostics) do if d.severity==severity then count=count+1 end end
    return count
end
function asdl.logistics(n,...) return assert(hooks[n.ty], 'missing hook: '..n.ty)(n,...) end
-- @HOOKS@
local function n(ty,tag,fields) return asdl.make(ty,tag,fields or {}) end
local function e(tag,fields) return n('expression',tag,fields) end
local function name(s) return e('Name',{name=s}) end
local function lit(v) return e('Literal',{value=n('literal','Integer',{value=v})}) end
local function seq(...) return e('Sequence',{elems={...}}) end
local function assign(s,rhs) return e('Assign',{lhs=n('target','LocalTarget',{name=s}),rhs=rhs}) end
local function block(kind,body) return n('block',nil,{kind=n('block_kind',kind),params={},locals={},body=body}) end
local function method(s,body,params) return n('method',nil,{kind=n('method_kind','Instance'),name=s,params=params or {},body=body}) end
local function member(m) return n('class_member','MethodDef',{defn=m}) end
local function cls(s,base,members) return n('declaration','ClassDef',{name=s,superclass=base,members=members or {}}) end
local function top(...) return n('toplevel',nil,{decls={...}}) end
local ref, nonlocal_return, local_return = name('x'),e('Return',{value=lit(2)}),e('Return',{value=lit(3)})
local closure=block('Closure',seq(ref,nonlocal_return))
local lambda=block('Lambda',local_return)
local break_node=e('Break',{value=lit(4)})
local until_node=e('Until',{cond=lit(0),body=break_node})
local method_node=method('run',seq(assign('x',lit(1)),e('BlockValue',{closure=closure}),e('BlockValue',{closure=lambda}),until_node))
local root=top(cls('A',nil,{member(method_node)}),cls('A',nil,{member(method('run',lit(0)))}))
local result=assert(asdl.logistics(root))
assert(asdl.diagnostic.count('error')==0)
assert(asdl.diagnostic.count('warning')==1)
-- Normalization rebuilt the method; find its new handle.
local found=result.values.decls[1].values.members[1].values.defn
local loop=found.values.body.values.elems[4]
assert(loop.tag=='While' and loop.values.cond.tag=='Not')
assert(ref.annotations.resolution=='local')
assert(#closure.annotations.captures==1 and closure.annotations.captures[1].name=='x')
assert(nonlocal_return.annotations.nonlocal==true)
assert(nonlocal_return.annotations['control-target']==found.annotations['control-id'])
assert(local_return.annotations['control-target']==lambda.annotations['control-id'])
assert(break_node.annotations['control-target']==loop.annotations['control-id'])
assert(JSON(result):find('"$tag":"While"',1,true))
assert(SExpr(result):find('(While',1,true))
local bad=e('Return',{value=lit(0)})
assert(asdl.logistics(top(n('declaration','TopExpr',{expr=bad})))==nil)
assert(asdl.diagnostic.count('error')==1)
local bad_args=e('Send',{name='f',args={n('argument','Keyword',{name='a',value=lit(1)}),n('argument','Keyword',{name='a',value=lit(2)})}})
assert(asdl.logistics(top(n('declaration','TopExpr',{expr=bad_args})))==nil)
assert(asdl.diagnostic.count('error')==2)
local dup=method('bad',lit(0),{n('parameter','Required',{name='a'}),n('parameter','Required',{name='a'})})
assert(asdl.logistics(top(cls('B',nil,{member(dup)})))==nil)
assert(asdl.diagnostic.count('error')==3)
print('PASS: capture, nonlocal/local return, loop targets, normalization, reopening, serialization, invalid return/keyword/parameter.')
