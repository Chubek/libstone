module gateml.VisitorSpec;
import gateml.Module;
import gateml.AbsynSpec;

/// Visitor metadata is expressed with ordinary profile options, so bootstrap
/// compilers can preserve it without adding a new GateML section.
struct VisitorSpec {
    string order = "postorder";
    bool terminals = true;
    string[string] handlers;
    static VisitorSpec fromFrontend(const Frontend frontend) {
        import std.string : startsWith;
        VisitorSpec spec;
        foreach (key, value; frontend.options) {
            if (key == "visitor_order") spec.order = value;
            else if (key == "visitor_terminals") {
                if (value != "true" && value != "false") throw new GateError("invalid visitor_terminals");
                spec.terminals = value == "true";
            } else if (key.startsWith("visit_")) spec.handlers[key[6 .. $]] = value;
        }
        spec.validate(AbsynSpec.fromFrontend(frontend));
        return spec;
    }
    void validate(const AbsynSpec ast) const {
        if (order != "preorder" && order != "postorder") throw new GateError("invalid visitor order: " ~ order);
        foreach (kind, handler; handlers) {
            if (!ast.contains(kind) || !handler.length) throw new GateError("invalid visitor handler: " ~ kind);
        }
    }
}
