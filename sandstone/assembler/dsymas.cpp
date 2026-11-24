// Built-in DSymspec profiles. Project metadata onto the existing lookup model
// so parsing, feature selection, Lua introspection and object layout are shared.
// Instruction bytes, however, come only from the generated recipes.
#include "intern.h"
#include "../decoder/spec_records.hpp"
#include "DSymspec.h"

#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>

#define SAND_PROFILE(name) extern "C" const dsymspec_target *sand_profile_##name(void);
#include "dsymas_profiles.inc"
#undef SAND_PROFILE

namespace {
using domsymas::detail::Value;

const dsymspec_target *find_profile(std::string_view name) {
#define SAND_PROFILE(profile) if (name == #profile) return sand_profile_##profile();
#include "dsymas_profiles.inc"
#undef SAND_PROFILE
    return nullptr;
}

Value metadata(const dsymspec_value &value) {
    switch (value.kind) {
    case DSYMSPEC_VALUE_BOOL: return Value::boolean_value(value.integer != 0);
    case DSYMSPEC_VALUE_INTEGER: return Value::integer_value(value.integer);
    case DSYMSPEC_VALUE_STRING: return Value::string_value({value.string, value.count});
    case DSYMSPEC_VALUE_LIST: {
        auto out = Value::array_value();
        for (size_t i = 0; i < value.count; ++i)
            out.array->push_back(metadata(value.fields[i].value));
        return out;
    }
    case DSYMSPEC_VALUE_RECORD: {
        auto out = Value::object_value();
        for (size_t i = 0; i < value.count; ++i)
            out.object->emplace_back(value.fields[i].name, metadata(value.fields[i].value));
        return out;
    }
    default: return Value::null(); // Not part of the assembler lookup schema.
    }
}

void default_field(Value &entry, const char *name, const char *value) {
    if (!entry.find(name))
        entry.object->emplace_back(name, Value::string_value(value));
}
}

int sand_dsymas_load(sandstone_asm_t *ctx, const char *profile) {
    try {
        auto target = find_profile(profile);
        if (!target) {
            sand_set_error(ctx, "unknown built-in target '%s'; use --target or --bank", profile);
            return -1;
        }
        const char *keys[] = {"targets", "features", "registers", "mnemonics",
                              "encoders", "forms", "syntaxes", "sections",
                              "linkages", "relocs"};
        auto doc = Value::object_value();
        doc.object->emplace_back("schema", Value::integer_value(1));
        for (const auto *key : keys)
            doc.object->emplace_back(key, Value::array_value());
        const std::string no_recipe = "<no DSymspec recipe>";
        std::vector<const dsymspec_encoder *> recipes;
        bool needs_placeholder = false;
        for (size_t i = 0; i < target->declaration_count; ++i) {
            const auto &decl = target->declarations[i];
            if (decl.kind == DSYMSPEC_DECL_DEBUG)
                continue; // Not used by the assembler lookup views.
            Value entry;
            if (decl.kind == DSYMSPEC_DECL_ENCODER) {
                // These records are lookup references, not legacy programs.
                entry = Value::object_value();
                entry.object->emplace_back("name", Value::string_value(decl.name));
            } else {
                entry = metadata(*decl.record);
            }
            // The generated descriptors preserve source fields as written.
            // Supply the defaults expected by the shared metadata loader.
            switch (decl.kind) {
            case DSYMSPEC_DECL_RELOC:
                default_field(entry, "addend", "explicit");
                default_field(entry, "overflow", "error");
                break;
            case DSYMSPEC_DECL_SECTION:
                default_field(entry, "content", "unknown");
                break;
            case DSYMSPEC_DECL_LINKAGE:
                default_field(entry, "visibility", "default");
                default_field(entry, "symtype", "none");
                break;
            case DSYMSPEC_DECL_FORM:
                if (const auto *operands = entry.find("operands"))
                    for (auto &operand : *operands->array)
                        default_field(operand, "access", "read");
                break;
            default:
                break;
            }
            if (decl.kind == DSYMSPEC_DECL_FORM) {
                recipes.push_back(decl.encoder);
                if (!entry.find("encoder")) {
                    entry.object->emplace_back("encoder", Value::string_value(no_recipe));
                    needs_placeholder = true;
                }
            }
            (*doc.object)[1 + decl.kind].second.array->push_back(std::move(entry));
        }
        if (needs_placeholder) {
            auto entry = Value::object_value();
            entry.object->emplace_back("name", Value::string_value(no_recipe));
            doc.find("encoders")->array->push_back(std::move(entry));
        }
        domsymas::Builder builder;
        auto result = domsymas::detail::build_spec_records(doc, builder);
        if (!result.ok) {
            sand_set_error(ctx, "cannot load built-in target '%s': %s", profile, result.error.c_str());
            return -1;
        }
        ctx->bank = domsymas::detail::make_metadata_bank(builder);
        // Keep the backend selected even for a metadata-only profile.
        ctx->dsymas_forms = static_cast<const dsymspec_encoder **>(
            sand_realloc_array(nullptr, recipes.empty() ? 1 : recipes.size(), sizeof(*ctx->dsymas_forms)));
        if (!ctx->dsymas_forms) throw std::bad_alloc();
        for (size_t i = 0; i < recipes.size(); ++i)
            ctx->dsymas_forms[i] = recipes[i];
        return 0;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "cannot load built-in target '%s': %s", profile, e.what());
        return -1;
    }
}

DomSymAs_Error sand_dsymas_encode(const sandstone_asm_t *ctx, uint32_t form,
    const DomSymAs_OpVal *ops, uint32_t nops, DomSymAs_Bytes *bytes,
    DomSymAs_RelocOut **relocs, uint32_t *nrelocs) {
    if (form >= domsymas_form_count(ctx->bank)) return DOMSYMAS_ERR_BAD_FORM_REF;
    const auto *recipe = ctx->dsymas_forms[form];
    if (!recipe || recipe->unsupported_reason) return DOMSYMAS_ERR_ENCODE;
    try {
        std::vector<dsymspec_binding> bindings;
        bindings.reserve(nops);
        // DSymspec uses the first binding for a name. Preserve the bank
        // encoder's last-binding-wins behavior for shared aliases like imm.
        for (uint32_t i = nops; i > 0; --i) {
            const auto &op = ops[i - 1];
            bindings.push_back({op.name, op.ival, op.is_symbol ? (op.sval ? op.sval : "") : nullptr});
        }
        std::vector<dsymspec_relocation> fixups(recipe->max_relocs);
        std::unique_ptr<uint8_t, decltype(&std::free)> data(
            static_cast<uint8_t *>(std::malloc(recipe->max_bytes)), &std::free);
        if (!data && recipe->max_bytes) return DOMSYMAS_ERR_NOMEM;
        size_t len, count;
        if (dsymspec_encode(recipe, bindings.data(), bindings.size(), data.get(), recipe->max_bytes,
                            &len, fixups.data(), fixups.size(), &count) != DSYMSPEC_ENCODE_OK)
            return DOMSYMAS_ERR_ENCODE;
        DomSymAs_RelocOut *out = nullptr;
        if (count) {
            out = static_cast<DomSymAs_RelocOut *>(std::calloc(count, sizeof(*out)));
            if (!out) return DOMSYMAS_ERR_NOMEM;
        }
        for (size_t i = 0; i < count; ++i) {
            const auto &fixup = fixups[i];
            const auto *rec = fixup.type_name ? domsymas_find_reloc(ctx->bank, fixup.type_name)
                                              : domsymas_reloc_at(ctx->bank, fixup.type_id);
            if (!rec) {
                domsymas_free_relocs(out, static_cast<uint32_t>(i));
                return DOMSYMAS_ERR_BAD_RELOC;
            }
            // DSymspec uses ELF's field-start convention. SandAS's existing
            // AMD64 PC32 output model uses the end of the displacement.
            std::string_view name(rec->name);
            int64_t addend = fixup.addend;
            if (name == "R_X86_64_PC32" || name == "R_X86_64_PLT32" ||
                name == "R_X86_64_GOTPCREL") {
                if (addend > std::numeric_limits<int64_t>::max() - 4) {
                    domsymas_free_relocs(out, static_cast<uint32_t>(i));
                    return DOMSYMAS_ERR_BAD_RELOC;
                }
                addend += 4;
            }
            char *symbol = sand_strdup(fixup.symbol);
            if (!symbol) {
                domsymas_free_relocs(out, static_cast<uint32_t>(i));
                return DOMSYMAS_ERR_NOMEM;
            }
            out[i] = {rec->id, static_cast<uint32_t>(fixup.offset), addend, symbol};
        }
        *bytes = {data.release(), len};
        *relocs = out;
        *nrelocs = static_cast<uint32_t>(count);
        return DOMSYMAS_OK;
    } catch (const std::bad_alloc &) {
        return DOMSYMAS_ERR_NOMEM;
    }
}
