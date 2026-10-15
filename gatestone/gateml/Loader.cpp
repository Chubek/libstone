#include "Internal.hpp"

namespace gateml {
Program compile(std::string_view source, std::string file, Options options) {
    auto tree = parse(source, file, options);
    if (options.standard_library) {
        auto library = parse(standard_library(), "<stdlib>", options);
        // Splice the user program as the body of the library's final binding.
        // If that tail is ever missing, fail loudly rather than silently
        // discarding the user's program.
        auto tail = library;
        bool spliced = false;
        while (tail->kind == Expr::Let) {
            if (tail->children[1]->kind == Expr::Unit) { tail->children[1] = tree; spliced = true; break; }
            tail = tail->children[1];
        }
        if (!spliced) throw Error({"<stdlib>"}, "standard library has no unit tail to splice a program into");
        tree = library;
    }
    unsigned next = 1;
    auto inferred = infer(tree, builtin_types(next), next, options);
    if (options.optimize) optimize(tree);
    return {lower(tree, options), type_name(inferred)};
}
Execution evaluate(std::string_view source, Options options) {
    return run(compile(source, "<input>", options), options);
}
} // namespace gateml
