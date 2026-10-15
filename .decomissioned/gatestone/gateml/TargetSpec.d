module gateml.TargetSpec;
import gateml.AST;

/// Bytecode targets are portable; native code generation belongs to the C++
/// Jiterati integration. Explicit limits prevent unbounded parser/VM work.
struct TargetSpec {
    string name = "portable-i64";
    uint wordBits = 64;
    size_t maxTokens = 256;
    size_t maxSourceBytes = 1_048_576;
    size_t maxProductionSymbols = 256;
    size_t maxTreeNodes = 1_000_000;
    size_t maxTreeDepth = 1_024;
    size_t maxInstructions = 1_000_000;
    size_t maxStack = 65_536;
    void validate() const {
        if (wordBits != 64 || !name.length || !maxTokens || !maxSourceBytes || !maxProductionSymbols ||
            !maxTreeNodes || !maxTreeDepth || !maxInstructions || !maxStack)
            throw new GateError("invalid portable i64 target");
    }
}
