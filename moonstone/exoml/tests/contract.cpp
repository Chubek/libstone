#include <exoml/Contract.hpp>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace moonstone::exoml::contract;
using namespace moonstone::exoml::cdecl;

namespace {

TranslationUnit make_unit() {
    TranslationUnit unit;
    unit.version = schema_version;
    unit.header = "test.h";
    unit.complete = true;

    Declaration fn1;
    fn1.id = "decl_1";
    fn1.name = "test_create";
    fn1.kind = DeclKind::Function;
    fn1.parameters = {{"size", "size_t", {}, {}}, {"data", "const void *", {}, {}}};
    unit.declarations.push_back(fn1);

    Declaration fn2;
    fn2.id = "decl_2";
    fn2.name = "test_get";
    fn2.kind = DeclKind::Function;
    fn2.parameters = {{"owner", "test_t *", {}, {}}, {"index", "size_t", {}, {}}};
    unit.declarations.push_back(fn2);

    Declaration fn3;
    fn3.id = "decl_3";
    fn3.name = "test_destroy";
    fn3.kind = DeclKind::Function;
    fn3.parameters = {{"ptr", "test_t *", {}, {}}};
    unit.declarations.push_back(fn3);

    Declaration fn4;
    fn4.id = "decl_4";
    fn4.name = "test_process";
    fn4.kind = DeclKind::Function;
    fn4.parameters = {{"buf", "const char *", {}, {}},
                       {"len", "size_t", {}, {}},
                       {"cb", "void (*)(void *)", {}, {}},
                       {"ctx", "void *", {}, {}}};
    unit.declarations.push_back(fn4);

    return unit;
}

std::string minimal_sidecar() {
    return R"({
  "schema_version": 2,
  "library": "test",
  "headers": ["test.h"],
  "state": "validated",
  "functions": [
    {
      "c_name": "test_create",
      "return": { "kind": "owned", "destroy_with": "test_destroy",
                  "deallocator_family": "malloc" },
      "parameters": [
        { "index": 0, "name": "size", "ownership": "scalar" },
        { "index": 1, "name": "data", "ownership": "borrowed_immutable",
          "length_parameter": 0 }
      ],
      "errors": { "kind": "null_on_failure" },
      "effects": { "effects": ["allocate", "foreign_call"], "unknown": false },
      "destructors": [
        { "function_name": "test_destroy", "allocator_family": "malloc",
          "idempotent": false, "null_safe": true }
      ],
      "provenance": {
        "header": "test.h", "revision": "r1",
        "review_status": "reviewed", "author": "tester"
      }
    },
    {
      "c_name": "test_get",
      "return": { "kind": "borrow",
                  "lifetime": { "origin": "parameter", "parameter_index": 0 } },
      "parameters": [
        { "index": 0, "name": "owner", "ownership": "borrowed_mutable" },
        { "index": 1, "name": "index", "ownership": "scalar" }
      ],
      "errors": { "kind": "none" },
      "effects": { "effects": ["read"], "unknown": false },
      "provenance": {
        "header": "test.h", "revision": "r1",
        "review_status": "reviewed", "author": "tester"
      }
    }
  ],
  "bundle_provenance": {
    "header": "test.h", "revision": "r1",
    "review_status": "reviewed", "author": "tester"
  }
})";
}

int test_count = 0;
int pass_count = 0;

void check(bool condition, const char *name) {
    ++test_count;
    if (condition) {
        ++pass_count;
    } else {
        std::cerr << "FAIL: " << name << '\n';
    }
}

void test_parse_minimal() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    check(bundle.schema_version == 2, "parse: schema version");
    check(bundle.library == "test", "parse: library");
    check(bundle.headers.size() == 1 && bundle.headers[0] == "test.h", "parse: headers");
    check(bundle.state == ContractState::Validated, "parse: state");
    check(bundle.functions.size() == 2, "parse: function count");
    check(bundle.functions[0].c_name == "test_create", "parse: function name");
    check(bundle.functions[0].return_contract.kind == ReturnKind::Owned, "parse: return kind");
    check(bundle.functions[0].return_contract.destroy_with == "test_destroy",
          "parse: destroy_with");
    check(bundle.functions[0].parameters.size() == 2, "parse: parameter count");
    check(bundle.functions[0].parameters[1].ownership == OwnershipKind::BorrowedImmutable,
          "parse: parameter ownership");
    check(bundle.functions[0].parameters[1].length_parameter.has_value() &&
              *bundle.functions[0].parameters[1].length_parameter == 0,
          "parse: length_parameter");
    check(bundle.functions[0].destructors.size() == 1, "parse: destructor count");
    check(bundle.functions[0].destructors[0].null_safe == true, "parse: destructor null_safe");
    check(bundle.functions[1].return_contract.kind == ReturnKind::Borrow, "parse: borrow return");
    check(bundle.functions[1].return_contract.lifetime.has_value(), "parse: lifetime present");
    check(bundle.functions[1].return_contract.lifetime->origin == LifetimeOrigin::Parameter,
          "parse: lifetime origin");
}

void test_round_trip() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    auto serialized = serialize_contract(bundle);
    auto reparsed = parse_contract_sidecar(serialized);
    auto reserialized = serialize_contract(reparsed);
    check(serialized == reserialized, "round-trip: deterministic serialization");
    check(reparsed.schema_version == bundle.schema_version, "round-trip: version");
    check(reparsed.library == bundle.library, "round-trip: library");
    check(reparsed.functions.size() == bundle.functions.size(), "round-trip: function count");
    check(reparsed.functions[0].c_name == bundle.functions[0].c_name, "round-trip: function name");
    check(reparsed.functions[0].return_contract.kind == bundle.functions[0].return_contract.kind,
          "round-trip: return kind");
    check(reparsed.functions[0].parameters.size() == bundle.functions[0].parameters.size(),
          "round-trip: parameter count");
    check(reparsed.functions[0].destructors.size() == bundle.functions[0].destructors.size(),
          "round-trip: destructor count");
}

void test_reject_bad_version() {
    std::string bad = minimal_sidecar();
    auto pos = bad.find("\"schema_version\": 2");
    bad.replace(pos, 19, "\"schema_version\": 99");
    auto bundle = parse_contract_sidecar(bad);
    auto unit = make_unit();
    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-VERSION")
            found = true;
    check(found, "reject: bad version");
}

void test_reject_unknown_function() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    bundle.functions[0].c_name = "nonexistent_function";
    auto unit = make_unit();
    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-DRIFT")
            found = true;
    check(found, "reject: unknown function (drift)");
}

void test_reject_param_count_mismatch() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    bundle.functions[0].parameters.pop_back();
    auto unit = make_unit();
    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-DRIFT" &&
            d.message.find("parameter count mismatch") != std::string::npos)
            found = true;
    check(found, "reject: parameter count mismatch");
}

void test_reject_contradictory_ownership() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_create";
    ParameterContract p;
    p.index = 0;
    p.ownership = OwnershipKind::Consumed;
    fn.parameters.push_back(p);
    ParameterContract p2;
    p2.index = 0;
    p2.ownership = OwnershipKind::BorrowedImmutable;
    fn.parameters.push_back(p2);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_create";
    decl.kind = DeclKind::Function;
    decl.parameters = {{"a", "int", {}, {}}, {"b", "int", {}, {}}};
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-CONTRADICT")
            found = true;
    check(found, "reject: contradictory ownership");
}

void test_reject_borrow_without_lifetime() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_get";
    fn.return_contract.kind = ReturnKind::Borrow;
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_get";
    decl.kind = DeclKind::Function;
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-LIFETIME")
            found = true;
    check(found, "reject: borrow without lifetime");
}

void test_reject_borrow_bad_parameter() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_get";
    fn.return_contract.kind = ReturnKind::Borrow;
    fn.return_contract.lifetime = LifetimeSpec{LifetimeOrigin::Parameter, 99, {}};
    ParameterContract p;
    p.index = 0;
    fn.parameters.push_back(p);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_get";
    decl.kind = DeclKind::Function;
    decl.parameters = {{"owner", "void *", {}, {}}};
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-LIFETIME" &&
            d.message.find("nonexistent parameter") != std::string::npos)
            found = true;
    check(found, "reject: borrow references nonexistent parameter");
}

void test_reject_owned_no_destructor() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_create";
    fn.return_contract.kind = ReturnKind::Owned;
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_create";
    decl.kind = DeclKind::Function;
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-ALLOCATOR")
            found = true;
    check(found, "reject: owned return without destructor");
}

void test_reject_length_param_missing() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_create";
    ParameterContract p;
    p.index = 0;
    p.length_parameter = 5;
    fn.parameters.push_back(p);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_create";
    decl.kind = DeclKind::Function;
    decl.parameters = {{"buf", "const char *", {}, {}}};
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-LENGTH")
            found = true;
    check(found, "reject: nonexistent length parameter");
}

void test_reject_retained_callback_no_unregister() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_process";
    CallbackContract cb;
    cb.mode = CallbackMode::Retained;
    fn.callbacks.push_back(cb);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_process";
    decl.kind = DeclKind::Function;
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-CALLBACK")
            found = true;
    check(found, "reject: retained callback without unregister");
}

void test_reject_malformed_json() {
    bool threw = false;
    try {
        parse_contract_sidecar("{invalid json}");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "reject: malformed JSON");
}

void test_reject_unknown_enum() {
    bool threw = false;
    try {
        parse_contract_sidecar(R"({"schema_version":2,"functions":[{"c_name":"x","return":{"kind":"bogus_value"}}]})");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "reject: unknown enum value");
}

void test_reject_unknown_effect() {
    bool threw = false;
    try {
        parse_contract_sidecar(
            R"({"schema_version":2,"functions":[{"c_name":"x","effects":{"effects":["teleport"]}}]})");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "reject: unknown effect");
}

void test_safe_export() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    bundle.state = ContractState::Reviewed;
    for (auto &fn : bundle.functions)
        fn.provenance.review_status = ReviewStatus::Reviewed;
    check(is_safe_for_export(bundle), "safe export: reviewed bundle");

    bundle.state = ContractState::Discovered;
    check(!is_safe_for_export(bundle), "safe export: discovered is not safe");

    bundle.state = ContractState::Reviewed;
    bundle.functions[0].provenance.review_status = ReviewStatus::Unreviewed;
    check(!is_safe_for_export(bundle), "safe export: mixed review is not safe");
}

void test_projection() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    auto proj = project_to_ownership(bundle.functions[0], 2);
    check(proj.ownership.consumed_arguments.empty(), "projection: no consumed");
    check(proj.ownership.borrowed_arguments.count(1), "projection: borrowed index 1");
    check(!proj.ownership.returned_borrow, "projection: owned return is not borrow");
    check(proj.ownership.effects.effects.count(moonstone::Effect::Allocate),
          "projection: allocate effect");
    check(proj.ownership.effects.effects.count(moonstone::Effect::ForeignCall),
          "projection: foreign_call effect");

    auto proj2 = project_to_ownership(bundle.functions[1], 2);
    check(proj2.ownership.returned_borrow, "projection: borrow return");
    check(proj2.ownership.borrowed_arguments.count(0), "projection: borrowed owner");
}

void test_duplicate_function() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_create";
    bundle.functions.push_back(fn);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_create";
    decl.kind = DeclKind::Function;
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-DUPLICATE")
            found = true;
    check(found, "reject: duplicate function contract");
}

void test_duplicate_destructor() {
    ContractBundle bundle;
    bundle.schema_version = contract_schema_version;
    FunctionContract fn;
    fn.c_name = "test_create";
    DestructorContract d1;
    d1.function_name = "test_destroy";
    d1.allocator_family = "malloc";
    fn.destructors.push_back(d1);
    DestructorContract d2;
    d2.function_name = "test_destroy";
    d2.allocator_family = "malloc";
    fn.destructors.push_back(d2);
    bundle.functions.push_back(fn);

    TranslationUnit unit;
    Declaration decl;
    decl.name = "test_create";
    decl.kind = DeclKind::Function;
    unit.declarations.push_back(decl);

    auto diags = validate_contract(bundle, unit);
    bool found = false;
    for (const auto &d : diags)
        if (d.id == "EXOML-CONTRACT-DUPLICATE" &&
            d.message.find("destructor") != std::string::npos)
            found = true;
    check(found, "reject: duplicate destructor");
}

void test_valid_contract_no_diagnostics() {
    auto bundle = parse_contract_sidecar(minimal_sidecar());
    auto unit = make_unit();
    auto diags = validate_contract(bundle, unit);
    check(diags.empty(), "valid contract: no diagnostics");
}

} // namespace

int main() {
    try {
        test_parse_minimal();
        test_round_trip();
        test_reject_bad_version();
        test_reject_unknown_function();
        test_reject_param_count_mismatch();
        test_reject_contradictory_ownership();
        test_reject_borrow_without_lifetime();
        test_reject_borrow_bad_parameter();
        test_reject_owned_no_destructor();
        test_reject_length_param_missing();
        test_reject_retained_callback_no_unregister();
        test_reject_malformed_json();
        test_reject_unknown_enum();
        test_reject_unknown_effect();
        test_safe_export();
        test_projection();
        test_duplicate_function();
        test_duplicate_destructor();
        test_valid_contract_no_diagnostics();
    } catch (const std::exception &e) {
        std::cerr << "EXCEPTION: " << e.what() << '\n';
        return 1;
    }

    std::cout << pass_count << '/' << test_count << " contract tests passed\n";
    return pass_count == test_count ? 0 : 1;
}
