#include "asdl/codegen.hpp"
#include "asdl/dot.hpp"
#include "asdl/schema.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view usage = R"(usage: asdlc [options] input.asdl
  --header FILE, -o FILE  AST header (default: stdout)
  --source FILE          separate C implementation (requires --header)
  --header-only          static-inline header (default)
  --visitor FILE         re-includable, macro-configured walker header
  --listener FILE        mmap event listener header
  --no-builtins          omit all ASDL runtime definitions
  --omit-builtin NAME    omit one builtin (repeatable, or comma-separated)
  --builtins-header FILE emit ASDL runtime in a separate included header
  --include NAME         AST include path for source/visitor
  --prefix NAME          C namespace (default: module name or asdl_)
  --guard NAME           override the AST include guard
  --[no-]enums           optimize nullary sums to enums (default: on)
  --[no-]line-directives preserve injected C locations (default: on)
  --check               validate without generating C files
  --schema FILE         export JSON schema; '-' selects stdout
  --cpp-header FILE     emit the C++ schema companion header
  --dot FILE            export Graphviz DOT; '-' selects stdout
  --dot-view VIEW       schema (default), dependencies, or imports
  --import-root DIR     add a schema search root (repeatable)
  --help, -h            this summary
  --man                 extended compiler help
  --version, -V         generator version
Use '-' as the input filename to read standard input.
)";

constexpr std::string_view manual = R"(
SOURCE
  A schema contains declarations such as:

      module Tree {
          span = (int start, int end)
          expr = Number(int value) | Add(expr left, expr right)
               | Block(expr+ items)
               attributes (span? location)
      }

  Type names begin lowercase; constructors begin uppercase. A field can
  omit its name. A suffix of '?' makes a field optional, '*' allows zero
  or more elements, and '+' requires at least one. Products cannot be
  recursive, even through another definition or a container. Sums can.
  Use list[type], array[type,count], array[count], or map[key,value] for
  generic container types. Comments start with '--', '//', or '#', or
  can be enclosed in '/*' and '*/'.

  A preamble may include '%{' C text '%}' and '%' option directives.
  Place '%%' on a line by itself after the preamble. A second '%%'
  terminates the declarations and starts a C epilogue. Explicit command
  line options override directives.

OUTPUT
  By default the C output is a standalone header with static-inline
  implementations. For separate compilation use --header and --source.
  Initialize a module arena before calling AST builders, check their
  results, and destroy the arena to release all arena-owned storage.
  Runtime containers passed to builders are shallow copies; keep their
  owning storage alive while the AST uses them.

  --check performs parsing, schema validation, and checks for C name
  collisions without publishing generated source files. JSON and DOT
  may still be requested with --schema and --dot. Output destinations
  must not alias the input or one another, including through hard links.
)";

struct Arguments {
    asdl::Options options;
    std::vector<std::string> inputs;
    std::vector<fs::path> import_roots;
    bool check = false, help = false, man = false, version = false;
};

Arguments parse_arguments(int argc, char** argv)
{
    Arguments args;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument(argv[i]);
        if (positional || argument == "-" || argument.empty() || argument.front() != '-') {
            args.inputs.push_back(std::move(argument));
            continue;
        }
        if (argument == "--") {
            positional = true;
            continue;
        }
        const auto dash_count = argument.compare(0, 2, "--") == 0 ? 2 : 1;
        const auto equals = argument.find('=');
        auto option = argument.substr(dash_count, equals - dash_count);
        if (option == "o") option = "header";
        if (option == "h") option = "help";
        if (option == "V") option = "version";
        if (option == "no-builtin") option = "omit-builtin";
        if (option == "I") option = "import-root";
        if (option == "header" || option == "source" || option == "visitor" ||
            option == "listener" || option == "builtins-header" || option == "omit-builtin" ||
            option == "include" || option == "prefix" || option == "guard" ||
            option == "schema" || option == "cpp-header" || option == "dot" ||
            option == "dot-view" || option == "import-root") {
            if (equals == std::string::npos && i + 1 == argc)
                throw std::runtime_error("--" + option + " requires a value");
            auto value = equals == std::string::npos ? std::string(argv[++i]) :
                argument.substr(equals + 1);
            if (option == "import-root") {
                if (value.empty())
                    throw std::runtime_error("--import-root requires a nonempty value");
                args.import_roots.emplace_back(std::move(value));
            } else if (option == "omit-builtin" && args.options.count(option)) {
                args.options[option] += ',' + value;
            } else {
                args.options[option] = std::move(value);
            }
            continue;
        }
        if (equals != std::string::npos)
            throw std::runtime_error("option '" + option + "' does not take a value");
        if (option == "check") args.check = true;
        else if (option == "help") args.help = true;
        else if (option == "man") args.man = true;
        else if (option == "version") args.version = true;
        else if (option == "header-only") args.options[option] = "1";
        else {
            const auto start = option.compare(0, 3, "no-") == 0 ? 3 :
                (option.compare(0, 2, "no") == 0 ? 2 : 0);
            const auto name = option.substr(start);
            if (name != "enums" && name != "line-directives" && name != "builtins")
                throw std::runtime_error("unknown option '" + option + "'");
            args.options[name] = start ? "0" : "1";
        }
    }
    return args;
}

std::string read_input(const std::string& filename)
{
    std::ifstream file;
    if (filename != "-") {
        file.open(filename, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open '" + filename + "': " + std::strerror(errno));
    }
    auto& input = filename == "-" ? std::cin : file;
    std::string buffer;
    char area[8192];
    while (input.read(area, sizeof area) || input.gcount())
        buffer.append(area, static_cast<std::size_t>(input.gcount()));
    if (input.bad() || !input.eof())
        throw std::runtime_error("cannot read '" + filename + "'");
    return buffer;
}

struct Output {
    std::string path;
    std::string text;
};

using Inode = std::pair<dev_t, ino_t>;

std::string system_error(const char* operation, const std::string& path, int err)
{
    return std::string(operation) + " '" + path + "': " + std::strerror(err);
}

fs::path parent_of(const fs::path& path)
{
    return path.has_parent_path() ? path.parent_path() : fs::path(".");
}

std::string canonical_output(const std::string& path)
{
    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    if (!ec)
        return resolved.string();
    auto parent = fs::canonical(parent_of(path), ec);
    if (ec || !fs::is_directory(parent))
        throw std::runtime_error("output directory does not exist for '" + path + "'");
    return (parent / fs::path(path).filename()).lexically_normal().string();
}

struct StagedOutput {
    std::string path;
    std::vector<char> temporary;
    int fd = -1;
    bool renamed = false;

    explicit StagedOutput(const std::string& path) : path(path)
    {
        auto pattern = (parent_of(path) / ".asdl-XXXXXXXX").string();
        temporary.assign(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        fd = mkstemp(temporary.data());
        if (fd == -1)
            throw std::runtime_error(system_error("cannot stage", path, errno));
    }

    StagedOutput(const StagedOutput&) = delete;
    StagedOutput& operator=(const StagedOutput&) = delete;
    StagedOutput(StagedOutput&&) = delete;
    StagedOutput& operator=(StagedOutput&&) = delete;

    ~StagedOutput()
    {
        if (fd != -1)
            close(fd);
        if (!renamed)
            unlink(temporary.data());
    }

    void write_text(std::string_view text, mode_t mode)
    {
        while (!text.empty()) {
            const auto count = write(fd, text.data(), std::min<std::size_t>(text.size(), 65536));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error(system_error("cannot write", path, count < 0 ? errno : EIO));
            text.remove_prefix(static_cast<std::size_t>(count));
        }
        if (fchmod(fd, mode) < 0)
            throw std::runtime_error(system_error("cannot set mode for", path, errno));
        const auto handle = std::exchange(fd, -1);
        if (close(handle) < 0)
            throw std::runtime_error(system_error("cannot close", path, errno));
    }

    void publish()
    {
        if (rename(temporary.data(), path.c_str()) < 0)
            throw std::runtime_error(system_error("cannot publish", path, errno));
        renamed = true;
    }
};

void publish_outputs(const std::string& input, const std::vector<Output>& outputs)
{
    std::map<std::string, std::string> paths;
    std::map<Inode, std::string> inodes;
    struct stat status {};
    if (input != "-") {
        paths.emplace(fs::canonical(input).string(), "input");
        if (stat(input.c_str(), &status) == 0)
            inodes.emplace(Inode{status.st_dev, status.st_ino}, "input");
    }
    // This executable is single-threaded; reading umask uses a temporary
    // change and is intentionally kept out of the embedding library.
    const auto mask = umask(0);
    umask(mask);
    std::vector<mode_t> modes;
    modes.reserve(outputs.size());
    for (const auto& output : outputs) {
        const auto& path = output.path;
        const auto label = "output '" + path + "'";
        const auto canonical = path == "-" ? std::string("<stdout>") : canonical_output(path);
        const auto [path_entry, path_added] = paths.emplace(canonical, label);
        if (!path_added)
            throw std::runtime_error(label + " aliases " + path_entry->second);
        auto mode = static_cast<mode_t>(0666 & ~mask);
        if (path != "-" && stat(path.c_str(), &status) == 0) {
            if (!S_ISREG(status.st_mode))
                throw std::runtime_error(label + " is not a regular file");
            const auto [node_entry, node_added] =
                inodes.emplace(Inode{status.st_dev, status.st_ino}, label);
            if (!node_added)
                throw std::runtime_error(label + " aliases " + node_entry->second);
            mode = status.st_mode & 0777;
        }
        modes.push_back(mode);
    }

    // Nothing is replaced until every destination passes preflight and
    // every temporary output has been written and closed successfully.
    std::vector<std::unique_ptr<StagedOutput>> staged;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].path == "-")
            continue;
        auto& file = staged.emplace_back(std::make_unique<StagedOutput>(outputs[i].path));
        file->write_text(outputs[i].text, modes[i]);
    }
    for (auto& file : staged)
        file->publish();
    for (const auto& output : outputs) {
        if (output.path == "-") {
            std::cout << output.text << std::flush;
            if (!std::cout)
                throw std::runtime_error("cannot write stdout");
        }
    }
}

asdl::DotView dot_view(const asdl::Options& options)
{
    const auto entry = options.find("dot-view");
    if (entry == options.end() || entry->second == "schema")
        return asdl::DotView::schema;
    if (entry->second == "dependencies")
        return asdl::DotView::dependencies;
    if (entry->second == "imports")
        return asdl::DotView::imports;
    throw std::runtime_error("unknown DOT view '" + entry->second + "'");
}

int run(int argc, char** argv)
{
    const auto args = parse_arguments(argc, argv);
    if (args.help) {
        std::cout << usage;
        return 0;
    }
    if (args.version) {
        std::cout << "asdlc 2.0\n";
        return 0;
    }
    if (args.man) {
        std::cout << usage << manual;
        return 0;
    }
    if (args.inputs.size() != 1)
        throw std::runtime_error(std::string(usage));

    const auto& input = args.inputs.front();
    auto schema = asdl::parse(read_input(input), input);
    asdl::resolve(schema, args.import_roots);
    asdl::resolve_options(schema, args.options);
    asdl::validate(schema);
    const auto view = dot_view(schema.options);
    // The AST emitter still checks target-language name collisions during
    // --check. Derived source artifacts are not rendered in that mode.
    if (args.check)
        for (const auto* artifact : {"visitor", "listener", "cpp-header"})
            schema.options.erase(artifact);
    auto generated = asdl::generate(schema);
    std::vector<Output> outputs;
    const auto add = [&](const char* key, std::string text) {
        if (const auto option = schema.options.find(key); option != schema.options.end())
            outputs.push_back({option->second, std::move(text)});
    };
    if (!args.check) {
        const auto header = schema.options.find("header");
        outputs.push_back({header == schema.options.end() ? "-" : header->second,
                           std::move(generated.header)});
        add("source", std::move(generated.source));
        add("visitor", std::move(generated.visitor));
        add("listener", std::move(generated.listener));
        add("builtins-header", std::move(generated.builtins_header));
        add("cpp-header", std::move(generated.cpp_header));
    }
    add("schema", std::move(generated.schema));
    if (schema.options.count("dot"))
        add("dot", asdl::emit_dot(schema, view));
    publish_outputs(input, outputs);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        const std::string_view message = error.what();
        std::cerr << "asdlc: " << message;
        if (message.empty() || message.back() != '\n')
            std::cerr << '\n';
        return 1;
    }
}
