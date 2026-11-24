#pragma once

#include <cephyr/lower.hpp>

#include <functional>

namespace cephyr::detail {

// Source-ordered declaration semantics shared by code generation and interface
// projection. The consumers differ in how they process bodies and initializers.
struct DeclarationConsumer {
    bool projection = false;
    std::function<void(Node *, std::string, CType)> type;
    std::function<void(Node *, const std::string &, Symbol &, unsigned)> function;
    std::function<bool(Node *, const std::string &, Symbol &, unsigned)> object;
};

void resolve_file_declarations(Unit &, Node *root, Node *&at, const DeclarationConsumer &);
bool check_linkage_alignment(Unit &, const Symbol &);
bool finish_tentative_object(Unit &, Symbol &);
bool validate_interface_initializer(Unit &, Symbol &);

// CType records carry a semantic layout pointer, enums a parser identity. A
// projection can observe the names/bodies that introduce those same identities
// without walking the entire syntax tree or re-resolving types out of scope.
class TagObserver {
public:
    using Observe = std::function<void(Node *, const CType &)>;
    TagObserver(const Unit &, Observe);
    ~TagObserver();
    TagObserver(const TagObserver &) = delete;
    TagObserver &operator=(const TagObserver &) = delete;

    static void notify(const Unit &, Node *, const CType &);

private:
    static thread_local const TagObserver *active_;
    const Unit &unit_;
    Observe observe_;
    const TagObserver *previous_;
};

} // namespace cephyr::detail
