module gateml.Runtime;
import gateml.AST;
import gateml.Module;
import gateml.Sema;
import gateml.Bytecode;
import gateml.AbstractMachine;
import gateml.VM;
import std.conv : to, ConvException;

alias LoweringHook = void delegate(Tree node, ref Program program, uint arity);
alias LoweringHooks = LoweringHook[string];

Program lower(const Frontend frontend, Tree root, LoweringHooks hooks = null) {
    validate(frontend);
    if (root is null || root.terminal) throw new GateError("lowering requires a nonterminal tree");
    validateTree(root);
    Program program; program.name = frontend.name;
    void emit(Tree node) {
        auto binding = node.name in frontend.lowering;
        if (binding is null) throw new GateError("missing lowering hook: " ~ node.name, node.span);
        auto facility = *binding;
        if (facility == "integer" && !(facility in hooks)) {
            if (node.children.length != 1 || !node.children[0].terminal) throw new GateError("integer requires one terminal", node.span);
            auto text = node.children[0].text;
            if (!text.length) throw new GateError("empty integer", node.span);
            size_t start = text[0] == '-' || text[0] == '+' ? 1 : 0;
            if (start == text.length) throw new GateError("invalid integer", node.span);
            foreach (char c; text[start .. $]) if (c < '0' || c > '9') throw new GateError("invalid integer", node.span);
            try { program.emit(Opcode.constant, text.to!long, 0, null, node.span); }
            catch (ConvException error) { throw new GateError("invalid or overflowing i64 literal: " ~ text, node.span); }
            return;
        }
        uint arity;
        foreach (child; node.children) if (!child.terminal) { emit(child); ++arity; }
        if (auto hook = facility in hooks) {
            if (*hook is null) throw new GateError("null lowering hook: " ~ facility, node.span);
            (*hook)(node, program, arity); return;
        }
        int expected = facilityArity(facility);
        if (expected >= 0 && arity != expected) throw new GateError("lowering arity mismatch", node.span);
        Opcode opcode;
        switch (facility) {
            case "identity": return;
            case "neg": opcode = Opcode.neg; break;
            case "not": opcode = Opcode.bitNot; break;
            case "add": opcode = Opcode.add; break;
            case "sub": opcode = Opcode.sub; break;
            case "mul": opcode = Opcode.mul; break;
            case "sdiv": opcode = Opcode.sdiv; break;
            case "srem": opcode = Opcode.srem; break;
            case "band": opcode = Opcode.band; break;
            case "bor": opcode = Opcode.bor; break;
            case "bxor": opcode = Opcode.bxor; break;
            case "shl": opcode = Opcode.shl; break;
            case "lshr": opcode = Opcode.lshr; break;
            case "ashr": opcode = Opcode.ashr; break;
            default: program.emit(Opcode.call, 0, arity, facility, node.span); return;
        }
        program.emit(opcode, 0, 0, null, node.span);
    }
    emit(root); program.emit(Opcode.ret); verify(program); return program;
}

long evaluate(const Frontend frontend, Tree tree, Facility[string] facilities = null) {
    auto vm = new VM; vm.facilities = facilities;
    return vm.run(lower(frontend, tree));
}
