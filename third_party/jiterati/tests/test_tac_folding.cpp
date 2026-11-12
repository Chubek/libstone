/** @file test_tac_folding.cpp @brief Fixed-width TAC folding and arithmetic-trap regressions. */
#include "IR/IR.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
using namespace jiterati::ir;

static TACFunction make(TACOpcode op, Type type, std::int64_t a, std::int64_t b, bool comparison=false) {
    TACFunction fn; fn.name="fold"; fn.return_type=comparison?Type::bool_ty():type;
    auto value=fn.make_temp(fn.return_type);
    auto& block=fn.add_block("entry");
    TACInstruction inst; inst.opcode=op; inst.type=fn.return_type; inst.result=value;
    inst.operands={TACValue::constant_value(type,a)};
    if (op!=TACOpcode::Neg && op!=TACOpcode::Not) inst.operands.push_back(TACValue::constant_value(type,b));
    block.instructions.push_back(inst);
    TACInstruction ret; ret.opcode=TACOpcode::Return; ret.operands={value}; block.instructions.push_back(ret);
    return fn;
}
static void folded(TACOpcode op, Type type, std::int64_t a, std::int64_t b, std::int64_t expected, bool compare=false) {
    auto fn=make(op,type,a,b,compare);
    CHECK(peephole(fn));
    auto const& result=fn.blocks[0].instructions[0];
    CHECK(result.opcode==TACOpcode::Move);
    auto const& immediate=result.operands[0].constant;
    if (compare) CHECK(std::get<bool>(immediate)==bool(expected));
    else CHECK(std::get<std::int64_t>(immediate)==expected);
    CHECK(fn.blocks[0].instructions[1].operands[0].constant==immediate);
    CHECK(!peephole(fn));
}
static void unchanged(TACOpcode op, Type type, std::int64_t a, std::int64_t b) {
    auto fn=make(op,type,a,b); CHECK(!peephole(fn)); CHECK(fn.blocks[0].instructions[0].opcode==op);
}
int main() {
    auto i32=Type::i32(), i64=Type::i64();
    folded(TACOpcode::Add,i32,INT32_MAX,1,INT32_MIN);
    folded(TACOpcode::Add,i64,INT64_MAX,1,INT64_MIN);
    folded(TACOpcode::Sub,i64,INT64_MIN,1,INT64_MAX);
    folded(TACOpcode::Mul,i32,INT32_MAX,2,-2);
    folded(TACOpcode::Mul,i64,INT64_MIN,-1,INT64_MIN);
    folded(TACOpcode::Neg,i64,INT64_MIN,0,INT64_MIN);
    folded(TACOpcode::Neg,i32,INT32_MIN,0,INT32_MIN);
    folded(TACOpcode::Not,i32,0,0,-1);
    folded(TACOpcode::Shl,i64,-1,63,INT64_MIN);
    folded(TACOpcode::Shl,i32,-1,31,INT32_MIN);
    folded(TACOpcode::AShr,i64,INT64_MIN,63,-1);
    folded(TACOpcode::AShr,i32,0xffffffffLL,31,-1);
    folded(TACOpcode::LShr,i64,-1,1,INT64_MAX);
    folded(TACOpcode::LShr,i32,-1,1,INT32_MAX);
    folded(TACOpcode::UDiv,i64,-1,2,INT64_MAX);
    folded(TACOpcode::URem,i32,-1,2,1);
    folded(TACOpcode::SDiv,i32,-7,3,-2);
    folded(TACOpcode::SRem,i32,-7,3,-1);
    folded(TACOpcode::SLt,i32,0xffffffffLL,0,1,true);
    folded(TACOpcode::ULt,i32,-1,0,0,true);
    folded(TACOpcode::Eq,i32,0x100000000LL,0,1,true);
    for (auto t : {i32,i64}) {
        auto minimum=t==i32?std::int64_t(INT32_MIN):INT64_MIN;
        for (auto op : {TACOpcode::SDiv,TACOpcode::SRem}) {
            unchanged(op,t,minimum,-1); unchanged(op,t,42,0);
        }
        unchanged(TACOpcode::UDiv,t,42,0); unchanged(TACOpcode::URem,t,42,0);
        for (auto op : {TACOpcode::Shl,TACOpcode::LShr,TACOpcode::AShr}) {
            unchanged(op,t,1,-1); unchanged(op,t,1,t==i32?32:64);
        }
    }
    auto fp=make(TACOpcode::Add,Type::f64(),1,2); CHECK(!peephole(fp));
    auto mismatch=make(TACOpcode::Add,i32,1,2);
    mismatch.blocks[0].instructions[0].operands[1].type=i64; CHECK(!peephole(mismatch));
    std::puts("TAC arithmetic folding passed");
}
