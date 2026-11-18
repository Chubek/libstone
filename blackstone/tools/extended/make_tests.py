#!/usr/bin/env python3
"""Build standalone Lua differential tests for the generated Blackstone suite."""
import pathlib, sys, json
from generate import CHOSEN
root=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else pathlib.Path(__file__).resolve().parents[3]
tests=root/'blackstone'/'tests'; tests.mkdir(parents=True, exist_ok=True)
params={'X':1,'Y':2,'Z':3,'A':4,'B':5,'C':6,'D':7,'E':8}
def luatab(obj):
    if isinstance(obj,str):return json.dumps(obj)
    if isinstance(obj,bool):return 'true' if obj else 'false'
    if obj is None:return 'nil'
    if isinstance(obj,int):return str(obj)
    if isinstance(obj,list):return '{'+','.join(luatab(x) for x in obj)+'}'
    if isinstance(obj,dict):return '{'+','.join('['+luatab(k)+']='+luatab(v) for k,v in obj.items())+'}'
    raise ValueError(type(obj))
def make_case(rule):
    vals={'K':4,'K1':2,'K2':3,'MASK':63}
    vals.update(rule['values'])
    if 'cancel_mul_constant' in rule['name']: vals['K']=3
    ts={i:{'kind':'integer','bits':1 if name=='C' else 8,'type_name':''} for name,i in params.items()}
    instructions=[]; nextid=9
    def rec(e,parentbits=8):
        nonlocal nextid
        if isinstance(e,str):return {'kind':'value','value':params[e]}
        if isinstance(e,int):return {'kind':'integer','integer':e,'bits':parentbits,'type_name':''}
        if isinstance(e,dict):
            if 'capture' in e:return {'kind':'integer','integer':vals[e['capture']],'bits':parentbits,'type_name':''}
            raise ValueError('cannot instantiate computation in source')
        assert isinstance(e,list)
        op=e[0]
        bits=1 if op in ('eq','ne','lt','le','gt','ge') else (parentbits if op in ('not','xor','and','or','neg') and parentbits==1 else 8)
        # select first operand one bit; eq/ne compare C when used inside a selector
        ins=[]
        for idx,a in enumerate(e[1:]):
            argbits=(1 if op=='select' and idx==0 else 8 if op=='select' else 1 if (op in ('not','xor','and','or','neg') and bits==1) else
                     (1 if op in ('eq','ne') and ('C' in json.dumps(e)) and all(v not in json.dumps(e) for v in ['"X"','"Y"','"Z"','"A"','"B"','"D"','"E"']) else 8))
            # Comparison of 1-bit C vs constants must use bool constants.
            ins.append(rec(a,argbits))
        item={'op':op,'result_id':nextid,'kind':'integer','bits':bits,'type_name':'',
              'operands':ins,'targets':[],'incoming':[],'annotation':''}
        ts[nextid]={'kind':'integer','bits':bits,'type_name':''}
        instructions.append(item);nextid+=1
        return {'kind':'value','value':item['result_id']}
    rootop=rec(rule['source'])
    instructions.append({'op':'return','kind':'void','bits':0,'type_name':'',
                         'operands':[rootop],'targets':[],'incoming':[],'annotation':''})
    return {'integer_semantics':'modular','blocks':{1:instructions},'value_types':ts,
            'new_types':[],'next_value':nextid,'dominators':{1:{1:True}},'loop_list':[]},rootop['value']

LUA_TEST=r'''
local testcases = __TESTCASES__
local function copy(t)
    if type(t) ~= 'table' then return t end
    local result={}
    for k,v in pairs(t) do result[k]=copy(v) end
    return result
end
local function equal(a,b)
    if type(a) ~= 'table' or type(b) ~= 'table' then return a==b end
    for k,v in pairs(a) do if not equal(v,b[k]) then return false end end
    for k in pairs(b) do if a[k]==nil then return false end end
    return true
end
local function mask(bits) return bits==64 and -1 or (1<<bits)-1 end
local function signed(n,bits)
    n=n & mask(bits)
    if bits<64 and (n & (1<<(bits-1))) ~=0 then n=n | ~mask(bits) end
    return n
end
local function evaluate(ctx,vals,signed_mode)
    local values={}
    for id,v in pairs(vals) do values[id]=v end
    local function operand(o)
        if o.kind=='integer' then return o.integer & mask(o.bits),o.bits end
        if o.kind=='value' then
            local t=assert(ctx.value_types[o.value], 'unknown value type: '..o.value)
            return assert(values[o.value], 'undefined SSA value: '..o.value) & mask(t.bits),t.bits
        end
        error('unsupported operand')
    end
    for _,i in ipairs(ctx.blocks[1]) do
        if i.op=='return' then return operand(i.operands[1]) end
        local a,w=operand(i.operands[1])
        local b=(#i.operands>=2) and operand(i.operands[2]) or 0
        local r,op=0,i.op
        if op=='constant' or op=='copy' then r=a
        elseif op=='add' then r=a+b
        elseif op=='sub' then r=a-b
        elseif op=='mul' then r=a*b
        elseif op=='neg' then r=-a
        elseif op=='not' then r=~a
        elseif op=='and' then r=a&b
        elseif op=='or' then r=a|b
        elseif op=='xor' then r=a~b
        elseif op=='shl' then assert(b<w); r=a<<b
        elseif op=='shr' then
            assert(b<w)
            if signed_mode and b>0 and signed(a,w)<0 then
                r=(a>>b) | (~0 << (w-b))
            else r=a>>b end
        elseif op=='div' or op=='mod' then
            -- These fixtures mask the numerator and use a positive divisor.
            -- Use exact integer arithmetic, including for full-width operands.
            if signed_mode then a,b=signed(a,w),signed(b,w) end
            assert(a>=0 and b>0,'division fixture left the nonnegative domain')
            r=op=='div' and a//b or a%b
        elseif op=='eq' then r=(a==b) and 1 or 0
        elseif op=='ne' then r=(a~=b) and 1 or 0
        elseif op=='select' then
            r=a~=0 and b or operand(i.operands[3])
        else error('unknown op '..op) end
        values[i.result_id]=r & mask(i.bits)
    end
    error('missing return')
end
local executions,changed_count=0,0
local widths={8,16,32,64}
local function widen(ctx,bits)
    for _,t in pairs(ctx.value_types) do if t.bits==8 then t.bits=bits end end
    for _,inst in ipairs(ctx.blocks[1]) do
        if inst.bits==8 then inst.bits=bits end
        for _,o in ipairs(inst.operands) do
            if o.kind=='integer' and o.bits==8 then o.bits=bits end
        end
    end
    return ctx
end
local function positive_fixture(t,bits)
    local ctx=widen(copy(t.ctx),bits)
    if t.name=='ext_overflowing_left_shifts_zero' then
        for _,inst in ipairs(ctx.blocks[1]) do
            if inst.op=='shl' then inst.operands[2].integer=bits//2 end
        end
    elseif t.name=='ext_left_shift_cleared_mask' then
        ctx.blocks[1][1].operands[2].integer=7 << (bits-3)
    end
    return ctx
end
local function verify_linear_ssa(ctx)
    local defined={}
    for id=1,8 do defined[id]=true end
    for _,inst in ipairs(ctx.blocks[1]) do
        for _,o in ipairs(inst.operands) do
            if o.kind=='value' then
                assert(defined[o.value], 'forward SSA reference: '..o.value)
            elseif o.kind=='integer' then
                assert(o.bits>=1 and o.bits<=64,'invalid literal width')
            end
        end
        if inst.result_id then
            assert(not defined[inst.result_id], 'double SSA definition')
            assert(ctx.value_types[inst.result_id].bits==inst.bits,'SSA result type mismatch')
            defined[inst.result_id]=true
        end
    end
end
local seen={}
for idx,t in ipairs(testcases) do
    local p=t.path
    assert(not seen[t.name],'duplicate fixture: '..t.name)
    seen[t.name]=true
    _G.lrewrite={kernel={define=function(metadata)
        assert(metadata.name==t.name and metadata.ir=='ssa',p)
    end}}
    assert(loadfile(p))()
    local run=lrewrite.kernel.run
    assert(type(run)=='function',p)
    for _,bits in ipairs(widths) do
        local before=positive_fixture(t,bits)
        local after=copy(before)
        assert(run(after)==true and not equal(before,after),
               'FAILED POSITIVE MATCH: '..t.name..' width='..bits)
        changed_count=changed_count+1
        local extra=after.next_value-before.next_value
        assert(extra==#after.new_types,'incorrect new value count: '..t.name)
        verify_linear_ssa(after)
        local unknown=copy(before)
        unknown.integer_semantics='unknown'
        local original=copy(unknown)
        assert(run(unknown)==false and equal(unknown,original),'unknown semantics accepted: '..t.name)

        -- The same well-typed graph with unrelated instructions is not a match.
        local negative=copy(before)
        for _,inst in ipairs(negative.blocks[1]) do
            if inst.result_id then
                inst.op='constant'
                inst.operands={{kind='integer',integer=0,bits=inst.bits,type_name=''}}
            end
        end
        original=copy(negative)
        assert(run(negative)==false and equal(negative,original),'nonmatching graph changed: '..t.name)

        -- Exercise the numeric guards with the same operation shapes.
        local guarded=copy(before)
        if t.name=='ext_overflowing_left_shifts_zero' then
            guarded.blocks[1][1].operands[2].integer=1
        elseif t.name=='ext_left_shift_cleared_mask' then
            guarded.blocks[1][1].operands[2].integer=1
        elseif t.name=='ext_eq_cancel_mul_constant' or t.name=='ext_ne_cancel_mul_constant' then
            for _,inst in ipairs(guarded.blocks[1]) do
                if inst.op=='mul' then inst.operands[2].integer=2 end
            end
        elseif t.name=='ext_zero_left_shift' or t.name=='ext_zero_right_shift' then
            guarded.blocks[1][1].operands[2].integer=bits
        elseif t.name=='ext_nonnegative_divide_by_power2' or t.name=='ext_nonnegative_mod_by_power2' then
            guarded.blocks[1][1].operands[2].integer=-1
        end
        if not equal(before,guarded) then
            original=copy(guarded)
            assert(run(guarded)==false and equal(guarded,original),'numeric guard ignored: '..t.name)
        end
        for step=1,80 do
            local v={}
            local samples={0,1,2,3,7,15,31,63,64,127,128,129,254,255,
                mask(bits), (1<<(bits-1)), (1<<(bits-1))-1,
                (step*2654435761) ~ (step<<17), (-step*1664525)}
            for k=1,8 do v[k]=samples[((step*(k*3+1)+k*7)%#samples)+1] & mask(bits) end
            v[6]=v[6]&1
            for _,signed_mode in ipairs({false,true}) do
                local expected=evaluate(before,v,signed_mode)
                local actual=evaluate(after,v,signed_mode)
                if expected~=actual then
                    error(string.format('DIFFERENTIAL FAILURE: %s width=%d sample=%d signed=%s expected=%d actual=%d',t.name,bits,step,tostring(signed_mode),expected,actual))
                end
                executions=executions+1
            end
        end
    end
    if idx%20==0 then io.stdout:write('validated kernels: ',idx,'\n') end
end
io.stdout:write(string.format('PASS: %d kernels, %d signed/unsigned differential executions, %d applicable width cases\n',#testcases,executions,changed_count))
'''
raw=[]
for r in CHOSEN:
    ctx,_=make_case(r)
    path='blackstone/kernels/extra/'+r['category']+'/'+r['name'].replace('_','-')+'.lua'
    raw.append({'name':'ext_'+r['name'],'path':path,'ctx':ctx})
cases='{\n'+''.join('    '+luatab(case)+',\n' for case in raw)+'}'
(tests/'test_extensions.lua').write_text(LUA_TEST.replace('__TESTCASES__',cases),encoding='utf-8')
print('Test fixtures:',len(raw),'Lua length', (tests/'test_extensions.lua').stat().st_size)
