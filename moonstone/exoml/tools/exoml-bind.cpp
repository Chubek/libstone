#include <charconv>
#include <exoml/Contract.hpp>
#include <exoml/Extract.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::string read_file(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << stream.rdbuf();
    return ss.str();
}
void write_output(const std::string &text, const std::string &path) {
    if (path.empty()) {
        if (!(std::cout << text) || !std::cout.flush())
            throw std::runtime_error("cannot write stdout");
    } else {
        std::ofstream stream(path, std::ios::binary);
        if (!(stream << text) || !stream.flush())
            throw std::runtime_error("cannot write " + path);
    }
}

int run_extract(int argc, char **argv) {
    using namespace moonstone::exoml::cdecl;
    ExtractOptions options;
    std::string output;
    bool cache_report = false;
    std::set<std::string> single;
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--cache-report") {
            if (cache_report)
                throw std::invalid_argument("duplicate option " + flag);
            cache_report = true;
            continue;
        }
        if (++i == argc)
            throw std::invalid_argument("missing value for " + flag);
        std::string value = argv[i];
        if (flag == "--include")
            options.include_directories.push_back(value);
        else if (flag == "--define")
            options.defines.push_back(value);
        else if (flag == "--undefine")
            options.undefines.push_back(value);
        else if (flag == "--provenance")
            options.provenance_files.push_back(value);
        else if (flag == "--compile-arg")
            options.compiler_arguments.push_back(value);
        else {
            if (!single.insert(flag).second)
                throw std::invalid_argument("duplicate option " + flag);
            if (flag == "--header")
                options.header = value;
            else if (flag == "--revision")
                options.revision = value;
            else if (flag == "--target")
                options.target = value;
            else if (flag == "--resource-dir")
                options.resource_directory = value;
            else if (flag == "--out")
                output = value;
            else if (flag == "--cache-dir")
                options.cache_directory = value;
            else if (flag == "--compile-commands")
                options.compile_commands = value;
            else if (flag == "--compile-file")
                options.compile_file = value;
            else if (flag.starts_with("--max-")) {
                std::size_t n = 0;
                auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), n);
                if (error != std::errc{} || end != value.data() + value.size())
                    throw std::invalid_argument("invalid nonnegative integer for " + flag);
                if (flag == "--max-types")
                    options.limits.types = n;
                else if (flag == "--max-declarations")
                    options.limits.declarations = n;
                else if (flag == "--max-depth")
                    options.limits.depth = n;
                else if (flag == "--max-input-bytes")
                    options.limits.input_bytes = n;
                else if (flag == "--max-entries")
                    options.limits.entries = n;
                else if (flag == "--max-cache-bytes")
                    options.limits.cache_bytes = n;
                else
                    throw std::invalid_argument("unsupported option " + flag);
            } else
                throw std::invalid_argument("unsupported option " + flag);
        }
    }
    if (options.header.empty())
        throw std::invalid_argument("--header is required");
    CacheReport activity;
    auto unit = extract_c11(options, activity);
    for (const auto &d : unit.diagnostics)
        std::cerr << moonstone::format(d) << '\n';
    if (cache_report) {
        static constexpr const char *names[] = {"disabled", "miss",     "hit",
                                                "rebuilt",  "bypassed", "error"};
        std::cerr << "{\"cache\":{\"status\":\"" << names[unsigned(activity.status)]
                  << "\",\"stored\":" << (activity.stored ? "true" : "false") << ",\"key\":\""
                  << moonstone::json_escape(activity.key) << "\",\"reason\":\""
                  << moonstone::json_escape(activity.reason) << "\"}}\n";
    }
    auto json = describe_translation_unit(unit);
    write_output(json, output);
    return unit.complete ? 0 : 3;
}

int run_validate(int argc, char **argv) {
    using namespace moonstone::exoml;
    std::string decl_path, contract_path, output;
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        if (++i == argc)
            throw std::invalid_argument("missing value for " + flag);
        std::string value = argv[i];
        if (flag == "--decl")
            decl_path = value;
        else if (flag == "--contract")
            contract_path = value;
        else if (flag == "--out")
            output = value;
        else
            throw std::invalid_argument("unsupported option " + flag);
    }
    if (decl_path.empty())
        throw std::invalid_argument("--decl is required");
    if (contract_path.empty())
        throw std::invalid_argument("--contract is required");

    auto decl_json = read_file(decl_path);
    auto unit = cdecl::parse_translation_unit_json(decl_json);

    auto contract_json = read_file(contract_path);
    auto bundle = contract::parse_contract_sidecar(contract_json);

    auto diags = contract::validate_contract(bundle, unit);
    for (const auto &d : diags)
        std::cerr << moonstone::format(d) << '\n';

    if (!output.empty()) {
        auto normalized = contract::serialize_contract(bundle);
        write_output(normalized, output);
    }

    if (!diags.empty())
        return 3;
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("expected subcommand; see --help");
        std::string_view sub = argv[1];
        if (sub == "--help" || sub == "-h") {
            std::cout
                << "exoml-bind <subcommand> [options]\n"
                   "\n"
                   "Subcommands:\n"
                   "  extract   Discover C11 declarations from a header\n"
                   "  validate  Validate a semantic contract against extracted declarations\n"
                   "\n"
                   "extract: --header FILE [--include DIR] [--define NAME[=VALUE]]\n"
                   "  [--undefine NAME] [--target TRIPLE] [--resource-dir DIR]\n"
                   "  [--revision REV] [--provenance FILE] [--out FILE]\n"
                   "  [--compile-commands FILE] [--compile-file SOURCE] [--compile-arg ARG]\n"
                   "  [--cache-dir DIR] [--cache-report]\n"
                   "  [--max-types N] [--max-declarations N] [--max-depth N]\n"
                   "  [--max-input-bytes N] [--max-entries N] [--max-cache-bytes N]\n"
                   "\n"
                   "validate: --decl FILE --contract FILE [--out FILE]\n"
                   "  --decl: JSON output from 'extract'\n"
                   "  --contract: *.exocontract.json sidecar\n"
                   "  --out: optional normalized contract JSON output\n"
                   "Exit codes: 0 = valid, 2 = usage error, 3 = validation failures, 7 = I/O error\n";
            return 0;
        }
        if (sub == "extract")
            return run_extract(argc, argv);
        if (sub == "validate")
            return run_validate(argc, argv);
        throw std::invalid_argument("unknown subcommand: " + std::string(sub));
    } catch (const std::invalid_argument &e) {
        std::cerr << "exoml-bind: " << e.what() << '\n';
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "exoml-bind: " << e.what() << '\n';
        return 7;
    }
}
