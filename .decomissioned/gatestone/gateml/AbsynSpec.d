module gateml.AbsynSpec;
import gateml.Module;

struct AbsynSpec {
    string[] kinds;
    bool contains(string name) const {
        foreach (kind; kinds) if (kind == name) return true;
        return false;
    }
    void validate() const {
        bool[string] seen;
        foreach (kind; kinds) {
            if (!kind.length || kind in seen) throw new GateError("duplicate or empty AST kind: " ~ kind);
            seen[kind] = true;
        }
    }
    static AbsynSpec fromFrontend(const Frontend frontend) { return AbsynSpec(frontend.nodes.dup); }
}
