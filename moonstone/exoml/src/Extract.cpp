#include "CompileOptions.hpp"
#include "ExtractCache.hpp"
#include <exception>
#include <exoml/Extract.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

namespace moonstone::exoml::cdecl {
namespace {
using detail::sha256;
std::string text(CXString s) {
    struct Release {
        CXString value;
        ~Release() { clang_disposeString(value); }
    } release{s};
    const char *p = clang_getCString(s);
    return p ? p : "";
}
std::string path(const std::string &s) {
    if (s.empty() || s.find('\0') != std::string::npos)
        throw std::invalid_argument("empty path or NUL in path");
    return std::filesystem::canonical(s).string();
}
SourceSpan location(CXSourceRange range) {
    SourceSpan s;
    CXFile first = nullptr, last = nullptr;
    unsigned begin = 0, end = 0;
    clang_getExpansionLocation(clang_getRangeStart(range), &first, &s.line, &s.column, &begin);
    clang_getExpansionLocation(clang_getRangeEnd(range), &last, &s.end_line, &s.end_column, &end);
    if (first)
        s.file = text(clang_getFileName(first));
    return s;
}
std::vector<CXCursor> children(CXCursor parent, std::size_t limit) {
    struct Context {
        std::vector<CXCursor> nodes;
        std::size_t limit;
        std::exception_ptr error;
    } context{{}, limit, {}};
    clang_visitChildren(
        parent,
        [](CXCursor c, CXCursor, CXClientData data) noexcept {
            auto &ctx = *static_cast<Context *>(data);
            try {
                if (ctx.nodes.size() >= ctx.limit)
                    throw std::length_error("AST child budget exceeded");
                ctx.nodes.push_back(c);
                return CXChildVisit_Continue;
            } catch (...) {
                ctx.error = std::current_exception();
                return CXChildVisit_Break;
            }
        },
        &context);
    if (context.error)
        std::rethrow_exception(context.error);
    return context.nodes;
}
void clean(std::string_view s) {
    if (s.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos)
        throw std::invalid_argument("NUL/newline in extraction option");
}
std::optional<std::uint64_t> measured(long long n) {
    return n < 0 ? std::nullopt : std::optional<std::uint64_t>(static_cast<std::uint64_t>(n));
}
struct Session {
    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;
    ~Session() {
        if (tu)
            clang_disposeTranslationUnit(tu);
        if (index)
            clang_disposeIndex(index);
    }
};
std::vector<CXFile> included_files(CXTranslationUnit tu, const std::string &header,
                                   std::size_t limit) {
    struct Files {
        std::vector<CXFile> values;
        std::size_t limit;
        std::exception_ptr error;
    } files{{clang_getFile(tu, header.c_str())}, limit, {}};
    clang_getInclusions(
        tu,
        [](CXFile file, CXSourceLocation *, unsigned, CXClientData data) noexcept {
            auto &f = *static_cast<Files *>(data);
            if (f.error)
                return;
            try {
                if (f.values.size() >= f.limit)
                    throw std::length_error("included file budget exceeded");
                f.values.push_back(file);
            } catch (...) {
                f.error = std::current_exception();
            }
        },
        &files);
    if (files.error)
        std::rethrow_exception(files.error);
    return files.values;
}
void preprocess(TranslationUnit &out, const ExtractOptions &options, CXTranslationUnit tu,
                const std::vector<CXCursor> &cursors,
                const std::map<std::string, std::string> &inputs) {
    auto &p = out.preprocessing;
    std::size_t entries = 0, text_bytes = 0;
    auto charge = [&] {
        if (entries++ >= options.limits.entries)
            throw std::length_error("preprocessing entry budget exceeded");
    };
    auto tokens = [&](CXTranslationUnit unit, CXCursor c) {
        CXToken *native = nullptr;
        unsigned count = 0;
        clang_tokenize(unit, clang_getCursorExtent(c), &native, &count);
        struct Release {
            CXTranslationUnit tu;
            CXToken *tokens;
            unsigned count;
            ~Release() { clang_disposeTokens(tu, tokens, count); }
        } release{unit, native, count};
        std::vector<std::string> result;
        for (unsigned i = 0; i < count; ++i) {
            charge();
            auto s = text(clang_getTokenSpelling(unit, native[i]));
            if (s.size() >
                options.limits.input_bytes - std::min(text_bytes, options.limits.input_bytes))
                throw std::length_error("preprocessing text budget exceeded");
            text_bytes += s.size();
            result.push_back(std::move(s));
        }
        return result;
    };
    auto definition = [&](CXTranslationUnit unit, CXCursor c) {
        MacroDefinition d;
        d.name = text(clang_getCursorSpelling(c));
        d.source = location(clang_getCursorExtent(c));
        if (d.source.file.empty()) {
            CXString filename{};
            clang_getPresumedLocation(clang_getRangeStart(clang_getCursorExtent(c)), &filename,
                                      &d.source.line, &d.source.column);
            d.source.file = text(filename);
            CXString end_filename{};
            clang_getPresumedLocation(clang_getRangeEnd(clang_getCursorExtent(c)), &end_filename,
                                      &d.source.end_line, &d.source.end_column);
            (void)text(end_filename);
        }
        d.tokens = tokens(unit, c);
        d.function_like = clang_Cursor_isMacroFunctionLike(c);
        d.builtin = clang_Cursor_isMacroBuiltin(c);
        std::string identity = std::to_string(d.source.file.size()) + ':' + d.source.file + ':' +
                               std::to_string(d.source.line) + ':' +
                               std::to_string(d.source.column) + ':' + d.name;
        for (const auto &token : d.tokens)
            identity += ':' + std::to_string(token.size()) + ':' + token;
        d.id = "macro_" + sha256(identity);
        return d;
    };
    std::set<std::string> names, definitions;
    auto record = [&](CXTranslationUnit unit, CXCursor c) {
        auto d = definition(unit, c);
        auto id = d.id;
        names.insert(d.name);
        if (definitions.insert(id).second)
            p.definitions.push_back(std::move(d));
        return id;
    };
    for (auto c : cursors) {
        auto kind = clang_getCursorKind(c);
        if (kind == CXCursor_MacroDefinition) {
            charge();
            record(tu, c);
        }
        if (kind == CXCursor_MacroExpansion) {
            charge();
            MacroExpansion e;
            e.name = text(clang_getCursorSpelling(c));
            names.insert(e.name);
            e.source = location(clang_getCursorExtent(c));
            auto ref = clang_getCursorReferenced(c);
            if (clang_getCursorKind(ref) == CXCursor_MacroDefinition)
                e.definition_id = record(tu, ref);
            else
                e.unknown_reason =
                    "compiler did not expose a definition (dynamic builtin or restored macro)";
            p.expansions.push_back(std::move(e));
        }
        if (kind == CXCursor_InclusionDirective) {
            charge();
            auto included = clang_getIncludedFile(c);
            p.includes.push_back({included ? path(text(clang_getFileName(included))) : "",
                                  location(clang_getCursorExtent(c))});
        }
    }
    auto skipped = [&](CXTranslationUnit unit, std::size_t main_limit) {
        auto ranges = clang_getAllSkippedRanges(unit);
        struct Release {
            CXSourceRangeList *ranges;
            ~Release() {
                if (ranges)
                    clang_disposeSourceRangeList(ranges);
            }
        } release{ranges};
        std::vector<SourceSpan> result;
        if (!ranges)
            return result;
        for (unsigned i = 0; i < ranges->count; ++i) {
            auto range = ranges->ranges[i];
            CXFile file = nullptr;
            unsigned offset = 0;
            clang_getExpansionLocation(clang_getRangeStart(range), &file, nullptr, nullptr,
                                       &offset);
            if (file && path(text(clang_getFileName(file))) == out.header && offset >= main_limit)
                continue;
            charge();
            result.push_back(location(range));
        }
        return result;
    };
    std::size_t original_size = 0;
    auto original =
        clang_getFileContents(tu, clang_getFile(tu, out.header.c_str()), &original_size);
    if (!original)
        throw std::runtime_error("main-file buffer unavailable for macro-state snapshot");
    p.skipped_ranges = skipped(tu, original_size);
    for (auto name :
         {"__LINE__", "__FILE__", "__DATE__", "__TIME__", "__TIMESTAMP__", "__COUNTER__"})
        names.insert(name);
    for (const auto &a : out.arguments)
        if ((a.starts_with("-D") || a.starts_with("-U")) && a.size() > 2)
            names.insert(a.substr(2, a.find('=') == std::string::npos ? std::string::npos
                                                                      : a.find('=') - 2));
    if (std::any_of(out.diagnostics.begin(), out.diagnostics.end(), [](const auto &d) {
            return d.severity == Severity::Error || d.severity == Severity::Fatal;
        })) {
        for (const auto &name : names)
            p.final_macros.push_back(
                {name, "", "source errors prevent final macro snapshot", std::nullopt});
        return;
    }
    std::string prefix = "EXOML_MACRO_PROBE_" +
                         sha256(std::string_view(original, original_size)).substr(0, 16) + '_';
    while (std::any_of(names.begin(), names.end(),
                       [&](const auto &n) { return n.starts_with(prefix); }))
        prefix += '_';
    std::string source(original, original_size);
    source += '\n';
    std::map<std::string, std::size_t> markers, indices;
    for (const auto &name : names) {
        charge();
        bool safe = !name.empty() && !(name.front() >= '0' && name.front() <= '9') &&
                    std::all_of(name.begin(), name.end(), [](unsigned char c) {
                        return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_';
                    });
        if (!safe)
            throw std::invalid_argument("unsupported compiler macro identifier for snapshot");
        auto index = p.final_macros.size();
        p.final_macros.push_back({name, "", "", false});
        indices[name] = index;
        auto marker = prefix + std::to_string(index);
        markers[marker] = index;
        source += "#ifdef " + name + "\n#define " + marker + "\n#endif\n";
        if (source.size() > options.limits.input_bytes)
            throw std::length_error("macro snapshot source budget exceeded");
    }
    Session probe;
    if (!probe.index)
        throw std::runtime_error("cannot create compiler macro-snapshot index");
    std::vector<const char *> argv;
    for (const auto &a : out.arguments)
        argv.push_back(a.c_str());
    CXUnsavedFile unsaved{out.header.c_str(), source.data(),
                          static_cast<unsigned long>(source.size())};
    if (clang_parseTranslationUnit2(probe.index, out.header.c_str(), argv.data(), int(argv.size()),
                                    &unsaved, 1, CXTranslationUnit_DetailedPreprocessingRecord,
                                    &probe.tu) != CXError_Success ||
        !probe.tu)
        throw std::runtime_error("compiler macro-state snapshot parse failed");
    for (unsigned i = 0, n = clang_getNumDiagnostics(probe.tu); i < n; ++i) {
        auto d = clang_getDiagnostic(probe.tu, i);
        auto severity = clang_getDiagnosticSeverity(d);
        auto message = text(clang_getDiagnosticSpelling(d));
        clang_disposeDiagnostic(d);
        if (severity >= CXDiagnostic_Error)
            throw std::runtime_error("unsupported macro-state snapshot: " + message);
    }
    std::map<std::string, std::string> checked;
    for (auto file : included_files(probe.tu, out.header, options.limits.entries)) {
        if (!file)
            throw std::runtime_error("macro snapshot input unavailable");
        auto filename = path(text(clang_getFileName(file)));
        if (checked.contains(filename))
            continue;
        std::size_t size = 0;
        const char *data = clang_getFileContents(probe.tu, file, &size);
        if (!data)
            throw std::runtime_error("macro snapshot input buffer unavailable");
        if (filename == out.header) {
            size = original_size;
            data = original;
        }
        checked[filename] = sha256(std::string_view(data, size));
    }
    if (checked != inputs)
        throw std::runtime_error("included inputs changed during macro snapshot");
    auto span_key = [](const SourceSpan &s) {
        return s.file + ':' + std::to_string(s.line) + ':' + std::to_string(s.column) + ':' +
               std::to_string(s.end_line) + ':' + std::to_string(s.end_column);
    };
    auto probe_skipped = skipped(probe.tu, original_size);
    if (probe_skipped.size() != p.skipped_ranges.size() ||
        !std::equal(probe_skipped.begin(), probe_skipped.end(), p.skipped_ranges.begin(),
                    [&](const auto &a, const auto &b) { return span_key(a) == span_key(b); }))
        throw std::runtime_error("preprocessing conditions changed during macro snapshot");
    for (auto c : children(clang_getTranslationUnitCursor(probe.tu), options.limits.entries)) {
        CXFile file = nullptr;
        unsigned offset = 0;
        clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr, &offset);
        if (!file || path(text(clang_getFileName(file))) != out.header || offset < original_size)
            continue;
        auto name = text(clang_getCursorSpelling(c));
        if (clang_getCursorKind(c) == CXCursor_MacroDefinition && markers.contains(name))
            p.final_macros[markers.at(name)].defined = true;
        if (clang_getCursorKind(c) == CXCursor_MacroExpansion && indices.contains(name)) {
            auto ref = clang_getCursorReferenced(c);
            if (clang_getCursorKind(ref) == CXCursor_MacroDefinition)
                p.final_macros[indices.at(name)].definition_id = record(probe.tu, ref);
        }
    }
    for (auto &state : p.final_macros)
        if (state.defined == true && state.definition_id.empty())
            state.unknown_reason =
                "compiler confirms defined state but exposes no active definition ID";
    p.final_snapshot_complete = true;
}
class Extractor {
    TranslationUnit &out;
    const ExtractLimits &limits;
    CXTranslationUnit tu;
    std::string target_identity;
    std::size_t entries = 0;
    void charge() {
        if (entries++ >= limits.entries)
            throw std::length_error("extraction entry budget exceeded");
    }
    std::string key(CXType t) {
        return std::to_string(t.kind) + ":" + text(clang_getTypeSpelling(t)) + ":" +
               text(clang_getCursorUSR(clang_getTypeDeclaration(t))) + ":" +
               std::to_string(clang_isConstQualifiedType(t)) +
               std::to_string(clang_isVolatileQualifiedType(t)) +
               std::to_string(clang_isRestrictQualifiedType(t));
    }

  public:
    Extractor(TranslationUnit &unit, const ExtractLimits &budget, CXTranslationUnit translation)
        : out(unit), limits(budget), tu(translation) {
        target_identity = out.target.compiler + ':' + out.target.triple;
        for (const auto &arg : out.arguments)
            target_identity += ':' + std::to_string(arg.size()) + ':' + arg;
        target_identity = sha256(target_identity);
    }
    std::string type(CXType t, std::size_t depth = 0) {
        auto id = "t_" + sha256(target_identity + ":" + key(t));
        if (out.types.contains(id))
            return id;
        if (depth >= limits.depth || out.types.size() >= limits.types)
            throw std::length_error("type count/depth budget exceeded");
        auto &v = out.types[id]; // Publish before traversing recursive record pointers.
        charge();
        v.id = id;
        v.spelling = text(clang_getTypeSpelling(t));
        CXType canonical = clang_getCanonicalType(t);
        v.canonical_spelling = text(clang_getTypeSpelling(canonical));
        v.declaration_id = text(clang_getCursorUSR(clang_getTypeDeclaration(t)));
        v.qualifiers = {bool(clang_isConstQualifiedType(t)), bool(clang_isVolatileQualifiedType(t)),
                        bool(clang_isRestrictQualifiedType(t))};
        v.canonical_id = key(t) == key(canonical) ? id : type(canonical, depth + 1);
        // Clang reports extension sizes for void/functions; these are not C11 object layouts.
        if (canonical.kind != CXType_Void && canonical.kind != CXType_FunctionProto &&
            canonical.kind != CXType_FunctionNoProto && canonical.kind != CXType_Invalid) {
            v.size = measured(clang_Type_getSizeOf(t));
            v.alignment = measured(clang_Type_getAlignOf(t));
        }
        auto sub = [&](CXType child) { return type(child, depth + 1); };
        switch (t.kind) {
        case CXType_Elaborated:
            // Keep an edge, not a snapshot of a possibly still-building recursive record.
            v.kind = TypeKind::Elaborated;
            v.element = sub(clang_Type_getNamedType(t));
            break;
        case CXType_Typedef:
            v.kind = TypeKind::Typedef;
            v.element = sub(clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(t)));
            break;
        case CXType_Void:
            v.kind = TypeKind::Void;
            break;
        case CXType_Bool:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_UInt128:
            v.kind = TypeKind::Integer;
            v.is_signed = false;
            break;
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_Int128:
            v.kind = TypeKind::Integer;
            v.is_signed = true;
            break;
        case CXType_Float:
        case CXType_Double:
        case CXType_LongDouble:
            v.kind = TypeKind::Floating;
            break;
        case CXType_Pointer:
            v.kind = TypeKind::Pointer;
            v.element = sub(clang_getPointeeType(t));
            break;
        case CXType_ConstantArray:
        case CXType_IncompleteArray:
        case CXType_VariableArray:
        case CXType_DependentSizedArray:
            v.kind = TypeKind::Array;
            v.element = sub(clang_getArrayElementType(t));
            v.array_count = measured(clang_getArraySize(t));
            if (!v.array_count)
                v.unsupported_reason = "nonconstant array bound; explicit adapter required";
            break;
        case CXType_FunctionProto:
        case CXType_FunctionNoProto:
            v.kind = TypeKind::Function;
            v.prototype = t.kind == CXType_FunctionProto;
            v.variadic = clang_isFunctionTypeVariadic(t);
            v.calling_convention = clang_getFunctionTypeCallingConv(t);
            v.result = sub(clang_getResultType(t));
            for (int i = 0, n = clang_getNumArgTypes(t); i < n; ++i) {
                charge();
                v.arguments.push_back(sub(clang_getArgType(t, unsigned(i))));
            }
            if (!v.prototype || v.variadic)
                v.unsupported_reason =
                    "unprototyped/variadic function requires an explicit call-shape adapter";
            break;
        case CXType_Record: {
            v.kind = TypeKind::Record;
            auto decl = clang_getTypeDeclaration(t);
            auto definition = clang_getCursorDefinition(decl);
            if (!clang_Cursor_isNull(definition))
                decl = definition;
            v.is_union = clang_getCursorKind(decl) == CXCursor_UnionDecl;
            if (v.is_union)
                v.unsupported_reason = "union discovered; verified adapter required";
            if (!v.size)
                v.unsupported_reason = "incomplete record; opaque use only";
            for (auto child : children(decl, limits.entries)) {
                charge();
                auto kind = clang_getCursorKind(child);
                if (clang_isAttribute(kind))
                    v.attributes.push_back(text(clang_getCursorKindSpelling(kind)));
                bool anonymous = (kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) &&
                                 clang_Cursor_isAnonymousRecordDecl(child);
                if (kind != CXCursor_FieldDecl && !anonymous)
                    continue;
                Field f;
                f.name = anonymous ? "" : text(clang_getCursorSpelling(child));
                f.source = location(clang_getCursorExtent(child));
                f.type = sub(clang_getCursorType(child));
                if (!anonymous)
                    f.offset_bits = measured(clang_Cursor_getOffsetOfField(child));
                if (!anonymous && clang_Cursor_isBitField(child)) {
                    int width = clang_getFieldDeclBitWidth(child);
                    if (width >= 0)
                        f.bit_width = unsigned(width);
                    v.unsupported_reason = "bitfield record discovered; verified adapter required";
                }
                v.fields.push_back(std::move(f));
            }
            break;
        }
        case CXType_Enum: {
            v.kind = TypeKind::Enum;
            auto decl = clang_getTypeDeclaration(t);
            v.element = sub(clang_getEnumDeclIntegerType(decl));
            v.is_signed = out.types.at(v.element).is_signed;
            for (auto child : children(decl, limits.entries)) {
                charge();
                if (clang_getCursorKind(child) != CXCursor_EnumConstantDecl)
                    continue;
                v.enumerators.push_back(
                    {text(clang_getCursorSpelling(child)),
                     v.is_signed.value_or(true)
                         ? std::to_string(clang_getEnumConstantDeclValue(child))
                         : std::to_string(clang_getEnumConstantDeclUnsignedValue(child))});
            }
            break;
        }
        default:
            v.unsupported_reason = "unsupported Clang type " +
                                   text(clang_getTypeKindSpelling(t.kind)) +
                                   "; provide a supported typedef/manual adapter";
        }
        return id;
    }
    std::string tokens(CXCursor c) {
        CXToken *tokens = nullptr;
        unsigned count = 0;
        clang_tokenize(tu, clang_getCursorExtent(c), &tokens, &count);
        struct Release {
            CXTranslationUnit tu;
            CXToken *tokens;
            unsigned count;
            ~Release() { clang_disposeTokens(tu, tokens, count); }
        } release{tu, tokens, count};
        std::string s;
        for (unsigned i = 0; i < count; ++i) {
            charge();
            if (i)
                s += ' ';
            s += text(clang_getTokenSpelling(tu, tokens[i]));
            if (s.size() > limits.input_bytes)
                throw std::length_error("token text budget exceeded");
        }
        return s;
    }
    void declaration(CXCursor c) {
        if (out.declarations.size() >= limits.declarations)
            throw std::length_error("declaration budget exceeded");
        charge();
        Declaration d;
        d.id = text(clang_getCursorUSR(c));
        d.name = text(clang_getCursorSpelling(c));
        d.source = location(clang_getCursorExtent(c));
        if (d.id.empty())
            d.id = d.source.file + ":" + std::to_string(d.source.line) + ":" +
                   std::to_string(d.source.column) + ":" + d.name;
        d.definition = clang_isCursorDefinition(c);
        d.is_inline = clang_Cursor_isFunctionInlined(c);
        d.linkage = std::to_string(clang_getCursorLinkage(c));
        switch (clang_getCursorKind(c)) {
        case CXCursor_FunctionDecl:
            d.kind = DeclKind::Function;
            for (int i = 0, n = clang_Cursor_getNumArguments(c); i < n; ++i) {
                charge();
                auto p = clang_Cursor_getArgument(c, unsigned(i));
                d.parameters.push_back({text(clang_getCursorSpelling(p)),
                                        type(clang_getCursorType(p)),
                                        location(clang_getCursorExtent(p)), tokens(p)});
            }
            break;
        case CXCursor_TypedefDecl:
            d.kind = DeclKind::Typedef;
            break;
        case CXCursor_StructDecl:
        case CXCursor_UnionDecl:
            d.kind = DeclKind::Record;
            break;
        case CXCursor_EnumDecl:
            d.kind = DeclKind::Enum;
            break;
        case CXCursor_VarDecl:
            d.kind = DeclKind::Variable;
            break;
        case CXCursor_MacroDefinition:
            d.kind = DeclKind::Macro;
            d.function_like_macro = clang_Cursor_isMacroFunctionLike(c);
            d.tokens = tokens(c);
            d.unsupported_reason = "preprocessor definition, not a callable C ABI symbol";
            break;
        default:
            d.tokens = tokens(c);
            d.unsupported_reason = "unsupported declaration " +
                                   text(clang_getCursorKindSpelling(clang_getCursorKind(c))) +
                                   "; select supported symbols or provide a manual adapter";
        }
        if (d.kind != DeclKind::Macro && d.kind != DeclKind::Unsupported)
            d.type = type(clang_getCursorType(c));
        if (d.tokens.empty())
            d.tokens = tokens(c);
        out.declarations.push_back(std::move(d));
    }
};
} // namespace

TranslationUnit extract_c11(const ExtractOptions &o) {
    CacheReport cache;
    return extract_c11(o, cache);
}
TranslationUnit extract_c11(const ExtractOptions &o, CacheReport &report) {
    report = {};
    if (!o.cache_directory.empty()) {
        report.status = CacheStatus::Bypassed;
        report.reason = "source/graph validation did not complete";
    }
    TranslationUnit out;
    out.revision = o.revision;
    try {
        clean(o.revision);
        out.header = path(o.header);
        auto invocation = detail::compilation_options(o);
        out.compilation = invocation.context;
        out.arguments = {"-x",
                         "c",
                         "-std=c11",
                         "-fsyntax-only",
                         "-ferror-limit=20",
                         "-Xclang",
                         "-fno-pch-timestamp"};
        out.arguments.insert(out.arguments.end(), invocation.arguments.begin(),
                             invocation.arguments.end());
        Session session;
        if (!session.index)
            throw std::runtime_error("cannot create libclang index");
        std::vector<const char *> argv;
        for (const auto &a : out.arguments)
            argv.push_back(a.c_str());
        auto status = clang_parseTranslationUnit2(
            session.index, out.header.c_str(), argv.data(), int(argv.size()), nullptr, 0,
            CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_ForSerialization,
            &session.tu);
        if (status != CXError_Success || !session.tu)
            throw std::runtime_error("libclang parse failed: " + std::to_string(status));
        auto tu = session.tu;
        out.target.compiler = text(clang_getClangVersion());
        auto target = clang_getTranslationUnitTargetInfo(tu);
        if (!target)
            throw std::runtime_error("target information unavailable");
        out.target.triple = text(clang_TargetInfo_getTriple(target));
        int bits = clang_TargetInfo_getPointerWidth(target);
        clang_TargetInfo_dispose(target);
        if (bits <= 0)
            throw std::runtime_error("target pointer width unavailable");
        out.target.pointer_bits = unsigned(bits);
        for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i) {
            if (i >= o.limits.entries)
                throw std::length_error("diagnostic budget exceeded");
            auto native = clang_getDiagnostic(tu, i);
            Diagnostic d;
            d.id = "EXOML-C11-COMPILER";
            d.message = text(clang_getDiagnosticSpelling(native));
            auto severity = clang_getDiagnosticSeverity(native);
            d.severity = severity == CXDiagnostic_Fatal     ? Severity::Fatal
                         : severity == CXDiagnostic_Error   ? Severity::Error
                         : severity == CXDiagnostic_Warning ? Severity::Warning
                                                            : Severity::Note;
            auto loc = clang_getDiagnosticLocation(native);
            d.primary = location(clang_getRange(loc, loc));
            for (unsigned j = 0, count = clang_getDiagnosticNumRanges(native); j < count; ++j)
                d.related.push_back(location(clang_getDiagnosticRange(native, j)));
            clang_disposeDiagnostic(native);
            out.diagnostics.push_back(std::move(d));
        }
        auto cursors = children(clang_getTranslationUnitCursor(tu), o.limits.entries);
        for (auto c : cursors) {
            if (clang_getCursorKind(c) == CXCursor_MacroDefinition &&
                text(clang_getCursorSpelling(c)).starts_with("MULIB_IMPL_"))
                throw std::invalid_argument(
                    "active MULIB_IMPL_* definition forbidden in declaration scan");
        }
        // CXFile buffers are the compiler's parsed bytes, not a later reread.
        std::map<std::string, std::string> inputs;
        std::size_t bytes = 0;
        auto charge = [&](std::size_t n) {
            if (n > o.limits.input_bytes - std::min(bytes, o.limits.input_bytes))
                throw std::length_error("included input byte budget exceeded");
            bytes += n;
        };
        for (auto file : included_files(tu, out.header, o.limits.entries)) {
            if (!file)
                throw std::runtime_error("parsed input buffer unavailable");
            auto filename = path(text(clang_getFileName(file)));
            if (inputs.contains(filename))
                continue;
            std::size_t size = 0;
            const char *contents = clang_getFileContents(tu, file, &size);
            if (!contents)
                throw std::runtime_error("parsed input contents unavailable: " + filename);
            charge(size);
            inputs[filename] = sha256(std::string_view(contents, size));
        }
        auto compiler_inputs = inputs;
        if (invocation.database) {
            auto size = std::filesystem::file_size(invocation.database->path);
            charge(size);
            inputs[invocation.database->path] = invocation.database->sha256;
        }
        for (const auto &extra : o.provenance_files) {
            auto filename = path(extra);
            if (inputs.contains(filename))
                continue;
            auto size = std::filesystem::file_size(filename);
            charge(size);
            std::ifstream stream(filename, std::ios::binary);
            std::string contents(std::size_t(size), '\0');
            if (!stream.read(contents.data(), std::streamsize(size)))
                throw std::runtime_error("cannot read provenance file: " + filename);
            inputs[filename] = sha256(contents);
        }
        for (auto &[filename, hash] : inputs)
            out.inputs.push_back({filename, hash});
        try {
            preprocess(out, o, tu, cursors, compiler_inputs);
        } catch (const std::exception &e) {
            Diagnostic d;
            d.id = "EXOML-C11-PREPROCESS";
            d.message = e.what();
            out.diagnostics.push_back(std::move(d));
        }
        // A second compiler pass must not silently mix changed headers, sidecars
        // or compilation-database contents into one provenance record.
        for (const auto &input : out.inputs) {
            auto size = std::filesystem::file_size(input.path);
            if (size > o.limits.input_bytes)
                throw std::length_error("input changed beyond discovery byte budget");
            std::ifstream stream(input.path, std::ios::binary);
            std::string contents(std::size_t(size), '\0');
            if (!stream.read(contents.data(), std::streamsize(size)) ||
                stream.peek() != std::char_traits<char>::eof() || sha256(contents) != input.sha256)
                throw std::runtime_error("input changed during discovery: " + input.path);
        }
        out.complete =
            std::none_of(out.diagnostics.begin(), out.diagnostics.end(), [](const auto &d) {
                return d.severity == Severity::Error || d.severity == Severity::Fatal;
            });
        std::unique_ptr<detail::NormalizationCache> cache;
        if (!o.cache_directory.empty() && out.complete) {
            try {
                report.reason.clear();
                cache = std::make_unique<detail::NormalizationCache>(o, report, out, tu);
                if (auto hit = cache->load())
                    return std::move(*hit);
            } catch (const std::exception &e) {
                report.status = CacheStatus::Error;
                report.reason = e.what();
                throw;
            }
        }
        Extractor extractor(out, o.limits, tu);
        for (auto c : cursors) {
            CXFile file = nullptr;
            clang_getExpansionLocation(clang_getCursorLocation(c), &file, nullptr, nullptr,
                                       nullptr);
            if (!file || path(text(clang_getFileName(file))) != out.header)
                continue;
            if (clang_getCursorKind(c) == CXCursor_MacroExpansion ||
                clang_getCursorKind(c) == CXCursor_InclusionDirective)
                continue;
            extractor.declaration(c);
        }
        out.fingerprint = sha256(describe_translation_unit(out));
        if (cache) {
            try {
                cache->store(out);
            } catch (const std::exception &e) {
                report.status = CacheStatus::Error;
                report.reason = e.what();
                throw;
            }
        }
    } catch (const std::exception &e) {
        out.complete = false;
        out.fingerprint.clear();
        if (report.status != CacheStatus::Disabled && report.status != CacheStatus::Error) {
            report.status = CacheStatus::Bypassed;
            report.reason = e.what();
        }
        Diagnostic d;
        d.id = "EXOML-C11-EXTRACT";
        d.message = e.what();
        d.primary.file = out.header;
        out.diagnostics.push_back(std::move(d));
    }
    return out;
}
} // namespace moonstone::exoml::cdecl
