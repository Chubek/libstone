module gateml.Bytecode;
import gateml.AST;
import gateml.TargetSpec;
import gateml.serde.Binary;

enum Opcode : ubyte {
    constant, neg, bitNot, add, sub, mul, sdiv, srem, band, bor, bxor,
    shl, lshr, ashr, call, ret
}
struct Instruction {
    Opcode opcode;
    long immediate;
    uint arity;
    string facility;
    Span span;
}
struct Program {
    string name;
    Instruction[] instructions;
    void emit(Opcode opcode, long immediate = 0, uint arity = 0,
              string facility = null, Span span = Span.init) {
        instructions ~= Instruction(opcode, immediate, arity, facility, span);
    }
}

void verify(const Program program, TargetSpec target = TargetSpec.init) {
    target.validate();
    if (!program.instructions.length || program.instructions.length > target.maxInstructions)
        throw new GateError("invalid bytecode instruction count");
    size_t depth;
    foreach (i, instruction; program.instructions) {
        if (instruction.opcode > Opcode.max) throw new GateError("invalid opcode", instruction.span);
        size_t consumed;
        switch (instruction.opcode) {
            case Opcode.constant: break;
            case Opcode.neg, Opcode.bitNot: consumed = 1; break;
            case Opcode.call:
                if (!instruction.facility.length) throw new GateError("empty facility name", instruction.span);
                consumed = instruction.arity; break;
            case Opcode.ret:
                if (depth != 1 || i + 1 != program.instructions.length)
                    throw new GateError("return requires one value and must terminate bytecode", instruction.span);
                consumed = 1; break;
            default: consumed = 2;
        }
        if (depth < consumed) throw new GateError("bytecode stack underflow", instruction.span);
        depth -= consumed;
        if (instruction.opcode != Opcode.ret) ++depth;
        if (depth > target.maxStack) throw new GateError("bytecode stack limit exceeded", instruction.span);
    }
    if (program.instructions[$ - 1].opcode != Opcode.ret) throw new GateError("bytecode requires final return");
}

/// GMLB v1: little-endian lengths, i64 immediates; source spans are not persisted.
ubyte[] encode(const Program program) {
    verify(program);
    Writer writer;
    writer.raw(cast(const ubyte[]) "GMLB"); writer.octet(1);
    writer.string32(program.name); writer.le(program.instructions.length, 4);
    foreach (instruction; program.instructions) {
        writer.octet(cast(ubyte) instruction.opcode);
        writer.le(cast(ulong) instruction.immediate, 8);
        writer.le(instruction.arity, 4); writer.string32(instruction.facility);
    }
    return writer.data;
}

Program decode(const(ubyte)[] bytes, TargetSpec target = TargetSpec.init) {
    Reader reader = Reader(bytes);
    if (cast(string) reader.take(4) != "GMLB" || reader.octet() != 1) throw new GateError("unsupported bytecode format");
    Program result; result.name = reader.string32();
    auto count = reader.le(4);
    if (count > target.maxInstructions || count > reader.remaining / 17) throw new GateError("invalid bytecode length");
    foreach (unused; 0 .. count) {
        auto opcode = cast(Opcode) reader.octet(); auto immediate = cast(long) reader.le(8);
        auto arity = cast(uint) reader.le(4); auto facility = reader.string32();
        result.emit(opcode, immediate, arity, facility);
    }
    reader.finish(); verify(result, target); return result;
}
