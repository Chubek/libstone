return function(t)
local A,require,test,eq,arr,throws=t.A,t.require,t.test,t.eq,t.arr,t.throws
local I,C,G,S,n,lit=t.I,t.C,t.G,t.S,t.n,t.lit
local rng=require('std.testing.property').RNG

test('pass dependencies, cache invalidation, nil/false artifacts and rollback',function()
    local P=require('std.passes')
    local read={reads={schema=true}}
    local write={writes={schema=true}}
    assert(P.Conflict(read,write) and P.Conflict(write,read) and P.Conflict(nil,read))
    assert(not P.Conflict(read,read) and not P.Conflict({},nil))
    local calls,broken={},false
    local m=P.New({
        {name='result',requires={'parse','config','absent'},cache=true,effects={},
            key=function() return false end,
            run=function(_,d) assert(d.config==false and d.absent==nil);return d.parse end},
        {name='parse',cache=true,effects=read,key=function(i)return i.schema end,
            run=function(i)calls[#calls+1]=i.schema;return i.schema end},
        {name='config',cache=true,effects={},key=function(i)return i.options end,run=function()return false end},
        {name='absent',cache=true,effects={},key=function()return 1 end,run=function()end},
        {name='emit',requires={'result'},effects=write,run=function(_,d)
            assert(not broken,'emit failed');return 'emit:'..d.result
        end},
    })
    arr(P.Plan(m,{'emit'}),{'parse','config','absent','result','emit'})
    local r,s=P.Run(m,{schema='a',options=1},{targets={'emit'}})
    eq(r.emit,'emit:a');eq(s.ran,5);eq(s.hits,0)
    r,s=P.Run(m,{schema='a',options=1},{targets={'emit'}})
    eq(r.emit,'emit:a');eq(s.ran,1);eq(s.hits,4)
    broken=true
    throws(function()P.Run(m,{schema='b',options=1})end)
    broken=false
    r,s=P.Run(m,{schema='b',options=1},{targets={'emit'}})
    eq(r.emit,'emit:b');eq(s.ran,3);eq(s.hits,2);arr(calls,{'a','b','b'})
    P.Clear(m)
    _,s=P.Run(m,{schema='b',options=1},{targets={'result'}});eq(s.ran,4)
    local function noop()end
    throws(function()P.New({{name='a',requires={'b'},run=noop}})end)
    throws(function()P.New({{name='a',requires={'b'},run=noop},{name='b',requires={'a'},run=noop}})end)
    throws(function()P.New({{name='cached',cache=true,effects=write,run=noop}})end)
end)

test('source provenance, exact structural captures and rewrite contracts',function()
    local R=require('std.rewrite').Rewrite
    local M=require('std.match.structural')
    local Src=require('std.sourcemap')
    local left,right=lit(1),lit(2)
    local original=n('expression','Binary',{op=n('binop','Add'),lhs=left,rhs=right})
    local source=Src.New()
    local result=R.bottom_up(original,function(x)if x==left then return lit(3) end end,source)
    eq(Src.Trace(source,result),original)
    local _,newleft,newright=A.destruct(result)
    eq(Src.Trace(source,newleft),left);eq(newright,right)
    local next_map=Src.New()
    local last=R.top_down(result,function(x)if x==result then return lit(4) end end,next_map)
    eq(Src.Trace(Src.Compose(source,next_map),last),original)
    local cycle=Src.New();Src.Record(cycle,'a','b');Src.Record(cycle,'b','a')
    throws(function()Src.Trace(cycle,'a')end)
    throws(function()R.bottom_up(left,function()return false end)end)
    local pattern=M.Capture('x',false)
    eq(M.Match(pattern,false).x,false);eq(M.Match(pattern,true),nil)
    eq(M.Match({k=M.Any()},{}),nil)
    local repeated={k=M.Capture('x'),j=M.Capture('x')}
    assert(M.Match(repeated,{k=lit(1),j=lit(1)}));eq(M.Match(repeated,{k=lit(1),j=lit(2)}),nil)
    local D=require('std.diagnostics')
    local mark=D.Checkpoint()
    local r=D.Warn(D.FaultPattern('W01','value %d'),left,5)
    eq(r.code,'W01');eq(r.location,A.location(left));eq(r.message,'value 5')
    assert(not D.FailedSince(mark))
    throws(function()D.Fatal('stop',left)end);eq(D.Checkpoint(),mark+1)
end)

test('HM variable chains, shared terms and transactional first-order unification',function()
    local H=require('std.types.hm')
    local E=require('std.semantics.environment').Env
    local a,b=H.Fresh(),H.Fresh()
    local x=a
    for _=1,10000 do local y=H.Fresh();H.Unify(x,y);x=y end
    eq(H.Prune(a),x);eq(a.link,x)
    local shared=b
    for _=1,30 do shared=H.Tuple({shared,shared}) end
    eq(S.size(H.Free(shared)),1)
    H.Unify(x,shared)
    local env=E.bind_value(E.new(),'x',H.Mono(a))
    eq(#H.Generalize(env,b).quantified,0)
    eq(#H.Generalize(E.bind_value(env,'x',H.Mono(H.Named('bool'))),b).quantified,1)
    local U=require('std.types.unification')
    local function v(x)return U.Var(x)end
    local function s(x,...)return U.Term(x,{...})end
    local init={a=v('b')}
    local q=assert(U.Unify(s('pair',v('a'),v('b')),s('pair',s('ok'),s('ok')),init))
    eq(U.Apply(v('a'),q).symbol,'ok');eq(init.b,nil)
    eq(U.Unify(v('b'),s('f',v('a')),init),nil);eq(init.b,nil)
    eq(U.Unify(s('pair',v('a'),s('left')),s('pair',s('ok'),s('right')),init),nil)
    eq(init.b,nil)
    throws(function()U.Apply(v('x'),{x=s('f',v('x'))})end)
end)

test('finite coverage witnesses and redundancy agree with ordered matcher',function()
    local D=require('std.match.decisiontree')
    local function W()return {tag='wild'}end
    local function L(v)return {tag='lit',value=v}end
    local function P(a,b)return {tag='ctor',name='pair',args={a,b}}end
    local function O(a,b)return {tag='or',lhs=a,rhs=b}end
    local bool={{pattern=L(true),domains={}},{pattern=L(false),domains={}}}
    local domain={{pattern=P(W(),W()),domains={bool,bool}}}
    local values={{true,true},{true,false},{false,true},{false,false}}
    local function view(v)if type(v)=='table' then return 'pair',v end end
    local nextint=rng(53626)
    local leaves={W(),L(true),L(false),O(L(false),L(true))}
    for _=1,300 do
        local rows,covered={},{}
        local redundant={}
        for i=1,nextint(0,8) do
            local a,b=leaves[nextint(1,4)],leaves[nextint(1,4)]
            local shape=nextint(1,4)
            local p=shape==1 and W() or shape==2 and O(P(a,b),P(b,a)) or P(a,b)
            local guarded=nextint(1,3)==1
            local any=false
            local f=D.Compile({{pattern=p}},view)
            for j,x in ipairs(values) do
                if f(x) and not covered[j] then
                    any=true
                    if not guarded then covered[j]=true end
                end
            end
            if not any then redundant[#redundant+1]=i end
            rows[i]={pattern=p,guarded=guarded}
        end
        local report=D.Coverage(rows,domain)
        arr(report.redundant,redundant)
        eq(report.exhaustive,S.size(covered)==4)
        if report.witness then
            local f=D.Compile({{pattern=report.witness}},view)
            local found=false
            for i,x in ipairs(values) do if f(x) then assert(not covered[i]);found=true end end
            assert(found)
        end
    end
    local calls=0
    local f=D.Compile({{pattern=O(W(),W()),guard=function()calls=calls+1;return false end},
        {pattern=W(),action=function()return false,'ok'end}},view)
    local index,value,extra=f(false)
    eq(index,2);eq(value,false);eq(extra,'ok');eq(calls,1)
end)

test('JSON/Binary round trips for generated values and malformed transport',function()
    local J=require('std.serialize')
    local random=rng(981)
    local atoms={J.Null,false,true,'\0😀\n"',math.mininteger,math.maxinteger,1.0000000000000002,5e-324,-0.0}
    local function value(depth)
        local choice=random(1,depth>0 and 3 or 1)
        if choice==1 then return atoms[random(1,#atoms)] end
        local r=choice==2 and J.Array() or {}
        for i=1,random(0,4) do
            local key=choice==2 and i or ('field'..i)
            r[key]=value(depth-1)
        end
        return r
    end
    for _,a in ipairs(atoms) do
        local x=J.DecodeJSON(J.JSON(a));eq(x,a)
        if type(a)=='number' then eq(math.type(a),math.type(x)) end
    end
    eq(1 / J.DecodeJSON(J.JSON(-0.0)),-math.huge)
    for _=1,300 do
        local x=value(4);local json=J.JSON(x);local bin=J.Binary(x)
        eq(J.JSON(J.FromJSON(json)),json)
        eq(J.JSON(J.FromBinary(bin)),json)
        local k=random(0,#bin-1)
        throws(function()J.FromBinary(bin:sub(1,k))end)
    end
    for _,bad in ipairs({'"\237\160\128"','"\\udbff\\udbff"','"\\udfff"','1e9999','[false,]',
        '{"a":null,"\\u0061":false}','{"$type":false}','{"$type":"test","b":[null],"c":1}'}) do
        throws(function()J.FromJSON(bad)end)
    end
    throws(function()J.DecodeJSON('[[[]]]',{depth=1})end)
    local node=n('test',nil,{a=false,b={lit(1)},c='value'})
    A.annotate(node,'items',J.Array({false,J.Null}))
    local copy=J.FromBinary(J.Binary(node,{annotations=true}),{annotations=true})
    eq(A.destruct(copy),false)
    eq(J.JSON(A.annotation(copy,'items')),'[false,null]')
end)

test('random dominance against entry-path deletion oracle',function()
    local D=require('std.graph.dominators')
    local nextint=rng(371)
    for _=1,120 do
        local g=G.new({1,2,3,4,5,6,7})
        for i=1,7 do for j=1,7 do if nextint(1,5)==1 then G.edge(g,i,j) end end end
        local function reach(skip)
            local seen={}
            local function visit(i)
                if i==skip or seen[i] then return end
                seen[i]=true
                for _,j in ipairs(g.out[i]) do visit(j) end
            end
            visit(1);return seen
        end
        local d,idom,f=D.Dominators(g,1),D.ImmediateDominators(g,1),D.Frontier(g,1)
        local full=reach()
        for _,x in ipairs(g.nodes) do
            if not full[x] then eq(d[x],nil) else
                local without=reach(x)
                for _,y in ipairs(g.nodes) do
                    if full[y] then
                        eq(not not d[y][x],not without[y])
                        local frontier=false
                        for _,pred in ipairs(g.incoming[y]) do
                            if full[pred] and not without[pred] and (x==y or without[y]) then frontier=true end
                        end
                        eq(not not f[x][y],frontier)
                    end
                end
                if idom[x] then
                    for y in pairs(d[x]) do if y~=x then assert(d[idom[x]][y]) end end
                else eq(x,1) end
            end
        end
    end
end)

test('backward boolean dataflow, SSA loop/copy cycles and edge liveness',function()
    local DF=require('std.analysis.dataflow')
    local g=G.new({'last','first'},{{'first','last'}})
    local ins,outs,stats=DF.Backward(g,{bottom=function()return false end,
        join=function(a,b)return a or b end,equal=function(a,b)return a==b end,
        statistics=true,boundary={last=true},transfer=function(v,a)return v=='first' and a or false end})
    eq(outs.last,true);eq(outs.first,false);eq(ins.first,false);eq(stats.steps,2)
    local SSA=require('std.transform.ssa')
    local instr=t.instr
    local c=C.New(1,{
        [1]={instrs={instr('init',{1,'y'})},term=instr('jump',{}, {},{2})},
        [2]={instrs={},term=instr('branch',{}, {1},{3,4})},
        [3]={instrs={instr('swap',{1,'y'},{'y',1})},term=instr('jump',{}, {},{2})},
        [4]={instrs={},term=instr('ret',{}, {'y','missing'})},
    },{1,2,3,4})
    local ss=SSA.ToSSA(c)
    assert(SSA.ValidateSSA(ss));eq(#ss.blocks[2].phis,2)
    assert(ss.undef.missing)
    local a,b=table.unpack(ss.blocks[2].phis)
    local live_in,live_out=require('std.analysis.liveness').Liveness(ss)
    assert(live_out[1][a.args[1]] and live_out[3][a.args[3]])
    assert(not live_out[1][a.args[3]] and not live_in[2][b.dest])
    local wrong=C.Copy(ss);wrong.blocks[2].phis[1].args[1]=a.args[3]
    throws(function()SSA.ValidateSSA(wrong)end)
    local swapped=C.Copy(ss)
    swapped.blocks[2].phis[1].args[3]=b.dest
    swapped.blocks[2].phis[2].args[3]=a.dest
    assert(SSA.ValidateSSA(swapped))
    local lower=SSA.FromSSA(swapped)
    local edge=lower.blocks[lower.blocks[3].term.targets[1]]
    local storage={[a.dest]=10,[b.dest]=20}
    for _,x in ipairs(edge.instrs) do storage[x.defs[1]]=storage[x.uses[1]] end
    eq(storage[a.dest],20);eq(storage[b.dest],10);eq(c.blocks[2].phis,nil)
end)

test('nested closure/lift/CPS/ANF equivalence, shadowing and lazy errors',function()
    local nextint=rng(5285)
    local transformations={
        function(e)return t.eval(require('std.transform.anf').ANormalize(e),{}) end,
        function(e)return t.eval(require('std.transform.cps').CPSConvert(e),{})(function(x)return x end) end,
        function(e)return t.closed_eval(require('std.transform.closureconv').ClosureConvert(e)) end,
        function(e)return t.closed_eval(require('std.transform.lambdalift').LambdaLift(e)) end,
    }
    local function expression(depth)
        if depth==0 then return I.Const(nextint(-5,5)) end
        local k=nextint(1,4)
        if k==1 then return I.Prim('+',{expression(depth-1),expression(depth-1)}) end
        if k==2 then return I.If(I.Const(nextint(0,1)==1),expression(depth-1),expression(depth-1)) end
        if k==3 then return I.Let('x',expression(depth-1),
            I.Let('f',I.Lambda('y',I.Prim('-',{I.Var('x'),I.Var('y')})),
                I.Let('x',I.Const(99),I.App(I.Var('f'),expression(depth-1))))) end
        return I.App(I.App(I.Lambda('x',I.Lambda('y',I.Prim('*',{I.Var('x'),I.Var('y')}))),
            expression(depth-1)),expression(depth-1))
    end
    for _=1,100 do
        local expr=expression(3);local expected=t.eval(expr,{})
        for _,f in ipairs(transformations) do eq(f(expr),expected) end
    end
    local bad=I.Prim('/',{I.Const(1),I.Const(0)})
    for _,f in ipairs(transformations) do
        eq(f(I.If(I.Const(false),bad,I.Const(17))),17)
        throws(function()f(I.Let('unused',bad,I.Const(17)))end)
    end
end)
end
