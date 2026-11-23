from pathlib import Path
import textwrap, json
ROOT=Path(__file__).resolve().parents[1]
FX=ROOT/'fixtures'
MASK=(1<<64)-1
u=lambda x:x&MASK
cases=[]
SEEDS=[0,1,2,7,0x8000000000000000,0xffffffffffffffff,0x9e3779b97f4a7c15]
YSEEDS=[0,1,3,7,0x8000000000000001,0xFEDCBA9876543210,0x123456789abcdef0]

def val(x):return f'0x{u(x):016x}UL'

def emit(name,category,description,program,checks,expectations,extra=''):
    # checks: strings of C test snippets inside main
    header=f'''/* {name}.c - {description}
 * C11, LP64 targets (unsigned long is 64 bits). No libraries required.
 * Observable return code: 0 pass, nonzero first failed case.
 * All expected values were computed independently by the suite generator.
 */
'''
    test_main='int main(void)\n{\n'+''.join('    '+c+'\n' for c in checks)+'    return 0;\n}\n'
    text=header+'\n'+textwrap.dedent(program).strip()+'\n\n'+(extra+'\n\n' if extra else '')+test_main
    (FX/(name+'.c')).write_text(text,encoding='ascii')
    cases.append(dict(name=name,category=category,description=description,expected_opportunities=expectations,checks=len(checks)))

def unary(name,cat,desc,program,fn,opps,seeds=SEEDS):
    checks=[f'if (bench_{name}({val(x)}) != {val(fn(x))}) return {i};' for i,x in enumerate(seeds,1)]
    emit(name,cat,desc,program,checks,opps)

def binary(name,cat,desc,program,fn,opps):
    checks=[f'if (bench_{name}({val(x)}, {val(y)}) != {val(fn(x,y))}) return {i};' for i,(x,y) in enumerate(zip(SEEDS,YSEEDS),1)]
    emit(name,cat,desc,program,checks,opps)

def ternary(name,cat,desc,program,fn,opps):
    sels=[0,1,2,3,0,2,3]
    checks=[f'if (bench_{name}({val(x)}, {val(y)}, {val(s)}) != {val(fn(x,y,s))}) return {i};' for i,(x,y,s) in enumerate(zip(SEEDS,YSEEDS,sels),1)]
    emit(name,cat,desc,program,checks,opps)

binary('algebraic','local','Classical algebraic identities and cancellation.', '''
unsigned long bench_algebraic(unsigned long x, unsigned long y)
{
    unsigned long a = (x + 0UL) ^ 0UL;
    unsigned long b = (1UL * y) - 0UL;
    unsigned long c = (a | 0UL) & ~0UL;
    unsigned long d = (b & ~0UL) + 0UL;
    return (c ^ d) + ((x ^ x) | (y & 0UL));
}
''',lambda x,y:u(x^y),['add_zero','mul_one','xor_self','and_zero','sub_zero'])

unary('constant_fold','local','Fold constants introduced by simple definitions.', '''
unsigned long bench_constant_fold(unsigned long x)
{
    unsigned long a = 17UL;
    unsigned long b = 5UL;
    unsigned long c = (a + b) * (a ^ b);
    unsigned long d = ((17UL << 3) | (255UL & 127UL));
    return x ^ (c + d);
}
''',lambda x:u(x^u(((17+5)*(17^5))+((17<<3)|(255&127)))),['fold_add','fold_mul','fold_xor','fold_or','fold_and'])

binary('copy_propagation','local','Chains of copies, constants and zero-valued additions.', '''
unsigned long bench_copy_propagation(unsigned long x, unsigned long y)
{
    unsigned long a = x;
    unsigned long b = a;
    unsigned long c = b;
    unsigned long d = 37UL;
    unsigned long e = d;
    unsigned long f = e + 0UL;
    return (((c + f) ^ d) + f) ^ y;
}
''',lambda x,y:u(u((u(x+37)^37)+37)^y),['copy_propagation','constant_propagation','add_zero'])

binary('cse_local','cse','Repeated pure expressions within one basic block.', '''
unsigned long bench_cse_local(unsigned long x, unsigned long y)
{
    unsigned long a = x * 7UL;
    unsigned long b = x * 7UL;
    unsigned long c = x ^ y;
    unsigned long d = x ^ y;
    unsigned long e = x + y;
    unsigned long f = x + y;
    return (a + b) ^ ((c | d) + (e ^ f));
}
''',lambda x,y:u(u(14*x)^u(x^y)),['cse_integer_arithmetic','cse_integer_bitwise'])

ternary('cse_dominance','cse-cfg','Dominating expression reused across two branch successors.', '''
unsigned long bench_cse_dominance(unsigned long x, unsigned long y, unsigned long selector)
{
    unsigned long base = x * 13UL + y;
    unsigned long result;
    if (selector & 1UL) {
        unsigned long branch_value = x * 13UL + y;
        result = branch_value ^ base;
    } else {
        unsigned long branch_value = x * 13UL + y;
        result = branch_value + base;
    }
    return result ^ (x | 0UL);
}
''',lambda x,y,s:u((0 if s&1 else u(2*u(13*x+y)))^x),['cse_integer_arithmetic','dominators','cfg'])

binary('bitwise_factor','extended','Absorption, factoring and cancellation in integer bitwise DAGs.', '''
unsigned long bench_bitwise_factor(unsigned long x, unsigned long y)
{
    unsigned long z = 0xa5a5a5a5a5a5a5a5UL;
    unsigned long a = (x & y) | (x & z);
    unsigned long b = (x | y) & (x | z);
    unsigned long c = (x ^ y) ^ y;
    unsigned long d = x & (x | z);
    return (a ^ b) + (c ^ d);
}
''',lambda x,y:u(((x&y)|(x&0xa5a5a5a5a5a5a5a5))^((x|y)&(x|0xa5a5a5a5a5a5a5a5))),['and_absorption','ext_factor_bitwise_or_of_ands','xor_cancel'])

binary('reassociation','extended','Modular integer reassociation with constant operands.', '''
unsigned long bench_reassociation(unsigned long x, unsigned long y)
{
    unsigned long a = ((x + 17UL) + 23UL) + 41UL;
    unsigned long b = (y * 3UL) * 5UL;
    unsigned long c = (x ^ 0x55UL) ^ 0xa5UL;
    unsigned long d = (y & 0xfff0UL) & 0x0ff0UL;
    return (a ^ b) + (c ^ d);
}
''',lambda x,y:u((u(x+17+23+41)^u(y*15))+((x^0x55^0xa5)^(y&0xfff0&0x0ff0))),['add_associate_constants','mul_associate_constants','xor_associate_constants'])

ternary('phi_join','cfg','Same-constant and distinct-value paths through SSA join parameters.', '''
unsigned long bench_phi_join(unsigned long x, unsigned long y, unsigned long selector)
{
    unsigned long a;
    unsigned long b;
    if (selector & 1UL)
        a = x + 7UL;
    else
        a = x + 7UL;
    if (selector & 2UL)
        b = x ^ y;
    else
        b = x | y;
    return a + b;
}
''',lambda x,y,s:u(x+7+(x^y if s&2 else x|y)),['phi_constant','cfg','copy_propagation'])

# Loop induction: use n with seed, expect from Python
for name in ('loop_induction','loop_multilatch'):
    if name=='loop_induction':
        body='''
unsigned long bench_loop_induction(unsigned long n, unsigned long seed)
{
    unsigned long result = seed;
    unsigned long i;
    for (i = 0UL; i < n; ++i) {
        result += i * 8UL;
        result ^= i & 1UL;
    }
    return result;
}
'''
    else:
        body='''
unsigned long bench_loop_multilatch(unsigned long n, unsigned long seed)
{
    unsigned long result = seed;
    unsigned long i = 0UL;
    while (i < n) {
        result += 8UL * i;
        if ((i & 1UL) != 0UL) {
            ++i;
            continue;
        }
        ++i;
    }
    return result;
}
'''
    checks=[]
    for idx,(n,seed) in enumerate([(0,0),(1,1),(2,7),(7,0xabcdef),(16,MASK),(31,0x8000000000000000),(48,0x12345678)],1):
        result=seed
        for i in range(n):
            result=u(result+8*i)
            if name=='loop_induction':result ^= i&1
        checks.append(f'if (bench_{name}({val(n)}, {val(seed)}) != {val(result)}) return {idx};')
    emit(name,'loop','Induction-value multiply by constant with '+('a single latch.' if name=='loop_induction' else 'multiple latch edges.'),body,checks,['strength_reduction','loop_info','phi'])

binary('unsigned_rem_mask','qml-overlap','Unsigned power-of-two remainder and guarded masking.', '''
unsigned long bench_unsigned_rem_mask(unsigned long x, unsigned long y)
{
    unsigned long rem = x % (1UL << 8);
    unsigned long a = (x & 255UL) & 65535UL;
    unsigned long b = ((y | 255UL) & (y & 255UL));
    return (rem + a) ^ b;
}
''',lambda x,y:u(u((x%256)+(x&255))^(y&255)),['integer_reduce','canonicalize','blackstone_algebraic'])

unary('unsigned_wrap','semantics','Unsigned modular wraparound at 64-bit boundary.', '''
unsigned long bench_unsigned_wrap(unsigned long x)
{
    unsigned long a = (x + 0xffffffffffffffffUL) + 1UL;
    unsigned long b = (x * 3UL) + (x * 5UL);
    unsigned long c = (x ^ x) + (x - x);
    return (a ^ b) + c;
}
''',lambda x:u(x^u(8*x)),['modular_semantics','reassociation','xor_self'])

# Signed division: independent C11 truncation, no MIN/-1, no signed overflow
checks=[]
for i,(x,y) in enumerate([(-98765,3),(-7,3),(-1,7),(0,7),(1,3),(98765,7),(-9223372036854775807,11)],1):
    q=(abs(x)//abs(y)) * (-1 if (x<0)^(y<0) else 1)
    r=x-q*y
    result=u((u(r)^u(q))+(1 if x<y else 2))
    sx = f'(-{abs(x)}L)' if x<0 else f'{x}L'
    checks.append(f'if (bench_signed_near_miss({sx}, {y}L) != {val(result)}) return {i};')
emit('signed_near_miss','negative-control','Signed division/remainder: do not apply unsigned identities.', '''
unsigned long bench_signed_near_miss(long x, long y)
{
    long rem = x % y;
    long quotient = x / y;
    return (((unsigned long)rem) ^ ((unsigned long)quotient)) + (x < y ? 1UL : 2UL);
}
''',checks,['do_not_rewrite_signed_rem_as_unsigned','respect_traps'])

# dynamic unsigned divisor, guaranteed nonzero
binary('dynamic_divisor','negative-control','Runtime-dependent nonzero divisor must not become a bit mask.', '''
unsigned long bench_dynamic_divisor(unsigned long x, unsigned long y)
{
    unsigned long divisor = y | 1UL;
    unsigned long remainder = x % divisor;
    unsigned long upper = (x & 65535UL) & 32767UL;
    return remainder + (upper ^ (y << 2));
}
''',lambda x,y:u(x%(y|1)+((x&65535&32767)^u(y<<2))),['do_not_replace_dynamic_remainder','mask_near_miss'])

# volatile and calls use global state, cannot automatically assume CSE
vchecks=[]
for i,(x,start) in enumerate(zip(SEEDS,YSEEDS),1):
    final_cell=u(start+1)
    out=u((start^final_cell)+3+final_cell)
    vchecks.append(f'cell = {val(start)}; calls = 0UL;')
    vchecks.append(f'if (bench_memory_effects({val(x)}) != {val(out)} || cell != {val(final_cell)} || calls != 2UL) return {i};')
emit('memory_effects','effects','Volatile reads/writes and side-effecting calls must stay observable.', '''
static volatile unsigned long cell;
static volatile unsigned long calls;
unsigned long touch(unsigned long x)
{
    calls = calls + 1UL;
    return x ^ calls;
}
unsigned long bench_memory_effects(unsigned long x)
{
    unsigned long a = cell;
    cell = a + 1UL;
    unsigned long b = cell;
    unsigned long c = touch(x);
    unsigned long d = touch(x);
    return (a ^ b) + (c ^ d) + cell;
}
''',vchecks,['do_not_eliminate_volatile','do_not_cse_calls','preserve_effects'])

pchecks=[]
for i,(seed,other,x,alias) in enumerate([(0,12,3,True),(1,3,5,False),(MASK,7,0,True),(0x8000000000000000,17,MASK,False),(99,7,42,True),(5,8,2,False),(0x123456,0x987654,0xaabb,True)],1):
    a=seed
    b=x if alias else seed
    s0=x if alias else seed
    s1=other if alias else x
    ret=u((a^b)+x)
    pchecks.append(f'slots[0] = {val(seed)}; slots[1] = {val(other)};')
    pointer='&slots[0]' if alias else '&slots[1]'
    pchecks.append(f'if (bench_pointer_alias(&slots[0], {pointer}, {val(x)}) != {val(ret)} || slots[0] != {val(s0)} || slots[1] != {val(s1)}) return {i};')
emit('pointer_alias','effects','Aliased/nonaliased reads across intervening writes.', '''
static unsigned long slots[2];
unsigned long bench_pointer_alias(unsigned long *p, unsigned long *q, unsigned long x)
{
    unsigned long before = *p;
    *q = x;
    unsigned long after = *p;
    return (before ^ after) + *q;
}
''',pchecks,['no_memory_cse_without_alias_proof','preserve_stores'])

# Shift count valid C11 guarded by mask
binary('shift_near_miss','negative-control','Different logical shift counts from runtime operands.', '''
unsigned long bench_shift_near_miss(unsigned long x, unsigned long y)
{
    unsigned long k = (y >> 58) & 63UL;
    unsigned long a = x >> k;
    unsigned long b = x << ((k + 1UL) & 63UL);
    return (a ^ b) + (x & y);
}
''',lambda x,y:u(((x >> ((y>>58)&63))^u(x << ((((y>>58)&63)+1)&63)))+(x&y)),['valid_shift_bounds','no_invalid_strength_reduction'])

# 3 generated benchmark density programs: 4 functions by 64 iterations
SALT=0xD1B54A32D192ED03
for kind in ('dense','sparse','inert'):
    funs=[]; checks=[]
    for fi in range(4):
        body=['unsigned long bench_'+kind+'_'+str(fi)+'(unsigned long x)','{']
        for step in range(64):
            salt=u(SALT*(fi+1)*(step+1))
            if kind=='dense' or (kind=='sparse' and step%8==0):
                body.append('    x = ((x + 0UL) ^ 0UL) * 1UL;')
            else:
                body.append(f'    x = (x * 3UL) ^ {val(salt)};')
        body.extend(['    return x;','}']);funs.append('\n'.join(body))
        for seed in (0,1,MASK,0x9e3779b97f4a7c15,0x8000000000000000):
            out=seed
            for step in range(64):
                salt=u(SALT*(fi+1)*(step+1))
                if kind=='dense' or (kind=='sparse' and step%8==0):
                    pass
                else:out=u(u(out*3)^salt)
            checks.append(f'if (bench_{kind}_{fi}({val(seed)}) != {val(out)}) return {len(checks)+1};')
    emit('stress_'+kind,'stress','Generated 4x64 basic-block operations; '+kind+' optimizer match density.', '\n\n'.join(funs), checks, ['algebraic-density-'+kind,'compile-overhead-control'])

# Mix small literals with bit 31 set and observable effects.
salts = [0, 1, 127, 255, 65535, 0x12345678, 0x7fffffff, 0x80000000, 0xffffffff]
body = ['static volatile unsigned long observed;',
        'unsigned long bench_sandslab_literals(unsigned long x)', '{']
for salt in salts:
    body += [f'    observed = x + {val(salt)};', '    x = observed ^ (x >> 3);']
body += ['    return x;', '}']
def literals(x):
    for salt in salts:
        x = u(x + salt) ^ (x >> 3)
    return x
unary('sandslab_literals', 'machine-code', 'Narrow literal materialization across volatile accesses.',
      '\n'.join(body), literals, ['narrow_immediate', 'preserve_flags_and_memory'])

unary('sandslab_wide', 'machine-code', 'Wide immediates and relocatable data address.', '''
static volatile unsigned long slot;
unsigned long bench_sandslab_wide(unsigned long x)
{
    slot = x + 0x100000000UL;
    return (slot ^ 0xffffffffffffffffUL) + 0x8000000000000000UL;
}
''', lambda x: u((u(x + (1 << 32)) ^ MASK) + (1 << 63)),
      ['keep_wide_immediate', 'preserve_relocation'])

(ROOT/'manifest.json').write_text(json.dumps({'schema':'cephyr-blackstone-c11-fixtures.v1','assumptions':['C11','LP64 (unsigned long 64 bits)','no standard library required','no undefined signed overflow or invalid shifts'],'fixtures':cases},indent=2)+'\n')
print(f'Generated {len(cases)} .c programs, {sum(c["checks"] for c in cases)} lines of test statements')
