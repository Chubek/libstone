#pragma once
#include "generated_frontend.hpp"
#include "protocol.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smtlib2::detail {
namespace frontend_cli = firestone_frontend::cli;
namespace frontend_repl = firestone_frontend::repl;

inline frontend_cli::Dialect frontend_dialect() {
    frontend_cli::Dialect dialect;
    dialect.cluster_short = false;
    dialect.negative_numbers = false;
    return dialect;
}

// File names can contain newlines and terminal control characters. Display
// their bytes without letting a path inject a second diagnostic or an escape.
inline std::string frontend_display_path(std::string_view path) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string quoted = "'";
    for (unsigned char c : path) {
        if (c == '\\' || c == '\'') {
            quoted += '\\';
            quoted += static_cast<char>(c);
        } else if (c >= ' ' && c <= '~') {
            quoted += static_cast<char>(c);
        } else {
            quoted += "\\x";
            quoted += hex[c >> 4];
            quoted += hex[c & 15];
        }
    }
    return quoted + '\'';
}

// A synchronized std::cin can present a C stdio read error as clean EOF.
// Keep stdio's buffering, but preserve its error indicator for the driver.
class FrontendStdinBuffer : public std::streambuf {
    char byte_{};

    int_type underflow() override {
        if (gptr() != egptr()) return traits_type::to_int_type(*gptr());
        const int next = std::getc(stdin);
        if (next == EOF) {
            if (std::ferror(stdin)) throw std::ios_base::failure("stdin read failed");
            return traits_type::eof();
        }
        byte_ = traits_type::to_char_type(next);
        setg(&byte_, &byte_, &byte_ + 1);
        return traits_type::to_int_type(byte_);
    }
};

/// @implstring Interleave Firestone's line directives with complete SMT-LIB
/// commands. Only inspect a colon at a command boundary, never inside SMT text.
inline int frontend_interact(Session& session, std::istream& in, bool prompts) {
    frontend_cli::Command root;
    root.name = "smtlib2";
    root.description = ":help   list directives\n:quit   end this session";
    root.dialect = frontend_dialect();
    for (auto name : {"help", "quit"}) {
        frontend_cli::Command child;
        child.name = name;
        child.description = root.description;
        root.subcommands.push_back(std::move(child));
    }
    frontend_repl::Directives directives(root);
    bool done = false;
    directives.bind({"quit"}, [&](const auto&) {
        done = true;
        return frontend_repl::Evaluation{};
    });
    directives.bind({"help"}, [&](const auto&) {
        return frontend_repl::Evaluation{true, root.description};
    });
    while (!session.exited() && !done) {
        if (prompts && !(std::cerr << "smtlib2> " << std::flush))
            throw std::ios_base::failure("REPL prompt output failed");
        int c;
        for (;;) {
            c = in.peek();
            if (c == std::char_traits<char>::eof()) {
                if (!in.eof() || in.bad()) throw std::ios_base::failure("REPL input failed");
                return 0;
            }
            if (lex_space(static_cast<char>(c))) in.get();
            else if (c == ';') {
                do { c = in.get(); }
                while (c != '\r' && c != '\n' && c != std::char_traits<char>::eof());
            } else break;
        }
        if (c != ':') {
            if (!drive_protocol(session, in, std::cout, {},
                                ProtocolErrorPolicy::report_and_continue, true)) return 1;
            continue;
        }
        std::string line;
        while ((c = in.get()) != std::char_traits<char>::eof() && c != '\r' && c != '\n') {
            if (line.size() == 64 * 1024) throw std::runtime_error("directive byte limit exceeded");
            line += static_cast<char>(c);
        }
        if (c == std::char_traits<char>::eof() && (in.bad() || !in.eof()))
            throw std::ios_base::failure("REPL input failed");
        auto answer = directives.dispatch(line).value();
        if (!answer.output.empty()) {
            if (!(std::cout << (answer.success ? answer.output : protocol_error_text(answer.output))
                            << '\n' << std::flush))
                throw std::ios_base::failure("REPL output failed");
        }
    }
    return 0;
}

/// @implstring Parse a native Firestone command schema and drive one Session.
/// Library users get no entry point unless they select an executable mode.
inline int frontend_main(int argc, char** argv, bool repl) {
    try {
        frontend_cli::Command cmd;
        cmd.name = repl ? "smtlib2-repl" : "smtlib2-cli";
        cmd.dialect = frontend_dialect();
        if (repl) {
            cmd.description = "Usage: smtlib2-repl [-h|--help] [--no-prompt]\n"
                              "Enter SMT-LIB commands or :help.";
            frontend_cli::Option quiet;
            quiet.name = "no-prompt";
            cmd.options.push_back(std::move(quiet));
        } else {
            cmd.description = "Usage: smtlib2-cli [-h|--help] [--] [FILE|-]\n"
                              "Read SMT-LIB from FILE or stdin. Use -- before a dash-prefixed FILE.";
            frontend_cli::Positional file;
            file.name = "file";
            file.required = false;
            cmd.positionals.push_back(std::move(file));
        }
        frontend_cli::OptParser parser(std::move(cmd));
        auto args = parser.parse(argc > 1 ? std::vector<std::string>(argv + 1, argv + argc)
                                          : std::vector<std::string>{});
        if (!args) {
            for (const auto& error : args.errors) std::cerr << error << '\n';
            return 2;
        }
        if (args.help_requested) {
            if (!(std::cout << parser.help() << '\n' << std::flush))
                throw std::ios_base::failure("help output failed");
            return 0;
        }
        Session session;
        if (repl || args.positionals.empty() || args.positionals.front() == "-") {
            FrontendStdinBuffer buffer;
            std::istream input(&buffer);
            if (repl) return frontend_interact(session, input, !args.has("no-prompt"));
            run_protocol(session, input, std::cout);
        } else {
            const auto& path = args.positionals.front();
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open input file " + frontend_display_path(path));
            try {
                run_protocol(session, file, std::cout);
            } catch (const std::ios_base::failure&) {
                if (file.bad() || (file.fail() && !file.eof()))
                    throw std::runtime_error("cannot read input file " + frontend_display_path(path));
                throw;
            }
        }
        return 0;
    } catch (const Error& error) {
        if (!(std::cout << protocol_error_text(error.what()) << '\n' << std::flush))
            std::cerr << "SMT-LIB protocol error output failed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
}
