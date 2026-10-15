module gateml.VM;
import gateml.AST;
import gateml.Bytecode;
import gateml.AbstractMachine;
import gateml.TargetSpec;

/// Signed i64 stack machine. Arithmetic wraps modulo 2^64; invalid division
/// and shifts trap rather than inheriting host-language undefined behavior.
class VM : AbstractMachine {
    Facility[string] facilities;
    TargetSpec target;
    private long[] stack;
    private bool running;
    this(TargetSpec target = TargetSpec.init) { target.validate(); this.target = target; }
    void reset() {
        if (running) throw new GateError("cannot reset a running VM");
        stack.length = 0;
    }
    long run(const Program program) {
        if (running) throw new GateError("VM execution is not reentrant");
        target.validate();
        if (!program.instructions.length || program.instructions.length > target.maxInstructions)
            throw new GateError("invalid bytecode instruction count");
        // A hook may hold another mutable alias to the caller's program.
        Program execution;
        execution.name = program.name;
        execution.instructions = program.instructions.dup;
        verify(execution, target);
        stack.length = 0; running = true;
        scope(exit) { stack.length = 0; running = false; }
        foreach (instruction; execution.instructions) {
            auto op = instruction.opcode;
            if (op == Opcode.ret) return stack[0];
            if (op == Opcode.constant) { stack ~= instruction.immediate; continue; }
            if (op == Opcode.call) {
                auto hook = instruction.facility in facilities;
                if (hook is null || *hook is null) throw new GateError("unresolved facility: " ~ instruction.facility, instruction.span);
                auto start = stack.length - instruction.arity;
                auto value = (*hook)(stack[start .. $]);
                stack.length = start; stack ~= value; continue;
            }
            if (op == Opcode.neg) { stack[$ - 1] = cast(long)(0UL - cast(ulong) stack[$ - 1]); continue; }
            if (op == Opcode.bitNot) { stack[$ - 1] = ~stack[$ - 1]; continue; }
            long b = stack[$ - 1], a = stack[$ - 2], value;
            stack.length -= 2;
            switch (op) {
                case Opcode.add: value = cast(long)(cast(ulong) a + cast(ulong) b); break;
                case Opcode.sub: value = cast(long)(cast(ulong) a - cast(ulong) b); break;
                case Opcode.mul: value = cast(long)(cast(ulong) a * cast(ulong) b); break;
                case Opcode.sdiv, Opcode.srem:
                    if (!b || a == long.min && b == -1) throw new GateError("invalid signed division", instruction.span);
                    value = op == Opcode.sdiv ? a / b : a % b; break;
                case Opcode.band: value = a & b; break;
                case Opcode.bor: value = a | b; break;
                case Opcode.bxor: value = a ^ b; break;
                case Opcode.shl, Opcode.lshr, Opcode.ashr:
                    if (b < 0 || b >= 64) throw new GateError("shift count outside [0, 63]", instruction.span);
                    if (op == Opcode.shl) value = cast(long)(cast(ulong) a << b);
                    else if (op == Opcode.lshr) value = cast(long)(cast(ulong) a >> b);
                    else value = a >> b;
                    break;
                default: throw new GateError("invalid arithmetic opcode", instruction.span);
            }
            stack ~= value;
        }
        throw new GateError("missing return");
    }
}
