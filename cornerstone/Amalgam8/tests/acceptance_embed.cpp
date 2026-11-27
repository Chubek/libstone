#include "Amalgam8Base.hpp"

#include <iostream>

int main() {
    auto r = am8::compile_source(R"(
        (library Legacy
          (include stdint.h)
          (syntax (constant $name $value) (def $name $value))
          (stage (list (quote constant) (quote ANSWER) (quote 42)))
          (fn int answer () "return ANSWER;")
          (cli "return answer() == 42 ? 0 : 1;"))
    )", "embedding.am8");
    if (r.source.find("#define ANSWER (42)") == r.source.npos ||
        r.source.find("int main(") != r.source.npos ||
        r.dependencies.size() != 1 || r.dependencies[0].header != "stdint.h" ||
        r.opaque_fragments.size() != 2 || r.opaque_fragments[0].role != "statement" ||
        r.opaque_fragments[1].role != "entry") {
        std::cerr << r.source;
        return 1;
    }
    try {
        (void)am8::compile_source("(library Embedding) trailing", "embedding.am8");
    } catch (const am8::Error& e) {
        return std::string_view(e.what()).starts_with("embedding.am8:1:") ? 0 : 2;
    }
    return 3;
}
