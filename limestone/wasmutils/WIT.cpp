#include "WITParser.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace limestone::wasmutils {
namespace {

constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool letter(char c) { return lower(c) || upper(c); }
constexpr char fold(char c) { return upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr unsigned max_type_depth = 128;
constexpr unsigned max_dependency_depth = 512;

bool valid_name(std::string_view name) {
    if (name.empty() || !letter(name.front()))
        return false;
    bool seen_lower = false;
    bool seen_upper = false;
    bool nonempty = false;
    for (char c : name) {
        if (c == '-') {
            if (!nonempty)
                return false;
            seen_lower = seen_upper = nonempty = false;
        } else {
            if (!(letter(c) || digit(c)))
                return false;
            nonempty = true;
            seen_lower |= lower(c);
            seen_upper |= upper(c);
            if (seen_lower && seen_upper)
                return false;
        }
    }
    return nonempty;
}

bool valid_version(std::string_view text) {
    for (int part = 0; part < 3; ++part) {
        std::size_t length = 0;
        while (length < text.size() && digit(text[length]))
            ++length;
        if (length == 0 || (length > 1 && text.front() == '0'))
            return false;
        text.remove_prefix(length);
        if (part < 2) {
            if (!text.starts_with('.'))
                return false;
            text.remove_prefix(1);
        }
    }
    const auto suffix = [&](bool prerelease) {
        do {
            text.remove_prefix(1); // '-', '+', or '.'
            std::size_t length = 0;
            bool numeric = true;
            while (length < text.size() &&
                   (letter(text[length]) || digit(text[length]) || text[length] == '-')) {
                numeric &= digit(text[length]);
                ++length;
            }
            if (length == 0 || (prerelease && numeric && length > 1 && text.front() == '0'))
                return false;
            text.remove_prefix(length);
        } while (text.starts_with('.'));
        return true;
    };
    if (text.starts_with('-') && !suffix(true))
        return false;
    if (text.starts_with('+') && !suffix(false))
        return false;
    return text.empty();
}

struct ScalarMapping {
    WitScalar kind;
    std::string_view spelling;
};

constexpr ScalarMapping scalars[] = {
    {WitScalar::Bool, "bool"}, {WitScalar::U8, "u8"}, {WitScalar::S8, "s8"},
    {WitScalar::U16, "u16"}, {WitScalar::S16, "s16"},
    {WitScalar::U32, "u32"}, {WitScalar::S32, "s32"},
    {WitScalar::U64, "u64"}, {WitScalar::S64, "s64"},
    {WitScalar::F32, "f32"}, {WitScalar::F64, "f64"}, {WitScalar::Char, "char"},
};

bool is_keyword(std::string_view word) {
    constexpr std::string_view keywords[] = {
        "as", "async", "borrow", "constructor", "enum", "export", "flags",
        "from", "func", "future", "import", "include", "interface", "list",
        "map", "option", "own", "package", "record", "resource", "result",
        "static", "stream", "string", "tuple", "type", "use", "variant",
        "with", "world",
    };
    for (auto keyword : keywords)
        if (word == keyword)
            return true;
    for (const auto& scalar : scalars)
        if (word == scalar.spelling)
            return true;
    return false;
}

struct Token {
    std::string_view text;
    WitLocation location;
    std::vector<WitDocComment> docs;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        std::vector<WitDocComment> docs;
        while (!at_end()) {
            if (space(peek())) {
                advance();
            } else if (starts("//")) {
                const auto start = location_;
                const bool doc = starts("///") && !starts("////");
                advance();
                advance();
                while (!at_end() && peek() != '\r' && peek() != '\n')
                    advance(true);
                if (doc) {
                    const auto text = source_.substr(start.offset + 3, location_.offset - start.offset - 3);
                    docs.push_back({std::string(text), start});
                }
            } else if (starts("/*")) {
                const auto start = location_;
                const bool doc = starts("/**") && !starts("/***");
                skip_block_comment();
                if (doc) {
                    const auto size = location_.offset - start.offset;
                    const auto text = source_.substr(start.offset + 3, size > 5 ? size - 5 : 0);
                    docs.push_back({std::string(text), start});
                }
            } else {
                break;
            }
        }
        const WitLocation start = location_;
        if (at_end())
            return {{}, start, std::move(docs)};

        const char c = peek();
        if (letter(c) || c == '%') {
            // Leave '->' intact even when adjacent to a name.
            advance();
            while (!at_end() &&
                   (letter(peek()) || digit(peek()) || (peek() == '-' && !starts("->"))))
                advance();
        } else if (digit(c)) {
            // Keep a SemVer spelling contiguous. The final dot in a use
            // path, before its member list, belongs to the grammar instead.
            do {
                advance();
            } while (!at_end() && (letter(peek()) || digit(peek()) || peek() == '+' ||
                     (peek() == '-' && !starts("->")) ||
                     (peek() == '.' && (letter(peek(1)) || digit(peek(1)) || peek(1) == '-'))));
        } else if (starts("->")) {
            advance();
            advance();
        } else if (c == '"') {
            advance();
            while (!at_end() && peek() != '"') {
                if (peek() == '\r' || peek() == '\n')
                    throw WitError(start, "unterminated WIT attribute string");
                if (peek() == '\\') {
                    advance();
                    if (at_end() || peek() == '\r' || peek() == '\n')
                        throw WitError(start, "unterminated WIT attribute string");
                }
                advance(true);
            }
            if (at_end())
                throw WitError(start, "unterminated WIT attribute string");
            advance();
        } else {
            advance();
        }
        return {source_.substr(start.offset, location_.offset - start.offset), start, std::move(docs)};
    }

private:
    std::string_view source_;
    WitLocation location_;

    bool at_end() const { return location_.offset == source_.size(); }
    char peek(std::size_t ahead = 0) const {
        return ahead < source_.size() - location_.offset ? source_[location_.offset + ahead] : '\0';
    }
    bool starts(std::string_view prefix) const {
        return source_.substr(location_.offset).starts_with(prefix);
    }

    void advance(bool allow_utf8 = false) {
        const char c = peek();
        const auto byte = static_cast<unsigned char>(c);
        if (allow_utf8 && byte >= 0x80) {
            const unsigned length = byte >= 0xc2 && byte <= 0xdf ? 2 :
                                    byte >= 0xe0 && byte <= 0xef ? 3 :
                                    byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
            if (!length)
                throw WitError(location_, "invalid UTF-8 in WIT source");
            const auto first = static_cast<unsigned char>(peek(1));
            if ((byte == 0xe0 && first < 0xa0) || (byte == 0xed && first >= 0xa0) ||
                (byte == 0xf0 && first < 0x90) || (byte == 0xf4 && first >= 0x90))
                throw WitError(location_, "invalid UTF-8 in WIT source");
            for (unsigned i = 1; i < length; ++i)
                if ((static_cast<unsigned char>(peek(i)) & 0xc0) != 0x80)
                    throw WitError(location_, "invalid UTF-8 in WIT source");
            location_.offset += length;
            location_.column += length;
            return;
        }
        if (!space(c) && (byte < '!' || byte > '~'))
            throw WitError(location_, "expected printable ASCII or WIT whitespace");
        ++location_.offset;
        if (c == '\r' || (c == '\n' &&
                          (location_.offset == 1 || source_[location_.offset - 2] != '\r'))) {
            ++location_.line;
            location_.column = 1;
        } else if (c != '\n') {
            ++location_.column;
        }
    }

    void skip_block_comment() {
        const auto start = location_;
        std::size_t depth = 1;
        advance();
        advance();
        while (depth != 0) {
            if (at_end())
                throw WitError(start, "unterminated block comment");
            if (starts("/*")) {
                ++depth;
                advance();
                advance();
            } else if (starts("*/")) {
                --depth;
                advance();
                advance();
            } else {
                advance(true);
            }
        }
    }
};

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    WitDocument parse() {
        WitDocument document;
        auto attrs = annotations();
        if (take("package")) {
            document.package = package();
            document.package->annotations = std::move(attrs);
            expect(";");
            attrs = annotations();
        }
        while (!at_end()) {
            if (take("interface"))
                document.interfaces.push_back(interface_decl(name(), std::move(attrs)));
            else if (take("world"))
                document.worlds.push_back(world(std::move(attrs)));
            else if (take("use"))
                document.uses.push_back(standalone_use(std::move(attrs)));
            else
                fail("expected an interface, world, or top-level use declaration");
            attrs = annotations();
        }
        if (!attrs.attributes.empty())
            fail("expected a declaration after attributes");
        return document;
    }

private:
    Lexer lexer_;
    Token token_;

    [[noreturn]] void fail(const std::string& message) const {
        throw WitError(token_.location, message);
    }

    bool at_end() const { return token_.text.empty(); }
    bool at(std::string_view text) const { return token_.text == text; }
    void bump() { token_ = lexer_.next(); }

    bool take(std::string_view text) {
        if (!at(text))
            return false;
        bump();
        return true;
    }

    void expect(std::string_view text) {
        if (!take(text))
            fail("expected '" + std::string(text) + "'");
    }

    WitName name(bool allow_keyword = false) {
        auto text = token_.text;
        const bool escaped = text.starts_with('%');
        if (escaped)
            text.remove_prefix(1);
        if (!valid_name(text))
            fail("expected a kebab-case WIT identifier");
        if (!escaped && !allow_keyword && is_keyword(text))
            fail("unexpected WIT keyword where a name was expected (escape it with '%')");
        WitName result{std::string(text), token_.location};
        bump();
        return result;
    }

    template<class ParseItem>
    void comma_list(std::string_view end, ParseItem parse_item) {
        if (take(end))
            return;
        for (;;) {
            parse_item();
            if (!take(",")) {
                expect(end);
                return;
            }
            if (take(end))
                return;
        }
    }

    WitAnnotations annotations() {
        WitAnnotations result;
        for (;;) {
            // Documentation comments remain whitespace outside declaration
            // boundaries, just like ordinary comments.
            for (auto& doc : token_.docs)
                result.docs.push_back(std::move(doc));
            token_.docs.clear();
            if (take("@")) {
                WitAttribute attr{name(true), {}};
                if (take("(")) {
                    comma_list(")", [&] {
                        auto key = name(true);
                        expect("=");
                        if (at_end() ||
                            !(letter(token_.text.front()) || digit(token_.text.front()) ||
                              token_.text.starts_with('"') || token_.text.starts_with('%')))
                            fail("expected an attribute argument value");
                        attr.arguments.push_back({std::move(key), std::string(token_.text),
                                                  token_.location});
                        bump();
                    });
                }
                result.attributes.push_back(std::move(attr));
            } else {
                return result;
            }
        }
    }

    std::optional<WitVersion> version() {
        if (!take("@"))
            return std::nullopt;
        if (!valid_version(token_.text))
            fail("expected a SemVer version (major.minor.patch with optional suffixes)");
        WitVersion result{std::string(token_.text), token_.location};
        bump();
        return result;
    }

    WitPackage package() {
        auto name_space = name();
        expect(":");
        return {std::move(name_space), name(), version()};
    }

    WitPath qualified_path(WitName name_space) {
        auto package_name = name();
        expect("/");
        auto item_name = name();
        return {WitPackage{std::move(name_space), std::move(package_name), version()},
                std::move(item_name)};
    }

    WitPath path() {
        auto first = name();
        if (take(":"))
            return qualified_path(std::move(first));
        return {std::nullopt, std::move(first)};
    }

    WitStandaloneUse standalone_use(WitAnnotations attrs) {
        WitStandaloneUse result{path(), std::nullopt, std::move(attrs)};
        if (take("as"))
            result.alias = name();
        expect(";");
        return result;
    }

    WitUse use(WitAnnotations attrs) {
        WitUse result{path(), {}, std::move(attrs)};
        expect(".");
        expect("{");
        comma_list("}", [&] {
            WitUseItem item{name(), std::nullopt};
            if (take("as"))
                item.alias = name();
            result.items.push_back(std::move(item));
        });
        expect(";");
        return result;
    }

    WitInclude include(WitAnnotations attrs) {
        WitInclude result{path(), {}, std::move(attrs)};
        if (take("with")) {
            expect("{");
            comma_list("}", [&] {
                auto original = name();
                expect("as");
                result.renames.push_back({std::move(original), name()});
            });
            take(";");
        } else {
            expect(";");
        }
        return result;
    }

    std::optional<WitTypeDefKind> type_def_kind() {
        if (take("type")) return WitTypeDefKind::Alias;
        if (take("record")) return WitTypeDefKind::Record;
        if (take("variant")) return WitTypeDefKind::Variant;
        if (take("enum")) return WitTypeDefKind::Enum;
        if (take("flags")) return WitTypeDefKind::Flags;
        if (take("resource")) return WitTypeDefKind::Resource;
        return std::nullopt;
    }

    WitInterface interface_decl(WitName interface_name, WitAnnotations attrs) {
        WitInterface result{std::move(interface_name), {}};
        result.annotations = std::move(attrs);
        expect("{");
        while (!take("}")) {
            auto item_attrs = annotations();
            if (take("use"))
                result.uses.push_back(use(std::move(item_attrs)));
            else if (const auto kind = type_def_kind())
                result.types.push_back(type_def(*kind, std::move(item_attrs)));
            else {
                auto function_name = name();
                expect(":");
                result.functions.push_back(function(std::move(function_name),
                    WitFunctionKind::Freestanding, std::move(item_attrs)));
            }
        }
        return result;
    }

    WitWorld world(WitAnnotations attrs) {
        WitWorld result{name()};
        result.annotations = std::move(attrs);
        expect("{");
        while (!take("}")) {
            auto item_attrs = annotations();
            if (take("use"))
                result.uses.push_back(use(std::move(item_attrs)));
            else if (take("include"))
                result.includes.push_back(include(std::move(item_attrs)));
            else if (const auto kind = type_def_kind())
                result.types.push_back(type_def(*kind, std::move(item_attrs)));
            else {
                const bool is_import = take("import");
                if (!is_import && !take("export"))
                    fail("expected a world import, export, include, type, use, or '}'");
                result.items.push_back(world_item(is_import, std::move(item_attrs)));
            }
        }
        return result;
    }

    WitWorldItem world_item(bool is_import, WitAnnotations attrs) {
        auto first = name();
        WitWorldItem result{WitWorldItemKind::Path, is_import};
        result.annotations = std::move(attrs);
        if (!take(":")) {
            result.path = WitPath{std::nullopt, std::move(first)};
            expect(";");
        } else if (at("func")) {
            result.kind = WitWorldItemKind::Function;
            result.function = function(std::move(first), WitFunctionKind::Freestanding, {});
        } else if (take("interface")) {
            result.kind = WitWorldItemKind::Interface;
            result.interface = interface_decl(std::move(first), {});
            take(";");
        } else {
            result.path = qualified_path(std::move(first));
            expect(";");
        }
        return result;
    }

    WitParameter parameter() {
        auto attrs = annotations();
        auto parameter_name = name();
        expect(":");
        return {std::move(parameter_name), type(), std::move(attrs)};
    }

    WitFunction function(WitName function_name, WitFunctionKind kind, WitAnnotations attrs) {
        WitFunction result{std::move(function_name), {}, std::nullopt, kind, std::move(attrs)};
        if (kind != WitFunctionKind::Constructor)
            expect("func");
        expect("(");
        comma_list(")", [&] { result.parameters.push_back(parameter()); });
        if (kind != WitFunctionKind::Constructor && take("->"))
            result.result = type();
        expect(";");
        return result;
    }

    WitTypeDef type_def(WitTypeDefKind kind, WitAnnotations attrs) {
        WitTypeDef result{name(), kind};
        result.annotations = std::move(attrs);
        if (kind == WitTypeDefKind::Alias) {
            expect("=");
            result.alias = type();
            expect(";");
        } else if (kind == WitTypeDefKind::Resource) {
            if (!take(";")) {
                expect("{");
                while (!take("}")) {
                    auto item_attrs = annotations();
                    const auto location = token_.location;
                    if (take("constructor")) {
                        result.functions.push_back(function({"constructor", location},
                            WitFunctionKind::Constructor, std::move(item_attrs)));
                    } else {
                        auto function_name = name();
                        expect(":");
                        const auto function_kind = take("static") ? WitFunctionKind::Static :
                                                                    WitFunctionKind::Method;
                        result.functions.push_back(function(std::move(function_name),
                            function_kind, std::move(item_attrs)));
                    }
                }
            }
        } else {
            expect("{");
            if (kind == WitTypeDefKind::Record) {
                comma_list("}", [&] { result.fields.push_back(parameter()); });
            } else {
                comma_list("}", [&] {
                    auto item_attrs = annotations();
                    WitCase item{name(), std::nullopt, std::move(item_attrs)};
                    if (kind == WitTypeDefKind::Variant && take("(")) {
                        item.payload = type();
                        expect(")");
                    }
                    result.cases.push_back(std::move(item));
                });
            }
        }
        return result;
    }

    WitType type(unsigned depth = 0) {
        if (depth == max_type_depth)
            fail("WIT type nesting is too deep");
        const auto location = token_.location;
        for (const auto& scalar : scalars)
            if (take(scalar.spelling))
                return {scalar.kind, location};
        WitType result{WitScalar::Bool, location};
        if (take("string")) {
            result.kind = WitTypeKind::String;
        } else if (at("list") || at("option")) {
            result.kind = at("list") ? WitTypeKind::List : WitTypeKind::Option;
            bump();
            expect("<");
            result.arguments.push_back(type(depth + 1));
            expect(">");
        } else if (take("tuple")) {
            result.kind = WitTypeKind::Tuple;
            expect("<");
            comma_list(">", [&] { result.arguments.push_back(type(depth + 1)); });
        } else if (take("result")) {
            result.kind = WitTypeKind::Result;
            if (take("<")) {
                if (at("_")) {
                    const auto unit_location = token_.location;
                    bump();
                    result.arguments.push_back({WitScalar::Bool, unit_location, WitTypeKind::Unit});
                    expect(",");
                    result.arguments.push_back(type(depth + 1));
                } else {
                    result.arguments.push_back(type(depth + 1));
                    if (take(","))
                        result.arguments.push_back(type(depth + 1));
                }
                expect(">");
            }
        } else if (at("own") || at("borrow")) {
            result.kind = at("own") ? WitTypeKind::Own : WitTypeKind::Borrow;
            bump();
            expect("<");
            result.name = name();
            expect(">");
        } else {
            result.kind = WitTypeKind::Named;
            result.name = name();
        }
        return result;
    }
};

[[noreturn]] void invalid(WitLocation at, const std::string& message) {
    throw WitError(at, message);
}

void check_name(const WitName& name) {
    if (!valid_name(name.text))
        invalid(name.location, "invalid WIT identifier");
}

std::string key(std::string_view spelling) {
    // Strong uniqueness is distinct from exact-spelling name resolution.
    std::string result;
    for (char c : spelling)
        if (c != '-')
            result += fold(c);
    return result;
}

void unique(std::set<std::string>& names, const WitName& name, std::string_view scope) {
    check_name(name);
    if (!names.insert(key(name.text)).second)
        invalid(name.location, "duplicate " + std::string(scope) + " name '" + name.text + "'");
}

struct NameCheck {
    const WitName* name;
    std::string_view scope;
};

void check_names(std::vector<NameCheck>& declarations) {
    std::stable_sort(declarations.begin(), declarations.end(), [](auto a, auto b) {
        return a.name->location.offset < b.name->location.offset;
    });
    std::set<std::string> names;
    for (const auto& declaration : declarations)
        unique(names, *declaration.name, declaration.scope);
}

void check_annotations(const WitAnnotations& annotations) {
    for (const auto& attribute : annotations.attributes) {
        check_name(attribute.name);
        std::set<std::string> arguments;
        for (const auto& argument : attribute.arguments) {
            unique(arguments, argument.name, "attribute argument");
            if (argument.value.empty())
                invalid(argument.location, "empty attribute argument value");
        }
    }
}

void check_package(const WitPackage& package) {
    check_name(package.name_space);
    check_name(package.name);
    if (package.version && !valid_version(package.version->text))
        invalid(package.version->location, "invalid WIT package version");
    check_annotations(package.annotations);
}

std::string package_key(const WitPackage& package) {
    // The '/' terminator distinguishes an unversioned ID from a versioned one.
    auto result = package.name_space.text + ":" + package.name.text;
    if (package.version)
        result += "@" + package.version->text;
    return result + "/";
}

std::string strong_path_key(std::string_view path) {
    const auto version = path.find('@');
    if (version == std::string_view::npos)
        return key(path);
    const auto slash = path.find('/');
    return key(path.substr(0, version)) + std::string(path.substr(version, slash - version)) +
           "/" + key(path.substr(slash + 1));
}

struct Target {
    const WitInterface* interface = nullptr;
    const WitWorld* world = nullptr;
    std::string id;

    bool foreign() const { return !interface && !world; }
};

enum class Visit { New, Active, Done };

struct TypeInfo {
    bool resource = false; // A resource declaration or an alias of one.
    bool borrowed = false; // Contains a borrow, possibly through a named type.
    bool unknown = false; // An unresolved type from a dependency.
};

struct Binding {
    const WitTypeDef* definition = nullptr;
    const WitUse* use = nullptr;
    const WitUseItem* import = nullptr;
    Visit state = Visit::New;
    TypeInfo info{};
};

struct Scope {
    std::map<std::string, Binding> types;
};

struct WorldNames {
    // Strong-uniqueness key -> exact spelling.
    std::map<std::string, std::string> named;
    std::map<std::string, std::string> paths;
};

struct WorldInfo {
    Visit state = Visit::New;
    WorldNames imports;
    WorldNames exports;
    bool foreign_include = false;
};

class Validator {
public:
    explicit Validator(const WitDocument& document) : document_(document) {}

    void run() {
        if (document_.interfaces.empty() && document_.worlds.empty() &&
            document_.uses.empty() && !document_.package)
            invalid({}, "expected a WIT declaration");
        if (document_.package) {
            check_package(*document_.package);
            local_package_ = package_key(*document_.package);
        }

        std::vector<NameCheck> names;
        for (const auto& interface : document_.interfaces) {
            names.push_back({&interface.name, "top-level"});
            interfaces_.emplace(interface.name.text, &interface);
        }
        for (const auto& world : document_.worlds) {
            names.push_back({&world.name, "top-level"});
            worlds_.emplace(world.name.text, &world);
        }
        for (const auto& use : document_.uses) {
            const auto& name = use.alias ? *use.alias : use.path.name;
            names.push_back({&name, "top-level"});
            aliases_.emplace(name.text, &use);
        }
        check_names(names);

        for (const auto& interface : document_.interfaces)
            collect_interface(interface);
        for (const auto& world : document_.worlds) {
            check_annotations(world.annotations);
            collect_scope(world_scopes_[&world], world.types, world.uses);
            for (const auto& item : world.items) {
                check_world_item(item, world.name.location);
                if (item.interface)
                    collect_interface(*item.interface);
            }
        }
        for (const auto& use : document_.uses) {
            check_path(use.path);
            check_annotations(use.annotations);
            resolve_path(use.path, 0);
        }

        for (const auto& interface : document_.interfaces)
            check_interface_dependencies(interface, interface.name.location, 0);
        for (const auto& interface : document_.interfaces)
            check_interface(interface);
        for (const auto& world : document_.worlds) {
            auto& scope = world_scopes_.at(&world);
            check_scope(scope, world.types, world.uses);
            for (const auto& item : world.items) {
                if (item.function)
                    check_function(scope, *item.function, false);
                if (item.interface)
                    check_interface(*item.interface);
            }
        }
        for (const auto& world : document_.worlds)
            check_world(world, world.name.location, 0);
    }

private:
    const WitDocument& document_;
    std::string local_package_;
    std::map<std::string, const WitStandaloneUse*> aliases_;
    std::map<std::string, const WitInterface*> interfaces_;
    std::map<std::string, const WitWorld*> worlds_;
    std::map<const WitStandaloneUse*, Visit> alias_states_;
    std::map<const WitStandaloneUse*, Target> alias_targets_;
    std::map<std::string, bool> foreign_kinds_; // true = interface, false = world.
    std::map<const WitInterface*, Visit> interface_states_;
    std::map<const WitInterface*, Scope> interface_scopes_;
    std::map<const WitWorld*, Scope> world_scopes_;
    std::map<const WitWorld*, WorldInfo> world_infos_;

    static void check_path(const WitPath& path) {
        check_name(path.name);
        if (path.package)
            check_package(*path.package);
    }

    Target resolve_path(const WitPath& path, unsigned depth) {
        check_path(path);
        const auto& name = path.name.text;
        if (path.package) {
            auto package = package_key(*path.package);
            if (package != local_package_)
                return {nullptr, nullptr, std::move(package) + name};
        } else if (const auto it = aliases_.find(name); it != aliases_.end()) {
            const auto* alias = it->second;
            auto& state = alias_states_[alias];
            if (state == Visit::Active)
                invalid(path.name.location, "cyclic top-level use involving '" + path.name.text + "'");
            if (state == Visit::Done)
                return alias_targets_.at(alias);
            if (depth == max_dependency_depth)
                invalid(path.name.location, "WIT top-level use chain is too deep");
            state = Visit::Active;
            auto target = resolve_path(alias->path, depth + 1);
            alias_targets_.emplace(alias, target);
            state = Visit::Done;
            return target;
        }
        if (const auto it = interfaces_.find(name); it != interfaces_.end())
            return {it->second, nullptr, local_package_ + name};
        if (const auto it = worlds_.find(name); it != worlds_.end())
            return {nullptr, it->second, local_package_ + name};
        invalid(path.name.location, "unknown WIT interface or world '" + path.name.text + "'");
    }

    Target interface_target(const WitPath& path) {
        auto target = resolve_path(path, 0);
        if (target.world)
            invalid(path.name.location, "expected a WIT interface, found a world");
        check_foreign_kind(path, target, true);
        return target;
    }

    void check_foreign_kind(const WitPath& path, const Target& target, bool interface) {
        if (!target.foreign())
            return;
        const auto [entry, inserted] = foreign_kinds_.emplace(target.id, interface);
        if (!inserted && entry->second != interface)
            invalid(path.name.location, "external WIT item used as both an interface and a world");
    }

    void collect_scope(Scope& scope, const std::vector<WitTypeDef>& types,
                       const std::vector<WitUse>& uses,
                       const std::vector<WitFunction>* functions = nullptr) {
        std::vector<NameCheck> names;
        for (const auto& type : types) {
            names.push_back({&type.name, "interface or world item"});
            check_annotations(type.annotations);
            scope.types.emplace(type.name.text, Binding{&type});
        }
        for (const auto& use : uses) {
            check_path(use.path);
            check_annotations(use.annotations);
            for (const auto& item : use.items) {
                check_name(item.name);
                const auto& name = item.alias ? *item.alias : item.name;
                names.push_back({&name, "interface or world item"});
                scope.types.emplace(name.text, Binding{nullptr, &use, &item});
            }
        }
        if (functions)
            for (const auto& function : *functions)
                names.push_back({&function.name, "interface item"});
        check_names(names);
    }

    void collect_interface(const WitInterface& interface) {
        check_name(interface.name);
        check_annotations(interface.annotations);
        collect_scope(interface_scopes_[&interface], interface.types, interface.uses,
                      &interface.functions);
    }

    static void check_world_item(const WitWorldItem& item, WitLocation fallback) {
        check_annotations(item.annotations);
        switch (item.kind) {
        case WitWorldItemKind::Path:
            if (item.path && !item.function && !item.interface) {
                check_path(*item.path);
                return;
            }
            break;
        case WitWorldItemKind::Function:
            if (!item.path && item.function && !item.interface)
                return;
            break;
        case WitWorldItemKind::Interface:
            if (!item.path && !item.function && item.interface)
                return;
            break;
        }
        invalid(fallback, "invalid WIT world item payload");
    }

    void check_interface_dependencies(const WitInterface& interface, WitLocation at, unsigned depth) {
        auto& state = interface_states_[&interface];
        if (state == Visit::Active)
            invalid(at, "cyclic WIT interface use involving '" + interface.name.text + "'");
        if (state == Visit::Done)
            return;
        if (depth == max_dependency_depth)
            invalid(at, "WIT interface use chain is too deep");
        state = Visit::Active;
        for (const auto& use : interface.uses) {
            const auto target = interface_target(use.path);
            if (target.interface)
                check_interface_dependencies(*target.interface, use.path.name.location, depth + 1);
        }
        state = Visit::Done;
    }

    void check_scope(Scope& scope, const std::vector<WitTypeDef>& types,
                     const std::vector<WitUse>& uses) {
        for (const auto& type : types)
            resolve_binding(scope, type.name, 0);
        for (const auto& use : uses) {
            interface_target(use.path); // Also verify uses with no imported names.
            for (const auto& item : use.items)
                resolve_binding(scope, item.alias ? *item.alias : item.name, 0);
        }
        // A method can refer to its own resource. Its signature is checked
        // after all type declarations have been resolved.
        for (const auto& type : types)
            if (type.kind == WitTypeDefKind::Resource)
                check_resource(scope, type);
    }

    void check_interface(const WitInterface& interface) {
        auto& scope = interface_scopes_.at(&interface);
        check_scope(scope, interface.types, interface.uses);
        for (const auto& function : interface.functions)
            check_function(scope, function, false);
    }

    TypeInfo resolve_binding(Scope& scope, const WitName& name, unsigned depth) {
        check_name(name);
        const auto it = scope.types.find(name.text);
        if (it == scope.types.end())
            invalid(name.location, "unknown WIT type '" + name.text + "'");
        auto& binding = it->second;
        if (binding.state == Visit::Active)
            invalid(name.location, "cyclic WIT type definition involving '" + name.text + "'");
        if (binding.state == Visit::Done)
            return binding.info;
        if (depth == max_dependency_depth)
            invalid(name.location, "WIT type dependency chain is too deep");

        binding.state = Visit::Active;
        if (binding.definition) {
            binding.info = check_definition(scope, *binding.definition, depth + 1);
        } else {
            const auto target = interface_target(binding.use->path);
            binding.info = target.foreign() ? TypeInfo{false, false, true} :
                resolve_binding(interface_scopes_.at(target.interface), binding.import->name, depth + 1);
        }
        binding.state = Visit::Done;
        return binding.info;
    }

    TypeInfo check_type(Scope& scope, const WitType& type, unsigned depth = 0) {
        if (depth == max_dependency_depth)
            invalid(type.location, "WIT type nesting is too deep");
        const auto count = type.arguments.size();
        const auto shape = [&](bool name, bool arguments) {
            if (type.name.has_value() != name || !arguments)
                invalid(type.location, "invalid WIT type arguments");
        };
        switch (type.kind) {
        case WitTypeKind::Scalar:
            shape(false, count == 0);
            for (const auto& scalar : scalars)
                if (scalar.kind == type.scalar)
                    return {};
            invalid(type.location, "invalid WIT scalar type");
        case WitTypeKind::String:
            shape(false, count == 0);
            return {};
        case WitTypeKind::Named:
            shape(true, count == 0);
            return resolve_binding(scope, *type.name, depth + 1);
        case WitTypeKind::Own:
        case WitTypeKind::Borrow: {
            shape(true, count == 0);
            const auto target = resolve_binding(scope, *type.name, depth + 1);
            if (!target.resource && !target.unknown)
                invalid(type.name->location, "WIT handle target must be a resource");
            return {false, type.kind == WitTypeKind::Borrow};
        }
        case WitTypeKind::List:
        case WitTypeKind::Option:
            shape(false, count == 1);
            break;
        case WitTypeKind::Tuple:
            shape(false, count != 0);
            break;
        case WitTypeKind::Result:
            shape(false, count <= 2);
            break;
        case WitTypeKind::Unit:
            invalid(type.location, "WIT '_' is only allowed as the first argument of result<_, E>");
        default:
            invalid(type.location, "invalid WIT type kind");
        }

        TypeInfo info;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& argument = type.arguments[i];
            if (type.kind == WitTypeKind::Result && i == 0 && count == 2 &&
                argument.kind == WitTypeKind::Unit) {
                if (argument.name || !argument.arguments.empty())
                    invalid(argument.location, "invalid WIT '_' type");
                continue;
            }
            info.borrowed |= check_type(scope, argument, depth + 1).borrowed;
        }
        return info;
    }

    TypeInfo check_definition(Scope& scope, const WitTypeDef& type, unsigned depth) {
        if ((type.kind == WitTypeDefKind::Alias) != type.alias.has_value() ||
            (type.kind != WitTypeDefKind::Record && !type.fields.empty()) ||
            (type.kind != WitTypeDefKind::Variant && type.kind != WitTypeDefKind::Enum &&
             type.kind != WitTypeDefKind::Flags && !type.cases.empty()) ||
            (type.kind != WitTypeDefKind::Resource && !type.functions.empty()))
            invalid(type.name.location, "invalid WIT type declaration payload");

        std::set<std::string> names;
        TypeInfo info;
        switch (type.kind) {
        case WitTypeDefKind::Alias:
            return check_type(scope, *type.alias, depth);
        case WitTypeDefKind::Record:
            for (const auto& field : type.fields) {
                unique(names, field.name, "record field");
                check_annotations(field.annotations);
                info.borrowed |= check_type(scope, field.type, depth).borrowed;
            }
            return info;
        case WitTypeDefKind::Enum:
        case WitTypeDefKind::Variant:
            if (type.cases.empty())
                invalid(type.name.location, "WIT variants and enums must have at least one case");
            [[fallthrough]];
        case WitTypeDefKind::Flags:
            for (const auto& item : type.cases) {
                unique(names, item.name, "case");
                check_annotations(item.annotations);
                if (const auto& payload = item.payload) {
                    if (type.kind != WitTypeDefKind::Variant)
                        invalid(payload->location, "only a WIT variant case can have a payload");
                    info.borrowed |= check_type(scope, *payload, depth).borrowed;
                }
            }
            return info;
        case WitTypeDefKind::Resource:
            return {true};
        }
        invalid(type.name.location, "invalid WIT type declaration kind");
    }

    void check_function(Scope& scope, const WitFunction& function, bool resource) {
        check_name(function.name);
        check_annotations(function.annotations);
        switch (function.kind) {
        case WitFunctionKind::Freestanding:
            if (resource)
                invalid(function.name.location, "invalid freestanding function in a WIT resource");
            break;
        case WitFunctionKind::Constructor:
            if (function.name.text != "constructor" || function.result)
                invalid(function.name.location, "invalid WIT constructor");
            [[fallthrough]];
        case WitFunctionKind::Method:
        case WitFunctionKind::Static:
            if (!resource)
                invalid(function.name.location, "WIT resource member outside a resource");
            break;
        default:
            invalid(function.name.location, "invalid WIT function kind");
        }

        std::set<std::string> names;
        if (function.kind == WitFunctionKind::Method)
            names.insert("self"); // Implicit borrow of the resource.
        for (const auto& param : function.parameters) {
            unique(names, param.name, "parameter");
            check_annotations(param.annotations);
            check_type(scope, param.type);
        }
        if (function.result && check_type(scope, *function.result).borrowed)
            invalid(function.result->location, "a WIT function cannot return a borrowed handle");
    }

    void check_resource(Scope& scope, const WitTypeDef& type) {
        std::set<std::string> names;
        bool has_constructor = false;
        for (const auto& member : type.functions) {
            if (member.kind == WitFunctionKind::Constructor) {
                if (has_constructor)
                    invalid(member.name.location, "duplicate WIT resource constructor");
                has_constructor = true;
            } else {
                unique(names, member.name, "resource member");
                if (key(member.name.text) == key(type.name.text))
                    invalid(member.name.location, "a WIT resource member cannot share its resource's name");
            }
            check_function(scope, member, true);
        }
    }

    const WorldInfo& check_world(const WitWorld& world, WitLocation at, unsigned depth) {
        auto& info = world_infos_[&world];
        if (info.state == Visit::Active)
            invalid(at, "cyclic WIT world include involving '" + world.name.text + "'");
        if (info.state == Visit::Done)
            return info;
        if (depth == max_dependency_depth)
            invalid(at, "WIT world include chain is too deep");
        info.state = Visit::Active;
        // Types declared in a WIT world are imports.
        for (const auto& type : world_scopes_.at(&world).types)
            info.imports.named.emplace(key(type.first), type.first);
        for (const auto& item : world.items) {
            auto& names = item.is_import ? info.imports : info.exports;
            if (item.path) {
                const auto target = interface_target(*item.path);
                if (!names.paths.emplace(strong_path_key(target.id), target.id).second)
                    invalid(item.path->name.location, "duplicate WIT world interface path");
            } else {
                const auto& name = item.function ? item.function->name : item.interface->name;
                check_name(name);
                if (!names.named.emplace(key(name.text), name.text).second)
                    invalid(name.location, std::string("duplicate world ") +
                            (item.is_import ? "import" : "export") + " name '" + name.text + "'");
            }
        }

        for (const auto& include : world.includes) {
            check_annotations(include.annotations);
            const auto target = resolve_path(include.path, 0);
            if (target.interface)
                invalid(include.path.name.location, "expected a WIT world, found an interface");
            check_foreign_kind(include.path, target, false);
            std::map<std::string, const WitName*> renames;
            for (const auto& rename : include.renames) {
                check_name(rename.name);
                if (!rename.alias)
                    invalid(rename.name.location, "WIT include rename requires an alias");
                check_name(*rename.alias);
                if (!renames.emplace(rename.name.text, &*rename.alias).second)
                    invalid(rename.name.location, "duplicate WIT include rename '" + rename.name.text + "'");
            }
            if (target.foreign()) {
                info.foreign_include = true;
                continue;
            }
            const auto& base = check_world(*target.world, include.path.name.location, depth + 1);
            info.foreign_include |= base.foreign_include;
            const auto has_name = [](const WorldNames& names, const std::string& name) {
                const auto it = names.named.find(key(name));
                return it != names.named.end() && it->second == name;
            };
            for (const auto& rename : include.renames) {
                if (!base.foreign_include && !has_name(base.imports, rename.name.text) &&
                    !has_name(base.exports, rename.name.text))
                    invalid(rename.name.location, "unknown named WIT include item '" + rename.name.text + "'");
            }
            const auto merge = [&](WorldNames& to, const WorldNames& from) {
                for (const auto& [strong_name, name] : from.named) {
                    const auto rename = renames.find(name);
                    const auto* alias = rename != renames.end() ? rename->second : nullptr;
                    if (!to.named.emplace(alias ? key(alias->text) : strong_name,
                                          alias ? alias->text : name).second)
                        invalid(alias ? alias->location : include.path.name.location,
                                "duplicate named item in WIT world include: '" + (alias ? alias->text : name) + "'");
                }
                for (const auto& [strong_path, path] : from.paths) {
                    const auto [entry, inserted] = to.paths.emplace(strong_path, path);
                    if (!inserted && entry->second != path)
                        invalid(include.path.name.location, "conflicting interface paths in WIT world include");
                }
            };
            merge(info.imports, base.imports);
            merge(info.exports, base.exports);
        }
        info.state = Visit::Done;
        return info;
    }
};

} // namespace

WitDocument parse_wit(std::string_view source) {
    auto document = Parser(source).parse();
    validate_wit(document);
    return document;
}

void validate_wit(const WitDocument& document) {
    Validator(document).run();
}

} // namespace limestone::wasmutils
