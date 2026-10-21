#include "Flintstone.hpp"
#include "targets/SandstoneTargets.hpp"
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void usage() {
    std::cout << "usage: flintstone [options] [FILE|-]\n"
        "  --check                    Verify without printing\n"
        "  --lower core|machine       Perform verified dialect conversion\n"
        "  --scalarize                Eliminate vector/tuple aggregates (pass)\n"
        "  --dce                      Remove unused pure operations\n"
        "  --kernels DIR --kernel NAME Run a Blackstone SSA kernel (repeatable)\n"
        "  --run FUNCTION --arg VALUE Execute; arguments may be repeated\n"
        "  --budget N                 Execution instruction budget (default 1000000)\n"
        "  --emit FILE --backend NAME  Emit bytecode (default vm) or a tiny native backend\n"
        "  --target NAME              Retarget the module (a tiny native name)\n"
        "  --stdlib DIR | -I DIR      Add a library search directory (repeatable)\n"
        "  --list-dialects            List registered dialect names\n"
        "  --list-targets             List backends, tiny natives and stdspec architectures\n"
        "  --bindings                 List Tourchstone binding targets\n"
        "  --version | --help\n";
}
template<class T> T number(const std::string& text) {
    T result{}; auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::invalid_argument("invalid number: " + text);
    return result;
}
// A non-finite literal is a usage error, not a value the VM would reject later.
double real_number(const std::string& text) {
    const auto value = number<double>(text);
    if (!std::isfinite(value)) throw std::invalid_argument("floating argument must be finite: " + text);
    return value;
}
}
int main(int argc, char** argv) {
    try {
        std::string input = "-", run, output, backend = "vm", directory, target_name;
        std::vector<std::string> arguments, kernels, search_dirs;
        std::optional<flintstone::Level> level;
        bool check = false, dce = false, scalarize = false, input_seen = false;
        std::size_t budget = 1000000;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            auto value = [&]() -> std::string { if (++i == argc) throw std::invalid_argument("missing value for " + option); return argv[i]; };
            if (option == "--help" || option == "-h") { usage(); return 0; }
            if (option == "--version") { std::cout << flintstone::version() << '\n'; return 0; }
            if (option == "--bindings") { for (const auto& target : flintstone::binding_targets()) std::cout << target << '\n'; return 0; }
            if (option == "--list-dialects") {
                flintstone::Registry registry;
                for (const auto& name : registry.dialect_names()) std::cout << name << '\n';
                return 0;
            }
            if (option == "--list-targets") {
                flintstone::Registry registry;
                flintstone::register_tiny_backends(registry);
                for (const auto& name : registry.backend_names()) std::cout << "backend:" << name << '\n';
                for (const auto& arch : flintstone::all_architectures())
                    std::cout << "arch:" << arch.key << ':' << arch.arch << ':' << arch.abi << '\n';
                return 0;
            }
            if (option == "--check") check = true;
            else if (option == "--scalarize") scalarize = true;
            else if (option == "--dce") dce = true;
            else if (option == "--run") run = value();
            else if (option == "--arg") arguments.push_back(value());
            else if (option == "--budget") budget = number<std::size_t>(value());
            else if (option == "--emit") output = value();
            else if (option == "--backend") backend = value();
            else if (option == "--target") target_name = value();
            else if (option == "--stdlib" || option == "-I") search_dirs.push_back(value());
            else if (option == "--kernels") directory = value();
            else if (option == "--kernel") kernels.push_back(value());
            else if (option == "--lower") {
                auto name = value();
                if (name == "core") level = flintstone::Level::Core;
                else if (name == "machine") level = flintstone::Level::Machine;
                else throw std::invalid_argument("expected core or machine");
            } else if (option == "-" || option.empty() || option[0] != '-') {
                if (input_seen) throw std::invalid_argument("only one input file is accepted");
                input = option; input_seen = true;
            } else throw std::invalid_argument("unknown option " + option);
        }
        if (!kernels.empty() && directory.empty()) throw std::invalid_argument("--kernel requires --kernels DIR");
        if (!arguments.empty() && run.empty()) throw std::invalid_argument("--arg requires --run");
        std::ifstream file;
        if (input != "-") { file.open(input, std::ios::binary); if (!file) throw std::runtime_error("cannot open " + input); }
        std::istream& stream = input == "-" ? std::cin : file;
        std::string source;
        char buffer[8192];
        while (stream) {
            stream.read(buffer, sizeof buffer); auto count = static_cast<std::size_t>(stream.gcount());
            if (source.size() + count > flintstone::max_source_bytes) throw std::runtime_error("source byte limit exceeded");
            source.append(buffer, count);
        }
        if (stream.bad()) throw std::runtime_error("input read failed");
        flintstone::Registry registry;
        flintstone::register_tiny_backends(registry);
        auto module = flintstone::parse(source, input == "-" ? "<stdin>" : input);
        if (!target_name.empty()) {
            flintstone::TinyArch arch;
            if (!flintstone::find_tiny_arch(target_name, arch)) throw std::invalid_argument("unknown --target " + target_name);
            auto bank = flintstone::make_tiny_bank(arch);
            module.target = flintstone::sandstone_target(*bank, 0, 64);
        }
        flintstone::resolve_imports(module, registry, search_dirs);
        flintstone::require_valid(module, registry);
        if (scalarize) {
            flintstone::PassManager scalarizer;
            scalarizer.add(flintstone::vector_scalarize_pass());
            scalarizer.add(flintstone::tuple_sroa_pass());
            scalarizer.run(module, registry);
        }
        if (level) flintstone::lower(module, registry, *level);
        if (!kernels.empty()) {
            if (module.level == flintstone::Level::High) flintstone::lower(module, registry, flintstone::Level::Core);
            flintstone::PassManager manager; manager.add(flintstone::blackstone_pass(directory, kernels)); manager.run(module, registry);
        }
        if (dce) { flintstone::PassManager manager; manager.add(flintstone::dead_code_elimination()); manager.run(module, registry); }
        if (!output.empty()) {
            auto artifact = flintstone::emit(module, registry, backend);
            if (!artifact.relocations.empty()) throw std::runtime_error("raw byte output would discard relocations");
            std::ofstream bytes(output, std::ios::binary | std::ios::trunc);
            bytes.write(reinterpret_cast<const char*>(artifact.bytes.data()), static_cast<std::streamsize>(artifact.bytes.size()));
            if (!bytes) throw std::runtime_error("cannot write " + output);
        }
        if (!run.empty()) {
            auto concrete = flintstone::instantiate(module, registry);
            const auto& f = concrete.function(run);
            if (f.parameters.size() != arguments.size()) throw std::invalid_argument("argument count differs from signature");
            std::vector<firestone::vm::Value> values;
            for (std::size_t n = 0; n < arguments.size(); ++n) {
                const auto& type = f.parameters[n].type;
                if (type == flintstone::Type::integer(1)) {
                    if (arguments[n] != "0" && arguments[n] != "1" && arguments[n] != "true" && arguments[n] != "false") throw std::invalid_argument("expected Boolean argument");
                    values.emplace_back(arguments[n] == "1" || arguments[n] == "true");
                } else if (type.kind == flintstone::TypeKind::Integer) values.emplace_back(number<std::int64_t>(arguments[n]));
                else if (type == flintstone::Type::floating()) values.emplace_back(real_number(arguments[n]));
                else throw std::invalid_argument("CLI execution accepts only integers and f64 arguments");
            }
            auto result = flintstone::execute(module, registry, run, values, budget);
            if (auto integer = std::get_if<std::int64_t>(&result)) std::cout << *integer;
            else if (auto real = std::get_if<double>(&result)) std::cout << *real;
            else if (auto boolean = std::get_if<bool>(&result)) std::cout << (*boolean ? "true" : "false");
            else if (std::holds_alternative<std::monostate>(result)) std::cout << "void";
            else throw std::runtime_error("non-scalar result cannot be printed by the CLI");
            std::cout << '\n';
        } else if (!check && output.empty()) std::cout << flintstone::print(module);
        return 0;
    } catch (const flintstone::Error& e) {
        for (const auto& diagnostic : e.diagnostics()) std::cerr << flintstone::format(diagnostic) << '\n';
        return 1;
    } catch (const std::invalid_argument& e) { std::cerr << "flintstone: " << e.what() << '\n'; return 2; }
    catch (const std::exception& e) { std::cerr << "flintstone: " << e.what() << '\n'; return 1; }
}
