#include "qwasm/qwasm.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <new>
#include <type_traits>

namespace {

void quote(std::ostream& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\')
            out << '\\' << static_cast<char>(c);
        else if (c >= 0x20 && c <= 0x7e)
            out << static_cast<char>(c);
        else
            out << "\\x" << hex[c >> 4] << hex[c & 15];
    }
    out << '"';
}

void print_limits(const qwasm::TypeLimits& limits) {
    std::cout << (limits.address64 ? " i64" : " i32") << " min=" << limits.minimum;
    if (limits.maximum)
        std::cout << " max=" << *limits.maximum;
}

void print_import(const qwasm::Import& entry) {
    std::cout << "  @" << entry.offset << ' ';
    quote(std::cout, entry.module);
    std::cout << ' ';
    quote(std::cout, entry.name);
    std::cout << ' ';
    std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, qwasm::FunctionImport>) {
            std::cout << "function type=" << item.type_index;
        } else if constexpr (std::is_same_v<T, qwasm::TableImport>) {
            std::cout << "table " << qwasm::to_string(item.element_type);
            print_limits(item.limits);
        } else if constexpr (std::is_same_v<T, qwasm::MemoryImport>) {
            std::cout << "memory";
            print_limits(item.limits);
            if (item.shared)
                std::cout << " shared";
        } else if constexpr (std::is_same_v<T, qwasm::GlobalImport>) {
            std::cout << "global " << qwasm::to_string(item.value_type)
                      << (item.is_mutable ? " mutable" : " immutable");
        } else if constexpr (std::is_same_v<T, qwasm::TagImport>) {
            std::cout << "tag type=" << item.type_index;
        }
    }, entry.descriptor);
    std::cout << '\n';
}

void print_module(const qwasm::Module& module) {
    std::cout << "WebAssembly core binary version " << module.version << '\n';
    std::cout << "Sections (" << module.sections.size() << "):\n";
    for (const auto& section : module.sections) {
        std::cout << "  @" << section.offset << ' ' << qwasm::to_string(section.id)
                  << " payload @" << section.payload.offset << " size=" << section.payload.size;
        if (section.custom) {
            std::cout << ' ';
            quote(std::cout, section.custom->name);
            std::cout << " data @" << section.custom->data.offset
                      << " size=" << section.custom->data.size;
        }
        std::cout << '\n';
    }

    std::cout << "Imports (" << module.imports.size() << "):\n";
    for (const auto& entry : module.imports)
        print_import(entry);
    std::cout << "Exports (" << module.exports.size() << "):\n";
    for (const auto& entry : module.exports) {
        std::cout << "  @" << entry.offset << ' ';
        quote(std::cout, entry.name);
        std::cout << ' ' << qwasm::to_string(entry.kind) << " index=" << entry.index << '\n';
    }
    if (module.start_function)
        std::cout << "Start function index: " << *module.start_function << '\n';
    if (module.data_count)
        std::cout << "Data count: " << *module.data_count << '\n';
}

int report(std::string_view path, std::string_view message) {
    std::cerr << "qwasm: ";
    quote(std::cerr, path);
    std::cerr << ": " << message << '\n';
    return 1;
}

int report(std::string_view path, const qwasm::Error& err) {
    std::cerr << "qwasm: ";
    quote(std::cerr, path);
    std::cerr << ": " << qwasm::to_string(err.kind) << " at byte " << err.offset
              << ": " << err.message << '\n';
    return 1;
}

int inspect_file(const char* path) {
    const qwasm::InspectionLimits limits;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return report(path, "could not open input");
    std::array<char, 16 * 1024> buffer;
    std::vector<std::uint8_t> bytes;
    for (;;) {
        // At most one byte beyond the input cap is ever read.
        const auto room = limits.max_module_bytes - bytes.size();
        const auto requested = std::min(room + 1, buffer.size());
        file.read(buffer.data(), static_cast<std::streamsize>(requested));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (file.bad() || (file.fail() && !file.eof()))
            return report(path, "could not read input");
        if (count > room)
            return report(path, qwasm::Error{qwasm::ErrorKind::limit_exceeded,
                                            limits.max_module_bytes,
                                            "module exceeds byte limit"});
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + count);
        if (file.eof())
            break;
    }

    const auto result = qwasm::inspect(bytes, limits);
    if (const auto* err = std::get_if<qwasm::Error>(&result))
        return report(path, *err);
    print_module(std::get<qwasm::Module>(result));
    std::cout.flush();
    if (!std::cout)
        return report(path, "could not write summary");
    return 0;
}

void usage(std::ostream& out) {
    out << "Usage: qwasm [--] FILE\n"
           "Inspect core Wasm sections, imports and exports without executing code.\n"
           "This is a metadata summary, not full validation.\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view option = argc > 1 ? argv[1] : "";
    if (argc == 2 && (option == "--help" || option == "-h")) {
        usage(std::cout);
        return 0;
    }
    const char* path = nullptr;
    if (argc == 2 && !option.starts_with('-'))
        path = argv[1];
    else if (argc == 3 && option == "--")
        path = argv[2];
    if (!path) {
        usage(std::cerr);
        return 2;
    }

    try {
        return inspect_file(path);
    } catch (const std::bad_alloc&) {
        return report(path, "not enough memory");
    }
}
