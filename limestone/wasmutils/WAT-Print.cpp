#include "WAT-Print.hpp"
#include "WASM-Validate.hpp"

#include <iomanip>
#include <locale>
#include <sstream>

namespace limestone::wasmutils::core {
namespace {

std::string_view type_name(Type type) {
    switch (type) {
    case Type::i32: return "i32";
    case Type::i64: return "i64";
    case Type::f32: return "f32";
    case Type::f64: return "f64";
    case Type::v128: return "v128";
    case Type::funcref: return "funcref";
    case Type::externref: return "externref";
    }
    throw Error(0, "unsupported value type");
}

std::string_view kind_name(ExternalKind kind) {
    switch (kind) {
    case ExternalKind::function: return "func";
    case ExternalKind::table: return "table";
    case ExternalKind::memory: return "memory";
    case ExternalKind::global: return "global";
    }
    throw Error(0, "unsupported external kind");
}

class Printer {
    std::ostringstream out_;

    void quoted(std::string_view bytes) {
        constexpr std::string_view hex = "0123456789abcdef";
        out_ << '"';
        for (const char ch : bytes) {
            const auto c = static_cast<unsigned char>(ch);
            if (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\')
                out_ << ch;
            else
                out_ << '\\' << hex[c >> 4] << hex[c & 15];
        }
        out_ << '"';
    }

    void types(std::string_view label, std::span<const Type> values) {
        if (!values.empty()) {
            out_ << " (" << label;
            for (auto type : values)
                out_ << ' ' << type_name(type);
            out_ << ')';
        }
    }

    void limits(const Limits& value) {
        out_ << value.min;
        if (value.max)
            out_ << ' ' << *value.max;
    }

    void table(const TableType& value) {
        limits(value.limits);
        out_ << ' ' << type_name(value.element);
    }

    void global(const GlobalType& value) {
        if (value.mutable_)
            out_ << "(mut " << type_name(value.type) << ')';
        else
            out_ << type_name(value.type);
    }

    void float_bits(std::uint64_t bits, unsigned fraction, unsigned exponent) {
        const auto fraction_mask = (std::uint64_t{1} << fraction) - 1;
        const auto exponent_mask = (1U << exponent) - 1;
        const auto frac = bits & fraction_mask;
        const auto exp = static_cast<unsigned>(bits >> fraction) & exponent_mask;
        if ((bits >> (fraction + exponent)) != 0)
            out_ << '-';
        if (exp == exponent_mask) {
            if (frac == 0)
                out_ << "inf";
            else
                out_ << "nan:0x" << std::hex << frac << std::dec;
        } else {
            // Express the exact integer significand times a power of two.
            // This includes zero/subnormals and does not depend on host FP.
            const auto significand = frac | (exp ? std::uint64_t{1} << fraction : 0);
            const auto bias = static_cast<int>(exponent_mask >> 1);
            out_ << "0x" << std::hex << significand << std::dec << 'p'
                 << static_cast<int>(exp ? exp : 1U) - bias - static_cast<int>(fraction);
        }
    }

    void memory(MemoryArg value) {
        out_ << " offset=" << value.offset << " align=" << (std::uint64_t{1} << value.align);
    }

    void instruction(const Instruction& insn) {
        const auto& info = *lookup(insn.opcode);
        out_ << info.mnemonic;
        const auto& imm = insn.immediate;
        switch (info.format) {
        case Format::none: case Format::zero: case Format::zero_zero:
            break;
        case Format::block: {
            const auto& type = std::get<BlockType>(imm).value;
            if (const auto* index = std::get_if<std::uint32_t>(&type))
                out_ << " (type " << *index << ')';
            else if (const auto* result = std::get_if<Type>(&type))
                types("result", std::span{result, 1U});
            break;
        }
        case Format::index: case Format::index_zero:
            out_ << ' ' << std::get<std::uint32_t>(imm);
            break;
        case Format::index_pair: {
            const auto pair = std::get<IndexPair>(imm);
            if (insn.opcode.byte == 0x11) {
                out_ << ' ' << pair.second << " (type " << pair.first << ')';
            } else if (insn.opcode.extension == 12) {
                out_ << ' ' << pair.second << ' ' << pair.first;
            } else {
                out_ << ' ' << pair.first << ' ' << pair.second;
            }
            break;
        }
        case Format::branch_table: {
            const auto& bt = std::get<BranchTable>(imm);
            for (const auto index : bt.labels)
                out_ << ' ' << index;
            out_ << ' ' << bt.otherwise;
            break;
        }
        case Format::types:
            types("result", std::get<std::vector<Type>>(imm));
            break;
        case Format::ref_type:
            out_ << (std::get<Type>(imm) == Type::funcref ? " func" : " extern");
            break;
        case Format::memory:
            memory(std::get<MemoryArg>(imm));
            break;
        case Format::memory_lane: {
            const auto operand = std::get<MemoryLane>(imm);
            memory(operand.memory);
            out_ << ' ' << unsigned(operand.lane);
            break;
        }
        case Format::lane:
            out_ << ' ' << unsigned(std::get<std::uint8_t>(imm));
            break;
        case Format::i32:
            out_ << " 0x" << std::hex << std::get<std::uint32_t>(imm) << std::dec;
            break;
        case Format::i64:
            out_ << " 0x" << std::hex << std::get<std::uint64_t>(imm) << std::dec;
            break;
        case Format::f32:
            out_ << ' ';
            float_bits(std::get<std::uint32_t>(imm), 23, 8);
            break;
        case Format::f64:
            out_ << ' ';
            float_bits(std::get<std::uint64_t>(imm), 52, 11);
            break;
        case Format::vector_bytes:
            if (insn.opcode.extension == 12)
                out_ << " i8x16";
            for (const auto byte : std::get<VectorBytes>(imm))
                out_ << ' ' << unsigned(byte);
            break;
        }
    }

    void initializer(const Program& program) {
        for (std::size_t i = 0; i + 1 < program.size(); ++i) {
            out_ << " (";
            instruction(program[i]);
            out_ << ')';
        }
    }

public:
    std::string print(const Module& module) {
        out_.imbue(std::locale::classic());
        out_ << "(module\n";
        for (const auto& type : module.types) {
            out_ << "  (type (func";
            types("param", type.params);
            types("result", type.results);
            out_ << "))\n";
        }
        for (const auto& import : module.imports) {
            out_ << "  (import ";
            quoted(import.module);
            out_ << ' ';
            quoted(import.name);
            out_ << " (" << kind_name(static_cast<ExternalKind>(import.desc.index())) << ' ';
            switch (import.desc.index()) {
            case 0: out_ << "(type " << std::get<std::uint32_t>(import.desc) << ')'; break;
            case 1: table(std::get<TableType>(import.desc)); break;
            case 2: limits(std::get<MemoryType>(import.desc).limits); break;
            case 3: global(std::get<GlobalType>(import.desc)); break;
            default: throw Error(0, "unsupported import descriptor");
            }
            out_ << "))\n";
        }
        for (const auto& func : module.functions) {
            out_ << "  (func (type " << func.type << ')';
            // A tiny binary can declare billions of locals via a single run.
            // Bound the text expansion instead of allocating gigabytes.
            std::size_t remaining_locals = 1'000'000;
            for (const auto& run : func.body.locals) {
                if (!run.count)
                    continue;
                if (run.count > remaining_locals)
                    throw Error(func.body.offset, "too many locals to expand as WAT");
                remaining_locals -= run.count;
                out_ << " (local";
                for (std::uint32_t i = 0; i < run.count; ++i)
                    out_ << ' ' << type_name(run.type);
                out_ << ')';
            }
            const auto& body = func.body.instructions;
            for (std::size_t i = 0; i + 1 < body.size(); ++i) {
                out_ << "\n    ";
                instruction(body[i]);
            }
            out_ << ")\n";
        }
        for (const auto& value : module.tables) {
            out_ << "  (table ";
            table(value);
            out_ << ")\n";
        }
        for (const auto& value : module.memories) {
            out_ << "  (memory ";
            limits(value.limits);
            out_ << ")\n";
        }
        for (const auto& value : module.globals) {
            out_ << "  (global ";
            global(value.type);
            initializer(value.init);
            out_ << ")\n";
        }
        for (const auto& value : module.exports) {
            out_ << "  (export ";
            quoted(value.name);
            out_ << " (" << kind_name(value.kind) << ' ' << value.index << "))\n";
        }
        if (module.start)
            out_ << "  (start " << *module.start << ")\n";
        for (const auto& elem : module.elements) {
            out_ << "  (elem";
            if (elem.mode == SegmentMode::active) {
                out_ << " (table " << elem.table << ") (offset";
                initializer(elem.offset);
                out_ << ')';
            } else if (elem.mode == SegmentMode::declarative) {
                out_ << " declare";
            }
            out_ << ' ' << type_name(elem.type);
            for (const auto& value : elem.init) {
                out_ << " (item";
                initializer(value);
                out_ << ')';
            }
            out_ << ")\n";
        }
        for (const auto& data : module.data) {
            out_ << "  (data";
            if (data.mode == SegmentMode::active) {
                out_ << " (memory " << data.memory << ") (offset";
                initializer(data.offset);
                out_ << ')';
            }
            out_ << ' ';
            quoted(data.bytes.empty() ? std::string_view{} : std::string_view{
                reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size()});
            out_ << ")\n";
        }
        out_ << ")\n";
        return out_.str();
    }
};

} // namespace

std::string print_module(const Module& module) {
    validate_module(module);
    return Printer{}.print(module);
}

} // namespace limestone::wasmutils::core
