#!/usr/bin/env python3
"""Regenerate independent, transactional Blackstone Lua optimization kernels.

No repository-local runtime changes or external Lua modules are required.
The generated kernels execute on the Blackstone SSA snapshot protocol.
"""
from __future__ import annotations
import json, pathlib
R=[]
def N(op,*args):return [op,*args]
def K(name):return {'capture':name}
def F(code):return {'compute':code}
def add(name,source,target,category='algebraic',when='true',doc=None,values=None):
    if any(x['name']==name for x in R): raise ValueError('duplicate '+name)
    R.append(dict(name=name,source=source,target=target,category=category,when=when,doc=doc or name.replace('_',' '),values=values or {}))
# Arithmetic identities in modular integer arithmetic.
add('zero_minus_x',N('sub',0,'X'),N('neg','X'))
add('multiply_negative_one',N('mul','X',-1),N('neg','X'))
add('xor_negative_one',N('xor','X',-1),N('not','X'),'bitwise')
add('double_add_to_shift',N('add','X','X'),N('shl','X',1),when='bits >= 2')
add('negate_difference',N('neg',N('sub','X','Y')),N('sub','Y','X'))
add('minus_negation_cancel',N('sub',0,N('neg','X')),'X')
add('subtract_own_sum',N('sub','X',N('add','X','Y')),N('neg','Y'))
add('subtract_both_negated',N('sub',N('neg','X'),N('neg','Y')),N('sub','Y','X'))
add('negated_self_difference',N('sub',N('neg','X'),'X'),N('mul','X',-2))
add('subtract_negative_self',N('sub','X',N('neg','X')),N('mul','X',2))
add('add_two_negatives',N('add',N('neg','X'),N('neg','Y')),N('neg',N('add','X','Y')))
add('negate_double',N('neg',N('add','X','X')),N('mul','X',-2))
add('negate_sum_with_negative_left',N('neg',N('add',N('neg','X'),'Y')),N('sub','X','Y'))
add('negate_sum_with_negative_right',N('neg',N('add','X',N('neg','Y'))),N('sub','Y','X'))
add('multiply_two_negatives',N('mul',N('neg','X'),N('neg','Y')),N('mul','X','Y'))
add('negate_minus_one_product',N('neg',N('mul','X',-1)),'X')
add('subtract_negative_double',N('sub',N('neg','X'),N('neg','X')),0)
add('negate_subtract_from_zero',N('neg',N('sub',0,'X')),'X')
# Algebra over bitvectors; identities are valid independently in each bit.
add('xor_own_complement',N('xor','X',N('not','X')),-1,'bitwise')
add('and_own_xor_complement',N('and','X',N('xor','X',-1)),0,'bitwise')
add('or_own_xor_complement',N('or','X',N('xor','X',-1)),-1,'bitwise')
add('xor_all_ones_twice',N('xor',N('xor','X',-1),-1),'X','bitwise')
add('not_xor_ones',N('not',N('xor','X',-1)),'X','bitwise')
add('xor_not_ones',N('xor',N('not','X'),-1),'X','bitwise')
add('or_with_xor_absorption',N('or','X',N('xor','X','Y')),N('or','X','Y'),'bitwise')
add('and_with_xor_reduction',N('and','X',N('xor','X','Y')),N('and','X',N('not','Y')),'bitwise')
add('xor_with_and_reduction',N('xor','X',N('and','X','Y')),N('and','X',N('not','Y')),'bitwise')
add('xor_with_or_reduction',N('xor','X',N('or','X','Y')),N('and',N('not','X'),'Y'),'bitwise')
add('and_not_or_contradiction',N('and','X',N('not',N('or','X','Y'))),0,'bitwise')
add('or_not_and_tautology',N('or','X',N('not',N('and','X','Y'))),-1,'bitwise')
add('and_or_not_absorption',N('and','X',N('or',N('not','X'),'Y')),N('and','X','Y'),'bitwise')
add('or_and_not_absorption',N('or','X',N('and',N('not','X'),'Y')),N('or','X','Y'),'bitwise')
add('or_disjoint_and_partition',N('or',N('and','X','Y'),N('and','X',N('not','Y'))),'X','bitwise')
add('xor_disjoint_and_partition',N('xor',N('and','X','Y'),N('and','X',N('not','Y'))),'X','bitwise')
add('and_disjoint_or_partition',N('and',N('or','X','Y'),N('or','X',N('not','Y'))),'X','bitwise')
add('xor_disjoint_or_partition',N('xor',N('or','X','Y'),N('or','X',N('not','Y'))),N('not','X'),'bitwise')
add('or_disjoint_and_other',N('or',N('and','X','Y'),N('and',N('not','X'),'Y')),'Y','bitwise')
add('xor_disjoint_and_other',N('xor',N('and','X','Y'),N('and',N('not','X'),'Y')),'Y','bitwise')
add('and_disjoint_or_other',N('and',N('or','X','Y'),N('or',N('not','X'),'Y')),'Y','bitwise')
add('xor_disjoint_or_other',N('xor',N('or','X','Y'),N('or',N('not','X'),'Y')),N('not','Y'),'bitwise')
add('and_with_implicated_and',N('and',N('or','X','Y'),N('and','X','Z')),N('and','X','Z'),'bitwise')
add('or_with_implicated_or',N('or',N('and','X','Y'),N('or','X','Z')),N('or','X','Z'),'bitwise')
add('xor_two_complements',N('xor',N('not','X'),N('not','Y')),N('xor','X','Y'),'bitwise')
add('xor_and_or_equals_xor',N('xor',N('and','X','Y'),N('or','X','Y')),N('xor','X','Y'),'bitwise')
add('and_xor_or_equals_xor',N('and',N('xor','X','Y'),N('or','X','Y')),N('xor','X','Y'),'bitwise')
add('or_xor_and_equals_or',N('or',N('xor','X','Y'),N('and','X','Y')),N('or','X','Y'),'bitwise')
add('and_not_complement_or_zero',N('and',N('not','X'),N('not',N('not','X'))),0,'bitwise')
add('or_not_complement_or_ones',N('or',N('not','X'),N('not',N('not','X'))),-1,'bitwise')
add('not_xor_same',N('not',N('xor','X','X')),-1,'bitwise')
add('or_disjoint_and_xor',N('or',N('xor','X','Y'),N('and','X','Y')),N('or','X','Y'),'bitwise')
add('subtract_or_and',N('sub',N('or','X','Y'),N('and','X','Y')),N('xor','X','Y'),'bitwise')
add('subtract_or_xor',N('sub',N('or','X','Y'),N('xor','X','Y')),N('and','X','Y'),'bitwise')
add('add_xor_and',N('add',N('xor','X','Y'),N('and','X','Y')),N('or','X','Y'),'bitwise')
# Equality simplifications valid for modular integers, no signedness assumptions.
for op in ['eq','ne']:
    is_eq=op=='eq'; truth=1 if is_eq else 0
    for kind in ('sub','xor'):
        add(f'{op}_{kind}_zero',N(op,N(kind,'X','Y'),0),N(op,'X','Y'),'comparison')
    add(f'{op}_add_own_operand',N(op,N('add','X','Y'),'X'),N(op,'Y',0),'comparison')
    add(f'{op}_sub_own_operand',N(op,N('sub','X','Y'),'X'),N(op,'Y',0),'comparison')
    add(f'{op}_xor_own_operand',N(op,'X',N('xor','X','Y')),N(op,'Y',0),'comparison')
    add(f'{op}_negative_zero',N(op,N('neg','X'),0),N(op,'X',0),'comparison')
    add(f'{op}_complement_ones',N(op,N('not','X'),-1),N(op,'X',0),'comparison')
    add(f'{op}_complement_zero',N(op,N('not','X'),0),N(op,'X',-1),'comparison')
    add(f'{op}_own_complement',N(op,'X',N('not','X')),0 if is_eq else 1,'comparison')
    add(f'{op}_two_negatives',N(op,N('neg','X'),N('neg','Y')),N(op,'X','Y'),'comparison')
    add(f'{op}_two_complements',N(op,N('not','X'),N('not','Y')),N(op,'X','Y'),'comparison')
    for kind in ('add','sub','xor','mul'):
        # Mul-by-odd is invertible mod 2^width, other ops always cancellative.
        when='(V("K") & 1) == 1' if kind=='mul' else 'true'
        add(f'{op}_cancel_{kind}_constant',N(op,N(kind,'X',K('K')),N(kind,'Y',K('K'))),N(op,'X','Y'),'comparison',when)
    for kind in ('add','mul','and','or','xor'):
        add(f'{op}_{kind}_commuted',N(op,N(kind,'X','Y'),N(kind,'Y','X')),truth,'comparison')
    add(f'{op}_duplicate_difference',N(op,N('sub','X','Y'),N('sub','X','Y')),truth,'comparison')
# Select rewrites (conditions are exactly one bit; root values are at least two bits).
add('select_equal_choose_right',N('select',N('eq','X','Y'),'X','Y'),'Y','select')
add('select_equal_choose_left',N('select',N('eq','X','Y'),'Y','X'),'X','select')
add('select_unequal_choose_left',N('select',N('ne','X','Y'),'X','Y'),'X','select')
add('select_unequal_choose_right',N('select',N('ne','X','Y'),'Y','X'),'Y','select')
add('select_zero_if_equal_else_self',N('select',N('eq','X',0),0,'X'),'X','select')
add('select_self_if_equal_else_zero',N('select',N('eq','X',0),'X',0),0,'select')
add('select_self_if_nonzero_else_zero',N('select',N('ne','X',0),'X',0),'X','select')
add('select_zero_if_nonzero_else_self',N('select',N('ne','X',0),0,'X'),0,'select')
add('select_nested_then_same_condition',N('select','C',N('select','C','A','B'),'D'),N('select','C','A','D'),'select')
add('select_nested_else_same_condition',N('select','C','A',N('select','C','B','D')),N('select','C','A','D'),'select')
add('select_both_nested_same_condition',N('select','C',N('select','C','A','B'),N('select','C','D','E')),N('select','C','A','E'),'select')
add('select_negated_condition',N('select',N('not','C'),'A','B'),N('select','C','B','A'),'select')
add('select_xor_one_condition',N('select',N('xor','C',1),'A','B'),N('select','C','B','A'),'select')
add('select_equal_zero_condition',N('select',N('eq','C',0),'A','B'),N('select','C','B','A'),'select')
add('select_unequal_zero_condition',N('select',N('ne','C',0),'A','B'),N('select','C','A','B'),'select')
add('select_equal_one_condition',N('select',N('eq','C',1),'A','B'),N('select','C','A','B'),'select')
add('select_unequal_one_condition',N('select',N('ne','C',1),'A','B'),N('select','C','B','A'),'select')
add('select_nested_negation_then',N('select','C',N('select',N('not','C'),'A','B'),'D'),N('select','C','B','D'),'select')
add('select_nested_negation_else',N('select','C','A',N('select',N('not','C'),'B','D')),N('select','C','A','B'),'select')
add('select_both_nested_negated',N('select','C',N('select',N('not','C'),'A','B'),N('select',N('not','C'),'D','E')),N('select','C','B','D'),'select')
# Distribution and factoring. Some rewrite to a smaller instruction count;
# others expose constant folding and simplify dominance-aware CSE.
add('factor_bitwise_or_of_ands',N('or',N('and','X','Y'),N('and','X','Z')),N('and','X',N('or','Y','Z')),'factoring')
add('factor_bitwise_xor_of_ands',N('xor',N('and','X','Y'),N('and','X','Z')),N('and','X',N('xor','Y','Z')),'factoring')
add('factor_bitwise_and_of_ors',N('and',N('or','X','Y'),N('or','X','Z')),N('or','X',N('and','Y','Z')),'factoring')
add('factor_add_multiplications',N('add',N('mul','X','Y'),N('mul','X','Z')),N('mul','X',N('add','Y','Z')),'factoring')
add('factor_sub_multiplications',N('sub',N('mul','X','Y'),N('mul','X','Z')),N('mul','X',N('sub','Y','Z')),'factoring')
add('factor_or_and_constants',N('or',N('and','X',K('K1')),N('and','X',K('K2'))),N('and','X',F('V("K1") | V("K2")')),'factoring')
add('factor_xor_and_constants',N('xor',N('and','X',K('K1')),N('and','X',K('K2'))),N('and','X',F('V("K1") ~ V("K2")')),'factoring')
add('factor_and_or_constants',N('and',N('or','X',K('K1')),N('or','X',K('K2'))),N('or','X',F('V("K1") & V("K2")')),'factoring')
add('factor_xor_or_constants',N('xor',N('or','X',K('K1')),N('or','X',K('K2'))),N('and',N('not','X'),F('V("K1") ~ V("K2")')),'factoring')
add('factor_or_and_shared_const',N('or',N('and','X',K('K')),N('and','Y',K('K'))),N('and',N('or','X','Y'),K('K')),'factoring')
add('factor_xor_and_shared_const',N('xor',N('and','X',K('K')),N('and','Y',K('K'))),N('and',N('xor','X','Y'),K('K')),'factoring')
add('factor_and_or_shared_const',N('and',N('or','X',K('K')),N('or','Y',K('K'))),N('or',N('and','X','Y'),K('K')),'factoring')
add('and_or_not_shared',N('and',N('or','X','Y'),N('not','X')),N('and','Y',N('not','X')),'factoring')
add('or_and_not_shared',N('or',N('and','X','Y'),N('not','X')),N('or','Y',N('not','X')),'factoring')
# Constant reassociation with immediate constants. K captures raw bit-patterns.
add('subtract_add_constants',N('sub',N('add','X',K('K1')),K('K2')),N('add','X',F('V("K1")-V("K2")')),'reassociation')
add('add_subtract_constants',N('add',N('sub','X',K('K1')),K('K2')),N('add','X',F('V("K2")-V("K1")')),'reassociation')
add('subtract_subtract_constants',N('sub',N('sub','X',K('K1')),K('K2')),N('sub','X',F('V("K1")+V("K2")')),'reassociation')
add('add_const_minus_value',N('add',N('sub',K('K1'),'X'),K('K2')),N('sub',F('V("K1")+V("K2")'),'X'),'reassociation')
add('subtract_const_minus_value',N('sub',N('sub',K('K1'),'X'),K('K2')),N('sub',F('V("K1")-V("K2")'),'X'),'reassociation')
add('subtract_sum_by_self_constant',N('sub','X',N('add','X',K('K'))),F('-V("K")'),'reassociation')
add('multiply_add_common_constants',N('add',N('mul','X',K('K1')),N('mul','X',K('K2'))),N('mul','X',F('V("K1")+V("K2")')),'reassociation')
add('multiply_sub_common_constants',N('sub',N('mul','X',K('K1')),N('mul','X',K('K2'))),N('mul','X',F('V("K1")-V("K2")')),'reassociation')
# Shift algebra: arithmetic right shift may be logical or sign extending;
# only identities valid for both are included.
add('multiply_by_power_of_two',N('mul','X',K('K')),N('shl','X',F('ilog2(V("K"))')),'shift',when='power2(V("K")) and V("K") >= 2 and ilog2(V("K")) < bits')
add('combine_left_shifts',N('shl',N('shl','X',K('K1')),K('K2')),N('shl','X',F('V("K1")+V("K2")')),'shift',when='validshift(V("K1"),bits) and validshift(V("K2"),bits) and V("K1")+V("K2") < bits')
add('overflowing_left_shifts_zero',N('shl',N('shl','X',K('K1')),K('K2')),0,'shift',when='validshift(V("K1"),bits) and validshift(V("K2"),bits) and V("K1")+V("K2") >= bits',values={'K1':5,'K2':5})
add('combine_right_shifts',N('shr',N('shr','X',K('K1')),K('K2')),N('shr','X',F('V("K1")+V("K2")')),'shift',when='validshift(V("K1"),bits) and validshift(V("K2"),bits) and V("K1")+V("K2") < bits')
add('zero_left_shift',N('shl',0,K('K')),0,'shift',when='validshift(V("K"),bits)')
add('zero_right_shift',N('shr',0,K('K')),0,'shift',when='validshift(V("K"),bits)')
add('clear_below_shift',N('and',N('shl','X',K('K')),K('MASK')),0,'shift',when='validshift(V("K"),bits) and (V("MASK") & ((1 << V("K"))-1)) == V("MASK")',values={'K':3,'MASK':7})
add('right_shift_cleared_mask',N('shr',N('and','X',K('MASK')),K('K')),0,'shift',when='validshift(V("K"),bits) and nonnegative(V("MASK"),bits) and (V("MASK") >> V("K")) == 0',values={'K':4,'MASK':7})
add('left_shift_cleared_mask',N('shl',N('and','X',K('MASK')),K('K')),0,'shift',when='validshift(V("K"),bits) and ((V("MASK") << V("K")) & widthmask(bits)) == 0',values={'K':3,'MASK':224})
add('shift_right_then_left_mask',N('shl',N('shr','X',K('K')),K('K')),N('and','X',F('~((1<<V("K"))-1)')),'shift',when='validshift(V("K"),bits)')
for op in ('and','or','xor'):
    add('distribute_shl_'+op,N(op,N('shl','X',K('K')),N('shl','Y',K('K'))),N('shl',N(op,'X','Y'),K('K')),'shift',when='validshift(V("K"),bits)')
    add('distribute_shr_'+op,N(op,N('shr','X',K('K')),N('shr','Y',K('K'))),N('shr',N(op,'X','Y'),K('K')),'shift',when='validshift(V("K"),bits)')
add('mask_before_left_shift',N('shl',N('and','X',K('MASK')),K('K')),N('and',N('shl','X',K('K')),F('V("MASK") << V("K")')),'shift',when='validshift(V("K"),bits)')
add('mask_before_right_shift',N('shr',N('and','X',K('MASK')),K('K')),N('and',N('shr','X',K('K')),F('V("MASK") >> V("K")')),'shift',when='validshift(V("K"),bits) and nonnegative(V("MASK"),bits)')
add('nonnegative_divide_by_power2',N('div',N('and','X',K('MASK')),K('K')),N('shr',N('and','X',K('MASK')),F('ilog2(V("K"))')),'shift',when='power2(V("K")) and V("K")>1 and ilog2(V("K")) < bits-1 and nonnegative(V("MASK"),bits)',values={'K':4,'MASK':63})
add('nonnegative_mod_by_power2',N('mod',N('and','X',K('MASK')),K('K')),N('and',N('and','X',K('MASK')),F('V("K")-1')),'shift',when='power2(V("K")) and V("K")>1 and ilog2(V("K")) < bits-1 and nonnegative(V("MASK"),bits)',values={'K':4,'MASK':63})
# Count and exports
if __name__=='__main__': print('TOTAL',len(R));from collections import Counter;print(Counter(x['category'] for x in R))

# Balanced, consciously curated selection of 120 rules. Additional candidates
# remain in this source as a research backlog, but are not shipped as kernels.
OMIT = {
 'algebraic': {'add_two_negatives'},
 'bitwise': {'and_not_complement_or_zero','or_not_complement_or_ones','not_xor_same',
             'xor_two_complements','subtract_or_xor','and_with_implicated_and',
             'or_with_implicated_or','and_xor_or_equals_xor','and_or_not_absorption',
             'or_and_not_absorption'},
 'comparison': {'eq_duplicate_difference','ne_duplicate_difference',
   'eq_add_commuted','ne_add_commuted','eq_mul_commuted','ne_mul_commuted',
   'eq_and_commuted','ne_and_commuted','eq_or_commuted','ne_or_commuted',
   'eq_xor_commuted','ne_xor_commuted','eq_add_own_operand','ne_add_own_operand',
   'eq_sub_own_operand'},
 'select': {'select_equal_one_condition','select_unequal_one_condition',
            'select_both_nested_negated'},
 'shift': set(),
 'factoring': {'factor_or_and_shared_const','factor_xor_and_shared_const',
              'factor_and_or_shared_const','factor_add_multiplications',
              'factor_sub_multiplications','and_or_not_shared',
              'or_and_not_shared','factor_xor_or_constants'},
 'reassociation': set()
}
CHOSEN=[r for r in R if r['name'] not in OMIT.get(r['category'],set())]
# 157 initial - 1-10-15-3-0-8 = 120
assert len(CHOSEN)==120, (len(CHOSEN),{p:sum(x['category']==p for x in CHOSEN) for p in set(x['category'] for x in CHOSEN)})

def lua_expr(x):
    if isinstance(x,str):return json.dumps(x)
    if isinstance(x,int):return str(x)
    if isinstance(x,dict):
        if 'capture' in x:return '{capture='+json.dumps(x['capture'])+'}'
        if 'compute' in x:return '{calc=function(V,bits) return '+x['compute']+' end}'
        raise ValueError(x)
    if isinstance(x,list):return '{'+','.join(map(lua_expr,x))+'}'
    raise ValueError(x)

LUA_ENGINE=r'''
-- Completely self-contained SSA rewrite; no global rule table or monkey patch.
local function widthmask(bits)
    return bits == 64 and -1 or ((1 << bits) - 1)
end
local function normalized(n,bits) return n & widthmask(bits) end
local function literal(n,bits)
    n = normalized(n,bits)
    if bits > 1 and bits < 64 and ((n & (1 << (bits-1))) ~= 0) then
        n = n | ~widthmask(bits)
    end
    return {kind="integer", integer=n, bits=bits, type_name=""}
end
local function validshift(n,bits)
    return n >= 0 and n < bits and bits >= 2
end
local function power2(n)
    return n > 0 and (n & (n-1)) == 0
end
local function ilog2(n)
    local k = 0
    while n > 1 do n = n >> 1; k = k + 1 end
    return k
end
local function nonnegative(n,bits)
    return bits == 64 and n >= 0 or (n & (1 << (bits-1))) == 0
end
local pattern = __PATTERN__
local replacement = __REPLACEMENT__
local guard = function(V,bits) return __GUARD__ end
local needs_temporary = __NEEDS_TEMPORARY__
local commutative = {add=true,mul=true,["and"]=true,["or"]=true,xor=true,eq=true,ne=true}
local comparisons = {eq=true,ne=true,lt=true,le=true,gt=true,ge=true}
local function same(a,b)
    if not a or not b or a.kind ~= b.kind then return false end
    if a.kind == "value" then return a.value == b.value end
    return a.kind == "integer" and a.bits == b.bits
        and (a.type_name or "") == (b.type_name or "")
        and normalized(a.integer,a.bits) == normalized(b.integer,b.bits)
end
lrewrite.kernel.run = function(ctx)
    if ctx.integer_semantics ~= "modular" then return false end
    assert(type(ctx.blocks)=="table" and type(ctx.value_types)=="table",
           "extended kernels require the Firestone typed SSA snapshot")
    local defs, originals, owners = {}, {}, {}
    local block_ids={}
    for id in pairs(ctx.blocks) do block_ids[#block_ids+1]=id end
    table.sort(block_ids)
    for _,id in ipairs(block_ids) do
        for _,i in ipairs(ctx.blocks[id]) do
            originals[#originals+1]=i
            owners[i]=id
            if i.result_id ~= nil then defs[i.result_id]=i end
        end
    end
    local function type_of(o)
        return o.kind == "value" and ctx.value_types[o.value] or o
    end
    local function builtin(t)
        return type(t)=="table" and t.kind=="integer"
           and (t.type_name or "")=="" and math.type(t.bits)=="integer"
           and t.bits>=1 and t.bits<=64
    end
    local function valid(i)
        if i.result_id == nil or not builtin(i) then return false end
        if i.op == "select" then
            return #i.operands==3 and builtin(type_of(i.operands[1]))
                and type_of(i.operands[1]).bits==1
                and builtin(type_of(i.operands[2])) and builtin(type_of(i.operands[3]))
                and type_of(i.operands[2]).bits==i.bits
                and type_of(i.operands[3]).bits==i.bits
        end
        if comparisons[i.op] then
            return i.bits==1 and #i.operands==2
                and builtin(type_of(i.operands[1])) and builtin(type_of(i.operands[2]))
                and type_of(i.operands[1]).bits==type_of(i.operands[2]).bits
        end
        if i.op=="not" or i.op=="neg" then
            return #i.operands==1 and builtin(type_of(i.operands[1]))
                and type_of(i.operands[1]).bits==i.bits
        end
        if i.op=="add" or i.op=="sub" or i.op=="mul" or i.op=="div"
           or i.op=="mod" or i.op=="and" or i.op=="or" or i.op=="xor"
           or i.op=="shl" or i.op=="shr" then
            return #i.operands==2 and builtin(type_of(i.operands[1]))
                and builtin(type_of(i.operands[2]))
                and type_of(i.operands[1]).bits==i.bits
                and type_of(i.operands[2]).bits==i.bits
        end
        return false
    end
    local function bind(bindings,key,operand,accept)
        local previous=bindings[key]
        if previous then return same(previous,operand) and accept() end
        bindings[key]=operand
        if accept() then return true end
        bindings[key]=nil
        return false
    end
    local function match_operand(o,p,b,c,accept)
        if type(p)=="string" then
            return builtin(type_of(o)) and bind(b,p,o,accept)
        end
        if type(p)=="number" then
            return o.kind=="integer" and builtin(o)
                and normalized(o.integer,o.bits)==normalized(p,o.bits) and accept()
        end
        if type(p)~="table" then return false end
        if p.capture then
            return o.kind=="integer" and builtin(o) and bind(c,p.capture,o,accept)
        end
        if o.kind~="value" then return false end
        local i=defs[o.value]
        if i==nil or not valid(i) or i.op~=p[1] then return false end
        if #i.operands~=#p-1 then return false end
        -- A match is provisional until siblings and the guard also accept it.
        local function attempt(n,flip)
            if n==#p then return accept() end
            local j=flip and (3-n) or n
            return match_operand(i.operands[j],p[n+1],b,c,function()
                return attempt(n+1,flip)
            end)
        end
        return attempt(1,false) or (#i.operands==2 and commutative[i.op] and attempt(1,true))
    end
    local function run_one(i)
        if not valid(i) or i.op~=pattern[1] then return false end
        local bits=i.bits
        if needs_temporary and bits==1 then return false end
        local b,c={},{}
        local comparison_operand_bits = comparisons[i.op]
            and type_of(i.operands[1]).bits or bits
        local function V(name)
            local v=c[name]
            assert(v, "missing literal capture: "..name)
            return normalized(v.integer,v.bits)
        end
        if not match_operand({kind="value",value=i.result_id},pattern,b,c,function()
            return guard(V,bits)
        end) then return false end
        local block=ctx.blocks[owners[i]]
        local function build(p,is_root,operand_bits)
            if type(p)=="string" then return assert(b[p],"missing capture: "..p) end
            if type(p)=="number" then return literal(p,operand_bits or bits) end
            if p.capture then return assert(c[p.capture],"missing literal: "..p.capture) end
            if p.calc then return literal(p.calc(V,bits),operand_bits or bits) end
            local op=p[1]
            local operands={}
            for n=2,#p do
                local child_bits = (comparisons[op] and comparison_operand_bits)
                    or (op=="select" and n==2 and 1) or bits
                operands[#operands+1]=build(p[n],false,child_bits)
            end
            if is_root then
                i.op,i.operands,i.incoming=op,operands,{}
                return nil
            end
            local id=ctx.next_value
            assert(math.type(id)=="integer" and id>=0,"invalid next SSA value ID")
            ctx.next_value=id+1
            ctx.new_types[#ctx.new_types+1]=bits
            ctx.value_types[id]={kind="integer",bits=bits,type_name=""}
            local temp={result_id=id,kind="integer",bits=bits,type_name="",
                        op=op,operands=operands,targets={},incoming={},annotation="blackstone.ext"}
            local pos
            for n,existing in ipairs(block) do if existing==i then pos=n;break end end
            assert(pos,"root instruction disappeared")
            table.insert(block,pos,temp)
            defs[id]=temp
            owners[temp]=owners[i]
            return {kind="value",value=id}
        end
        if type(replacement)=="table" and not replacement.capture
           and not replacement.calc then
            build(replacement,true,bits)
        else
            local operand=build(replacement,false,bits)
            i.op=operand.kind=="integer" and "constant" or "copy"
            i.operands={operand}
            i.incoming={}
        end
        return true
    end
    local changed=false
    for _,i in ipairs(originals) do
        if run_one(i) then changed=true end
    end
    return changed
end
'''

def produce(dest):
    dest=pathlib.Path(dest)
    for rule in CHOSEN:
        dir=dest/'blackstone'/'kernels'/'extra'/rule['category']
        dir.mkdir(parents=True,exist_ok=True)
        metadata=f'''-- Generated by blackstone/tools/extended/generate.py. Do not edit.\nlrewrite.kernel.define {{\n    name={json.dumps('ext_'+rule['name'])},\n    ir="ssa", phase={json.dumps(rule['category']+'_optimization')},\n    requires={{"def_use"}}, preserves={{"cfg","dominators"}},\n    documentation={json.dumps(rule['doc']+'. Only typed builtin modular integers; transformations retain effectful operand definitions and the CFG.')}\n}}\n'''
        target=rule['target']
        needs_temporary=isinstance(target,list) and any(isinstance(arg,list) for arg in target[1:])
        code=LUA_ENGINE.replace('__PATTERN__',lua_expr(rule['source'])).replace('__REPLACEMENT__',lua_expr(target)).replace('__GUARD__',rule['when']).replace('__NEEDS_TEMPORARY__','true' if needs_temporary else 'false')
        (dir/(rule['name'].replace('_','-')+'.lua')).write_text(metadata+code,encoding='utf-8')
    data=[{k:v for k,v in rule.items() if k!='values'} for rule in CHOSEN]
    docs=dest/'blackstone'/'docs'
    docs.mkdir(parents=True,exist_ok=True)
    (docs/'extended-kernels.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    return data

if __name__=='__main__':
    import sys
    dest=sys.argv[1] if len(sys.argv)>1 else pathlib.Path(__file__).resolve().parents[3]
    produce(dest)
    print('WROTE',len(CHOSEN),'KERNELS')
