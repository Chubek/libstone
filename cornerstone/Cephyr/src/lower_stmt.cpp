#include <cephyr/lower.hpp>

#include <AbstractSyntaxTree.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cephyr::detail {
namespace {

using cparser::ASTNodeKind;

struct SwitchCases {
    CType type; // Promoted type of the controlling expression.
    std::map<std::uint64_t, Id> values;
    Id otherwise = no_value;
};

void lower_statement(Function &, Node *, SwitchCases *);

CType function_result(const Function &function) {
    const CType *type = &function.symbol.type;
    if (type->kind == CType::Kind::pointer && type->element &&
        type->element->kind == CType::Kind::function)
        type = type->element.get();
    if (type->kind != CType::Kind::function || !type->element)
        return {};
    return *type->element;
}

bool is_main(const Function &function) {
    return function.unit.hosted && function.symbol.name == "main";
}

void branch_on(Function &function, Node *expression, Id yes, Id no) {
    // Still lower the expression for diagnostics and effects. An integer
    // constant condition contributes only its feasible edge to reachability.
    const Expr value = lower_expr(function, expression);
    if (value.type.integer() && value.id != no_value && value.constant) {
        function.jump(*value.constant ? yes : no);
        return;
    }
    const Id condition = truth(function, value, expression);
    if (condition == no_value) {
        function.trap();
        return;
    }
    function.branch(condition, yes, no);
}

bool expression_kind(ASTNodeKind kind) {
    return kind == cparser::NK_IDENT_NODE || kind == cparser::NK_SIZEOF_EXPR ||
           kind == cparser::NK_ALIGNOF_EXPR || kind == cparser::NK_OFFSETOF_EXPR ||
           kind == cparser::NK_UNARY_PLUS_EXPR ||
           kind == cparser::NK_GENERIC_EXPR ||
           kind == cparser::NK_COMPOUND_LITERAL ||
           (kind >= cparser::NK_INTEGER_CONST && kind <= cparser::NK_PUTI_EXPR);
}

std::optional<std::uint64_t> object_size(Function &function, const CType &type,
                                         Node *at) {
    auto size = size_of(type);
    if (!size || !*size) {
        function.error(at, "local object has incomplete or unsupported type",
                       "cephyr.local.type");
        return std::nullopt;
    }
    if (*size > std::numeric_limits<unsigned>::max()) {
        function.error(at, "local object is too large", "cephyr.local.size");
        return std::nullopt;
    }
    return size;
}

Node *scalar_initializer_expression(Function &function, Node *initializer) {
    while (initializer->getKind() == cparser::NK_LIST) {
        const auto values = elements(initializer);
        if (values.size() != 1) {
            function.error(initializer,
                           "scalar initializer must contain exactly one expression",
                           "cephyr.local.initializer");
            return nullptr;
        }
        initializer = values.front();
    }
    return initializer;
}

void initialize_scalar(Function &function, LValue object, Node *initializer) {
    initializer = scalar_initializer_expression(function, initializer);
    if (!initializer)
        return;
    if (!object.type.scalar()) {
        function.error(initializer, "local aggregate initialization is not supported",
                       "cephyr.local.initializer");
        return;
    }
    object.type.is_const = false;
    store(function, object, lower_expr(function, initializer), initializer);
}

void initialize_object(Function &function, LValue object, const std::vector<ObjectInitEntry> &plan,
                        std::uint64_t size, Node *at, bool defer_stores = false) {
    // On reaching a compound literal again in the same block activation,
    // an initializer can read that literal's previous value through an alias.
    // Evaluate all explicit values before touching its storage (6.7.9p23).
    std::vector<Expr> values;
    if (defer_stores) {
        values.resize(plan.size());
        for (std::size_t i = 0; i < plan.size(); ++i) {
            const auto &entry = plan[i];
            if (entry.clear || entry.bytes)
                continue;
            auto *source = entry.type.scalar()
                ? scalar_initializer_expression(function, entry.source) : entry.source;
            if (source)
                values[i] = lower_expr(function, source);
        }
    }
    const auto direct = object_has_qualifier(object.type, &CType::is_volatile);
    // A whole-record expression initializes the object directly. Otherwise
    // clear ordinary storage first. Volatile storage must not receive that
    // blanket write followed by a second write to the explicit subobjects.
    if (!direct && (object.type.kind != CType::Kind::record || at->getKind() == cparser::NK_LIST))
        zero_object(function, object.address, size, align_of(object.type));
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const auto &entry = plan[i];
        if (entry.clear) {
            const auto address = offset_address(function, object.address, entry.offset, entry.type);
            if (direct)
                zero_initialized_subobject(function, address, entry.type);
            else
                zero_object(function, address, *size_of(entry.type), align_of(entry.type));
        } else if (entry.bytes) {
            auto offset = entry.offset;
            auto element = *entry.type.element;
            element.is_const = false; // This store initializes the element.
            element.is_volatile |= entry.type.is_volatile;
            const auto width = element.bits / 8;
            const auto count = entry.bytes->size() / width;
            for (std::size_t i = 0; i < count; ++i) {
                std::uint64_t value = 0;
                for (unsigned byte = 0; byte < width; ++byte)
                    value |= std::uint64_t(static_cast<unsigned char>((*entry.bytes)[i * width + byte]))
                             << (byte * 8);
                if (direct || value)
                    store(function, {element, offset_address(function, object.address, offset, element)},
                          {element, function.constant(value, element), value}, entry.source);
                offset += width;
            }
            if (direct && count < entry.type.count) {
                auto tail = entry.type;
                tail.count -= count;
                zero_initialized_subobject(function,
                    offset_address(function, object.address, offset, element), tail);
            }
        } else if (entry.type.kind == CType::Kind::record) {
            const auto address = offset_address(function, object.address, entry.offset, entry.type);
            auto value = defer_stores ? values[i] : lower_expr(function, entry.source);
            (void)copy_record(function, {entry.type, address}, value,
                              entry.source, true);
        } else {
            const auto address = offset_address(function, object.address, entry.offset, entry.type);
            LValue member{entry.type, address, {}, false, nullptr, entry.bit_field};
            if (defer_stores) {
                member.type.is_const = false;
                store(function, member, values[i], entry.source);
            } else {
                initialize_scalar(function, member, entry.source);
            }
        }
    }
}

void lower_local_declaration(Function &function, Node *node) {
    if (node->getKind() == cparser::NK_TYPE_DECL) {
        auto *declaration = static_cast<cparser::TypeDeclASTNode *>(node);
        (void)read_type(declaration->getBody(), TypeContext(function.unit, &function), true);
        return;
    }
    const auto context = TypeContext(function.unit, &function);
    if (node->getKind() == cparser::NK_FUNCTION_DECL) {
        auto &decl = *static_cast<cparser::FunctionDeclASTNode *>(node);
        auto type = read_type(decl.getType(), context, true);
        if (type.valid())
            declare_block_linkage(function, node, decl.getName(), std::move(type), decl.getFlags());
        return;
    }
    if (node->getKind() != cparser::NK_VAR_DECL) {
        function.error(node, "unsupported local declaration",
                       "cephyr.local.unsupported");
        return;
    }

    auto *declaration = static_cast<cparser::VarDeclASTNode *>(node);
    Node *name_node = declaration->getName();
    Node *at = present(name_node) ? name_node : node;
    Node *type_node = declaration->getType();
    const unsigned flags =
        node->getFlags() | (present(type_node) ? type_node->getFlags() : 0);
    const std::string name = identifier(name_node);
    CType type = read_type(type_node, context, true);
    if (!type.valid()) {
        if (!name.empty())
            function.declare_local(name, {}, at);
        return;
    }
    if (type.kind == CType::Kind::void_ || type.kind == CType::Kind::function) {
        function.error(at, "local object must have a complete object type",
                       "cephyr.local.type");
        return;
    }

    if (name.empty()) {
        function.error(at, "local declaration has no identifier",
                       "cephyr.local.name");
        return;
    }

    const auto alignment = alignment_specifiers(declaration->getAlignments(), context);
    if (!alignment || !check_object_alignment(*alignment, align_of(type), at, context))
        return;

    Node *initializer = declaration->getInit();
    if (flags & SCS_EXTERN) {
        if (present(initializer))
            function.error(at, "block-scope declaration with linkage cannot have an initializer");
        else
            declare_block_linkage(function, node, name_node, std::move(type), flags,
                present(declaration->getAlignments()) ? alignment : std::nullopt);
        return;
    }
    if (flags & SCS_STATIC) {
        if (function.symbol.inline_only) {
            const CType *element = &type;
            while (!element->is_const && element->kind == CType::Kind::array && element->element)
                element = element->element.get();
            if (!element->is_const) {
                function.error(at, "external inline definition defines a modifiable static object");
                return;
            }
        }
        // The name has block scope and no C linkage, even though the object
        // is emitted with a private assembly symbol and static duration.
        const auto native_name = "__cephyr_tu_block_" + std::to_string(function.unit.block_statics++);
        if (!function.declare_local(name, {type, no_value, native_name}, at))
            return;
        Symbol storage{std::move(type), native_name, node, node, true, false,
                       present(initializer)};
        if (present(declaration->getAlignments()))
            storage.definition_alignment = alignment;
        if (define_global(function.unit, storage, &function))
            function.scopes.back().at(name).type = std::move(storage.type);
        return;
    }

    const CType declared_type = type;
    const bool register_object = (flags & SCS_REGISTER) != 0;
    if (!function.declare_local(name, {declared_type, no_value, {}, register_object}, at))
        return;
    std::optional<std::vector<ObjectInitEntry>> aggregate_init;
    if (type.aggregate() && present(initializer)) {
        aggregate_init = plan_object_initializer(
            TypeContext(function.unit, &function), type, initializer,
            object_has_qualifier(type, &CType::is_volatile));
        if (!aggregate_init)
            return;
    }
    if (type.kind == CType::Kind::array && !type.count) {
        function.error(at, "incomplete local array has no inferable bound",
                       "cephyr.local.type");
        return;
    }
    const auto size = object_size(function, type, at);
    if (!size)
        return;

    LValue slot = function.local(type, at, *alignment);
    if (slot.address == no_value)
        return;
    // The slot has its final size, but an inferred bound is not visible
    // until after the end of the initializer list.
    function.scopes.back().at(name).address = slot.address;
    if (aggregate_init)
        initialize_object(function, slot, *aggregate_init, *size, initializer);
    else if (present(initializer))
        initialize_scalar(function, slot, initializer);
    if (type.kind == CType::Kind::array && !declared_type.count)
        function.scopes.back().at(name).type = std::move(type);
}

void lower_sequence(Function &function, Node *sequence, SwitchCases *cases) {
    for (Node *item : elements(sequence)) {
        // Continue semantic checking after a terminator. The resulting block is
        // deliberately unreachable and is removed by the unit finalizer.
        if (!function.open())
            function.select(function.block());
        lower_statement(function, item, cases);
    }
}

void lower_compound(Function &function, cparser::CompoundStmtASTNode *compound,
                    bool nested_scope, SwitchCases *cases) {
    if (nested_scope)
        function.scopes.emplace_back();

    lower_sequence(function, compound->getItems(), cases);

    if (nested_scope)
        function.scopes.pop_back();
}

void lower_if(Function &function, cparser::IfStmtASTNode *statement,
              SwitchCases *cases) {
    const Id then_block = function.block();
    const bool has_else = present(statement->getElseClause());
    const Id else_block = has_else ? function.block() : no_value;
    const Id merge_block = function.block();
    branch_on(function, statement->getCondition(), then_block,
              has_else ? else_block : merge_block);

    function.select(then_block);
    lower_statement(function, statement->getThenClause(), cases);
    function.jump(merge_block);

    if (has_else) {
        function.select(else_block);
        lower_statement(function, statement->getElseClause(), cases);
        function.jump(merge_block);
    }

    function.select(merge_block);
}

void lower_while(Function &function, cparser::WhileStmtASTNode *statement,
                 SwitchCases *cases) {
    const Id condition_block = function.block();
    const Id body_block = function.block();
    const Id exit_block = function.block();
    function.jump(condition_block);

    function.select(condition_block);
    branch_on(function, statement->getCondition(), body_block, exit_block);

    function.loops.emplace_back(exit_block, condition_block);
    function.select(body_block);
    lower_statement(function, statement->getBody(), cases);
    function.jump(condition_block);
    function.loops.pop_back();

    function.select(exit_block);
}

void lower_do(Function &function, cparser::DoStmtASTNode *statement,
              SwitchCases *cases) {
    const Id body_block = function.block();
    const Id condition_block = function.block();
    const Id exit_block = function.block();
    function.jump(body_block);

    function.loops.emplace_back(exit_block, condition_block);
    function.select(body_block);
    lower_statement(function, statement->getBody(), cases);
    function.jump(condition_block);
    function.loops.pop_back();

    function.select(condition_block);
    branch_on(function, statement->getCondition(), body_block, exit_block);
    function.select(exit_block);
}

void lower_for_init(Function &function, Node *node) {
    if (!present(node))
        return;
    if (node->getKind() == cparser::NK_LIST) {
        for (Node *item : elements(node))
            lower_for_init(function, item);
        return;
    }
    if (node->getKind() == cparser::NK_VAR_DECL) {
        Node *type = static_cast<cparser::VarDeclASTNode *>(node)->getType();
        const auto flags = node->getFlags() | (present(type) ? type->getFlags() : 0);
        if (flags & (SCS_STATIC | SCS_EXTERN)) {
            function.error(node, "for declaration must declare automatic or register objects",
                           "cephyr.statement.for-declaration");
            return;
        }
        lower_local_declaration(function, node);
        return;
    }
    if (node->isType() || node->getKind() == cparser::NK_TYPE_DECL) {
        function.error(node, "for declaration must declare automatic or register objects",
                       "cephyr.statement.for-declaration");
        return;
    }
    (void)lower_expr(function, node);
}

void lower_for(Function &function, cparser::ForStmtASTNode *statement,
               SwitchCases *cases) {
    function.scopes.emplace_back();
    lower_for_init(function, statement->getInit());

    const Id condition_block = function.block();
    const Id body_block = function.block();
    const Id step_block = function.block();
    const Id exit_block = function.block();
    function.jump(condition_block);

    function.select(condition_block);
    if (present(statement->getCondition()))
        branch_on(function, statement->getCondition(), body_block, exit_block);
    else
        function.jump(body_block);

    // Analyze the third clause before the body, where enumerators from its
    // type names are already in scope. The CFG still runs the body first.
    function.select(step_block);
    if (present(statement->getStep()))
        (void)lower_expr(function, statement->getStep());
    function.jump(condition_block);

    function.loops.emplace_back(exit_block, step_block);
    function.select(body_block);
    lower_statement(function, statement->getBody(), cases);
    function.jump(step_block);
    function.loops.pop_back();

    function.select(exit_block);
    function.scopes.pop_back();
}

void lower_case(Function &function, cparser::CaseLabelASTNode *label,
                SwitchCases *cases) {
    if (!cases) {
        function.error(label, "case or default label is not within a switch",
                       "cephyr.statement.case");
        lower_statement(function, label->getStmt(), nullptr);
        return;
    }

    const Id target = function.block();
    if (Node *expression = label->getExpr(); present(expression)) {
        const auto errors = function.unit.diagnostics.error_count();
        const auto constant = integer_constant(expression, TypeContext(function.unit, &function));
        if (!constant || !constant->type.integer()) {
            if (function.unit.diagnostics.error_count() == errors)
                function.error(expression, "case label requires an integer constant expression",
                               "cephyr.statement.case");
        } else {
            const auto source = normalize(constant->value, constant->type);
            const auto key = normalize(source, cases->type);
            if (!cases->values.emplace(key, target).second)
                function.error(expression, "duplicate case value after conversion to switch type",
                               "cephyr.statement.case");
        }
    } else if (cases->otherwise != no_value) {
        function.error(label, "duplicate default label in switch",
                       "cephyr.statement.case");
    } else {
        cases->otherwise = target;
    }

    // Arrival here by ordinary execution is fallthrough; a switch edge may
    // enter the same block directly, bypassing any preceding initialization.
    function.jump(target);
    function.select(target);
    lower_statement(function, label->getStmt(), cases);
}

void lower_switch(Function &function, cparser::SwitchStmtASTNode *statement) {
    Node *expression = statement->getExpr();
    Expr controlling = lower_expr(function, expression);
    if (controlling.type.valid() && !controlling.type.integer())
        function.error(expression, "switch condition requires an integer expression",
                       "cephyr.statement.switch");
    SwitchCases cases;
    cases.type = controlling.type.integer() ? promote(controlling.type) : integer_type();
    controlling = controlling.type.integer()
        ? convert(function, std::move(controlling), cases.type, expression)
        : Expr{};

    const Id dispatch = function.current;
    const Id exit = function.block();
    function.loops.emplace_back(exit, no_value);
    // A switch starts execution at a matching label, not at the start of its
    // body. The initial disconnected block accumulates code before any label.
    function.select(function.block());
    lower_statement(function, statement->getStmt(), &cases);
    function.jump(exit);
    function.loops.pop_back();

    function.select(dispatch);
    const Id fallback = cases.otherwise == no_value ? exit : cases.otherwise;
    if (controlling.id == no_value) {
        function.trap();
    } else if (controlling.constant) {
        const auto found = cases.values.find(normalize(*controlling.constant, cases.type));
        function.jump(found == cases.values.end() ? fallback : found->second);
    } else {
        for (auto iter = cases.values.begin(); iter != cases.values.end(); ++iter) {
            const Id constant = function.constant(iter->first, cases.type);
            const Id matches = function.emit(integer_type(), "eq",
                                              std::to_string(controlling.id) + " " +
                                              std::to_string(constant));
            const bool last = std::next(iter) == cases.values.end();
            const Id next = last ? fallback : function.block();
            function.branch(matches, iter->second, next);
            if (!last)
                function.select(next);
        }
        if (cases.values.empty())
            function.jump(fallback);
    }
    function.select(exit);
}

void lower_return(Function &function, cparser::ReturnStmtASTNode *statement) {
    const CType result = function_result(function);
    Node *expression = statement->getExpr();
    if (!result.valid()) {
        function.error(statement, "cannot determine function return type",
                       "cephyr.return.type");
        function.trap();
        return;
    }
    if (result.kind == CType::Kind::void_) {
        if (present(expression)) {
            (void)lower_expr(function, expression);
            function.error(statement, "void function must not return a value",
                           "cephyr.return.value");
        }
        function.ret();
        return;
    }
    if (!present(expression)) {
        function.error(statement, "non-void function must return a value",
                       "cephyr.return.value");
        function.trap();
        return;
    }
    Expr value = convert(function, lower_expr(function, expression), result, expression);
    if (value.id == no_value) {
        function.trap();
        return;
    }
    switch (record_abi(result)) {
    case RecordABI::scalar:
        function.ret(abi_value(function, value));
        break;
    case RecordABI::word:
        function.ret(pack_record_abi(function, value));
        break;
    case RecordABI::pair: {
        const auto word_type = integer_type(64, false);
        for (unsigned index = 0; index < 2; ++index) {
            const auto address = offset_address(function, function.record_result_address,
                                                8 * index, word_type);
            store(function, {word_type, address},
                  {word_type, pack_record_abi(function, value, index), std::nullopt}, expression);
        }
        function.ret();
        break;
    }
    case RecordABI::indirect:
        if (copy_record(function, {result, function.record_result_address}, value, expression, true))
            function.ret();
        else
            function.trap();
        break;
    }
}

void lower_goto(Function &function, cparser::GotoStmtASTNode *statement) {
    Node *name = statement->getLabel();
    auto &label = function.labels[identifier(name)];
    if (label.target == no_value)
        label.target = function.block();
    if (!label.first_reference)
        label.first_reference = name;
    function.jump(label.target);
}

void lower_label(Function &function, cparser::LabelStmtASTNode *statement,
                 SwitchCases *cases) {
    Node *name = statement->getLabel();
    const auto key = identifier(name);
    auto &label = function.labels[key];
    if (label.definition) {
        function.error(name, "duplicate label '" + key + "'", "cephyr.statement.label");
    } else {
        if (label.target == no_value)
            label.target = function.block();
        label.definition = name;
        // An edge to a fixed-size automatic object's scope does not run any
        // initializer preceding the label. Its frame address already dominates
        // every block. Variably modified declarations are rejected by read_type.
        function.jump(label.target);
        function.select(label.target);
    }
    lower_statement(function, statement->getStmt(), cases);
}

void lower_parameter_slots(Function &function, Node *parameters) {
    struct Parameter {
        Node *node;
        std::string name;
        CType object_type;
    };
    std::vector<Parameter> lowered;
    const auto declarations = elements(parameters);
    const auto &parameter_types = function.symbol.type.params;
    PrototypeScope parameter_scope{nullptr, {}};
    auto parameter_context = TypeContext(function.unit, &function);
    parameter_context.prototype = &parameter_scope;

    for (Node *node : declarations) {
        if (!present(node))
            continue;
        if (node->getKind() != cparser::NK_PARM_DECL) {
            function.error(node, "unsupported function parameter declaration",
                           "cephyr.parameter");
            continue;
        }
        auto *parameter = static_cast<cparser::ParmDeclASTNode *>(node);
        CType type = read_parameter_type(parameter->getType(), parameter_context, true);
        if (type.kind == CType::Kind::void_ && declarations.size() == 1 &&
            parameter_types.empty()) {
            if (!identifier(parameter->getName()).empty())
                function.error(node, "void parameter must not have an identifier",
                               "cephyr.parameter.type");
            break;
        }
        if (!type.valid() || !(type.scalar() || type.kind == CType::Kind::record)) {
            function.error(node, "parameter type is not supported by native MachineIR",
                           "cephyr.parameter.type");
            continue;
        }
        CType adjusted = type;
        adjusted.clear_qualifiers();
        if (lowered.size() >= parameter_types.size() ||
            !compatible(adjusted, parameter_types[lowered.size()])) {
            function.error(node, "parameter list does not match the function type",
                           "cephyr.parameter.type");
            continue;
        }
        auto name = identifier(parameter->getName());
        if (!name.empty())
            parameter_scope.parameters.emplace(name,
                TypeBinding{type, (node->getFlags() & SCS_REGISTER) != 0});
        lowered.push_back({node, std::move(name), std::move(type)});
    }
    if (lowered.size() != parameter_types.size()) {
        function.error(parameters, "parameter list does not match the function type",
                       "cephyr.parameter.type");
        return;
    }

    // ABI parameters must occupy the first value IDs. Allocate all IDs before
    // frame_addr instructions for their normalized local slots.
    std::vector<Id> incoming;
    for (const CType &type : abi_parameter_types(function.symbol.type, true))
        incoming.push_back(function.value(type));
    auto arg = incoming.begin();
    const auto result = record_abi(*function.symbol.type.element);
    if (result == RecordABI::indirect)
        function.record_result_address = *arg++;
    else if (result == RecordABI::pair)
        function.record_result_address = incoming.back();

    for (std::size_t index = 0; index != lowered.size(); ++index) {
        const Parameter &parameter = lowered[index];
        const auto abi = record_abi(parameter_types[index]);
        const Id first = *arg++;
        const Id second = abi == RecordABI::pair ? *arg++ : no_value;
        if (parameter.name.empty()) {
            function.error(parameter.node, "function parameter has no identifier",
                           "cephyr.parameter.name");
            continue;
        }
        LValue slot = function.local(parameter.object_type, parameter.node);
        if (slot.address == no_value)
            continue;
        slot.register_object = (parameter.node->getFlags() & SCS_REGISTER) != 0;
        function.declare_local(parameter.name, slot, parameter.node);
        if (slot.type.kind == CType::Kind::record) {
            if (slot.type.is_volatile)
                function.error(parameter.node, "volatile by-value aggregate parameters are not supported",
                               "cephyr.abi.aggregate");
            else if (abi == RecordABI::indirect)
                copy_record(function, slot, {parameter_types[index], first, std::nullopt},
                            parameter.node, true);
            else {
                unpack_record_abi(function, slot, first);
                if (abi == RecordABI::pair)
                    unpack_record_abi(function, slot, second, 1);
            }
        } else {
            slot.type.is_const = false; // Bind the incoming value, not a C assignment.
            store(function, slot, Expr{parameter_types[index], first, std::nullopt},
                  parameter.node);
        }
    }
}

void lower_statement(Function &function, Node *statement, SwitchCases *cases) {
    const AnalysisDepth depth(function.statement_depth);
    if (!present(statement))
        return;
    if (statement->isType()) {
        const CType type = read_type(statement, TypeContext(function.unit, &function), true);
        if (type.valid() && type.kind != CType::Kind::record &&
            statement->getKind() != cparser::NK_ENUMERAL_TYPE)
            function.error(statement, "block-scope declaration declares no object",
                           "cephyr.local.declaration");
        return;
    }

    switch (statement->getKind()) {
    case cparser::NK_LIST:
        lower_sequence(function, statement, cases);
        return;
    case cparser::NK_COMPOUND_STMT:
        lower_compound(function,
                       static_cast<cparser::CompoundStmtASTNode *>(statement), true, cases);
        return;
    case cparser::NK_VAR_DECL:
    case cparser::NK_TYPE_DECL:
    case cparser::NK_FUNCTION_DECL:
        lower_local_declaration(function, statement);
        return;
    case cparser::NK_STATIC_ASSERT:
        check_static_assert(TypeContext(function.unit, &function), statement);
        return;
    case cparser::NK_IF_STMT:
        lower_if(function, static_cast<cparser::IfStmtASTNode *>(statement), cases);
        return;
    case cparser::NK_WHILE_STMT:
        lower_while(function, static_cast<cparser::WhileStmtASTNode *>(statement), cases);
        return;
    case cparser::NK_DO_STMT:
        lower_do(function, static_cast<cparser::DoStmtASTNode *>(statement), cases);
        return;
    case cparser::NK_FOR_STMT:
        lower_for(function, static_cast<cparser::ForStmtASTNode *>(statement), cases);
        return;
    case cparser::NK_BREAK_STMT:
        if (function.loops.empty()) {
            function.error(statement, "break statement is not within a loop or switch",
                           "cephyr.statement.break");
            function.trap();
        } else {
            function.jump(function.loops.back().first);
        }
        return;
    case cparser::NK_CONTINUE_STMT: {
        const auto loop = std::find_if(function.loops.rbegin(), function.loops.rend(),
                                       [](auto targets) { return targets.second != no_value; });
        if (loop == function.loops.rend()) {
            function.error(statement, "continue statement is not within a loop",
                           "cephyr.statement.continue");
            function.trap();
        } else {
            function.jump(loop->second);
        }
        return;
    }
    case cparser::NK_RETURN_STMT:
        lower_return(function,
                     static_cast<cparser::ReturnStmtASTNode *>(statement));
        return;
    case cparser::NK_NOP_EXPR:
        return;
    case cparser::NK_SWITCH_STMT:
        lower_switch(function, static_cast<cparser::SwitchStmtASTNode *>(statement));
        return;
    case cparser::NK_CASE_LABEL:
        lower_case(function, static_cast<cparser::CaseLabelASTNode *>(statement), cases);
        return;
    case cparser::NK_GOTO_STMT:
        lower_goto(function, static_cast<cparser::GotoStmtASTNode *>(statement));
        return;
    case cparser::NK_LABEL_STMT:
        lower_label(function, static_cast<cparser::LabelStmtASTNode *>(statement), cases);
        return;
    case cparser::NK_ASM_STMT:
        function.error(statement, "inline assembly is not supported",
                       "cephyr.statement.unsupported");
        function.trap();
        return;
    default:
        if (expression_kind(statement->getKind())) {
            (void)lower_expr(function, statement);
            return;
        }
        function.error(statement, "unsupported local declaration or statement syntax",
                       "cephyr.statement.unsupported");
        function.trap();
        return;
    }
}

} // namespace

LValue lower_compound_literal(Function &function, Node *node) {
    CType type;
    const auto plan = plan_compound_literal(TypeContext(function.unit, &function), node, type, true);
    if (!plan)
        return {};
    auto [slot, inserted] = function.compound_literal_slots.try_emplace(node);
    if (inserted)
        slot->second = function.local(type, node);
    const auto object = slot->second;
    if (object.address == no_value)
        return {};
    // The frame slot is fixed per occurrence and per invocation, but the
    // initialization runs here, each time control reaches the expression.
    auto *initializer = static_cast<cparser::CompoundLiteralASTNode *>(node)->getInit();
    if (type.scalar())
        initialize_scalar(function, object, initializer);
    else
        initialize_object(function, object, *plan, *size_of(type), initializer, true);
    return object;
}

void lower_function_body(Function &function, Node *parameters, Node *body) {
    const auto initial_errors = function.unit.diagnostics.error_count();
    lower_parameter_slots(function, parameters);
    if (function.symbol.type.variadic &&
        function.unit.diagnostics.error_count() == initial_errors) {
        const auto declarations = elements(parameters);
        if (!declarations.empty() && declarations.back()->getKind() == cparser::NK_PARM_DECL) {
            auto *last = static_cast<cparser::ParmDeclASTNode *>(declarations.back());
            const auto slot = function.find_local(identifier(last->getName()));
            const auto declared = read_type(last->getType(), TypeContext(function.unit, &function), true);
            if (slot && !slot->register_object && declared.scalar() &&
                (!declared.integer() || declared.bits >= 32) &&
                (!declared.real() || declared.bits == 64))
                function.va_start_parameter = slot->address;
        }
    }

    // C11 6.4.2.2: as if `static const char __func__[] = "name";`
    // immediately followed the function body's opening brace.
    auto character = plain_char_type(function.unit.source);
    character.is_const = true;
    CType function_name;
    function_name.kind = CType::Kind::array;
    function_name.element = std::make_shared<const CType>(std::move(character));
    function_name.count = function.source_name.size() + 1;
    function.function_name_symbol = "__cephyr_tu_func_" + std::to_string(function.unit.function_names++);
    function.declare_local("__func__", {std::move(function_name), no_value,
                                        function.function_name_symbol}, body);

    // Function parameters and declarations in the outermost compound statement
    // share one C scope.
    if (present(body) && body->getKind() == cparser::NK_COMPOUND_STMT)
        lower_compound(function, static_cast<cparser::CompoundStmtASTNode *>(body),
                       false, nullptr);
    else
        lower_statement(function, body, nullptr);

    for (const auto &[name, label] : function.labels)
        if (!label.definition)
            function.error(label.first_reference, "goto references undefined label '" + name + "'",
                           "cephyr.statement.goto");

    const auto reachable = reachable_blocks(function.blocks);
    if (function.open()) {
        const bool reaches_end =
            function.current < reachable.size() && reachable[function.current];
        const CType result = function_result(function);

        if (!reaches_end) {
            function.trap();
        } else if (is_main(function) && result.integer()) {
            Expr zero{result, function.constant(0, result), 0};
            function.ret(abi_value(function, zero));
        } else if (result.kind == CType::Kind::void_) {
            function.ret();
        } else {
            function.warn(body,
                          "control reaches the end of a non-void function; "
                          "the undefined path will trap",
                          "cephyr.return.fallthrough");
            function.trap();
        }
    }

    // Keep diagnostic-only unreachable continuations structurally complete;
    // the unit finalizer subsequently prunes them. Any other open block is a
    // lowering bug rather than a source-language fallthrough.
    for (std::size_t index = 0; index != function.blocks.size(); ++index) {
        Block &block = function.blocks[index];
        if (block.term.kind == Term::Kind::open) {
            if (index < reachable.size() && reachable[index] &&
                function.unit.diagnostics.error_count() == initial_errors)
                function.error(body, "statement lowering left a reachable block open",
                               "cephyr.frontend.cfg");
            block.term.kind = Term::Kind::trap;
        }
    }
}

} // namespace cephyr::detail
