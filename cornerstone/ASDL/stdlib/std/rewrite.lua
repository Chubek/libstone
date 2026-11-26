LIB = asdl.lib.new "std.rewrite"

local Rewrite = {}
-- A rule returns nil (no change) or a replacement of the same ASDL type.
local function replacement(node,r)
    if r==nil then return node end
    assert(asdl.isnode(r) and asdl.type(r)==asdl.type(node),"rewrite changed ASDL type")
    return r
end

function Rewrite.bottom_up(root, rule, source_map)
    local record=source_map and asdl.lib.require('std.sourcemap').Record
    local function visit(node)
        local mapped = asdl.map_children(node, visit)
        local r=replacement(mapped,rule(mapped))
        if record then
            record(source_map,mapped,node)
            record(source_map,r,mapped)
        end
        return r
    end

    return visit(root)
end

function Rewrite.pipeline(root, ...)

    for i = 1, select('#', ...) do

        local pass = select(i, ...)
        root = assert(pass(root), "rewrite returned nil")
    end

    return root
end

-- Useful for reporting and testing rewrite rules, without applying them.

function Rewrite.rule(type_name, tag, transform)

    return function(node)

        if asdl.type(node) == type_name and asdl.tag(node) == tag then

            return transform(node)
        end

    end

end

function Rewrite.top_down(root,rule,source_map)
    local record=source_map and asdl.lib.require('std.sourcemap').Record
    local function visit(n)
        local r=replacement(n,rule(n))
        if record then record(source_map,r,n) end
        local mapped=asdl.map_children(r,visit)
        if record then record(source_map,mapped,r) end
        return mapped
    end;
    return visit(root)
end

function Rewrite.fixed_point(root,pass,equal,limit)

    for _=1,limit or 100 do
        local r=pass(root);
        if equal(root,r) then
            return r
        end;
        root=r
    end;
    error('rewrite limit exceeded')
end

local function get(n,k)
    for _,f in ipairs(asdl.fields(n)) do
        if f.name==k then
            return f.value
        end
    end
end

function LIB.Curry(root)

    return Rewrite.bottom_up(root,function(n)

        local tag=asdl.tag(n);
        local ty=asdl.type(n)
        if tag=='LambdaMany' then
            local ps,body=asdl.destruct(n);
            for i=#ps,1,-1 do
                body=asdl.make(ty,'Lambda',{param=ps[i],body=body},n)
            end;
            return body
        elseif tag=='ApplicationMany' then
            local fn,args=asdl.destruct(n);
            for _,a in ipairs(args) do
                fn=asdl.make(ty,'Application',{fn=fn,arg=a},n)
            end;
            return fn
        end

    end)
end

-- Mini literal operators. Do not fold overloadable dispatch or operations with unknown semantics.

local binary={Add='+',Sub='-',Mul='*',Div='/',Mod='mod',Eq='=',Lt='<',Gt='>'}
local function literal(n)
    if asdl.tag(n)~='Literal' then
        return nil
    end;
    local v=asdl.destruct(n);
    return asdl.tag(v),asdl.destruct(v)
end

local function fold_node(n)

    local tag=asdl.tag(n);
    if tag~='Binary' and tag~='Unary' then
        return nil
    end

    local op,a,b=asdl.destruct(n);
    local kind,x=literal(a);
    if not kind then
        return nil
    end;
    local k,y;
    if b then
        k,y=literal(b);
        if not k then
            return nil
        end
    end;
    local o=asdl.tag(op);
    local f=asdl.lib.require('std.transform.constfold').Primitives();
    local result,ok
    if o=='Neg' and (kind=='Integer' or kind=='Real') then
        ok,result=pcall(function()
            return -x
        end)
    elseif o=='Not' and kind=='Boolean' then
        ok,result=true,not x
    elseif kind=='Boolean' and k=='Boolean' and (o=='And' or o=='Or') then
        ok=true;
        if o=='And' then
            result=x and y
        else
            result=x or y
        end

    elseif k==kind and (kind=='Integer' or kind=='Real') and o~='Div' and o~='Mod' and binary[o] then
        ok,result=pcall(f[binary[o]],x,y)
    end

    if not ok then
        return nil
    end;
    local rt=type(result)=='boolean' and 'Boolean' or kind
    return asdl.make(asdl.type(n),'Literal',{value=asdl.make('literal',rt,{value=result},n)},n)
end

function LIB.ConstFold(root,adapter)
    if not asdl.isnode(root) then
        return asdl.lib.require('std.transform.constfold').ConstFold(root,adapter)
    end;
    return Rewrite.bottom_up(root,adapter or fold_node)
end

LIB['foldable!']=function(n)
    return LIB.ConstFold(n)
end

-- Mini nullary calls: caller supplies hygienic AST inlining adapter; default IR handles lambda applications.

function LIB.Inline(root,target,adapter)
    if not asdl.isnode(root) then
        return asdl.lib.require('std.transform.inline').Inline(root,target)
    end;
    if adapter then
        return adapter(root,target,Rewrite)
    end

    -- Mini functions have local declarations and statements, and invocation is an expression.

    -- Inline only zero-local, single Return function with a closed literal expression.

    local decls=get(root,'fndef');
    if not decls then
        error('Inline requires an adapter for this AST schema')
    end

    local value;
    for _,fn in ipairs(decls) do
        local name,vars,body=asdl.destruct(fn);
        if name==target and #vars==0 and #body==1 and asdl.tag(body[1])=='Return' then
            local v=asdl.destruct(body[1]);
            if asdl.tag(v)=='Literal' then
                value=v
            end
        end
    end

    if not value then
        return root
    end;
    return Rewrite.bottom_up(root,function(n)
        if asdl.tag(n)=='Invocation' and asdl.destruct(n)==target then
            return asdl.clone(value)
        end
    end)
end

LIB.BetaReduce=asdl.lib.require('std.transform.inline').BetaReduce
LIB.EtaReduce=asdl.lib.require('std.transform.inline').EtaReduce
LIB.DeadEliminate=asdl.lib.require('std.transform.deadcode').DeadEliminate
LIB.Rewrite = Rewrite
asdl.signlib(LIB)
return LIB
