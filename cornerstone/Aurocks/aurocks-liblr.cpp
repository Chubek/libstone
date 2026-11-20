#include <liblr/DFAMinimization.hpp>

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

namespace lr = firestone::lr;

constexpr std::size_t max_nfa_states = 16'384;
constexpr std::size_t max_nfa_edges = 131'072;
constexpr std::size_t max_input_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_output_states = 16'384;
constexpr std::size_t label_size = lr::byte_count / 4;

// A small NFA can still expand exponentially. Bound the native process
// independently of the input and output limits where the OS supports it.
#if defined(RLIMIT_CPU) || defined(RLIMIT_AS)
void limit_resource(int resource, rlim_t limit) {
    struct rlimit current {};
    if (getrlimit(resource, &current) != 0)
        throw std::runtime_error("cannot read process resource limit");
    if (current.rlim_cur == RLIM_INFINITY || current.rlim_cur > limit) {
        current.rlim_cur = limit;
        if (setrlimit(resource, &current) != 0)
            throw std::runtime_error("cannot set process resource limit");
    }
}
#endif

void limit_expansion() {
#ifdef RLIMIT_CPU
    limit_resource(RLIMIT_CPU, 10);
#endif
#ifdef RLIMIT_AS
    limit_resource(RLIMIT_AS, 512 * 1024 * 1024);
#endif
}

class Input {
    std::istream& stream_;
    std::size_t bytes_ = 0;

    int get() {
        int c = stream_.get();
        if (c == std::char_traits<char>::eof()) {
            if (stream_.bad() || !stream_.eof())
                throw std::runtime_error("cannot read input");
        } else if (++bytes_ > max_input_bytes) {
            throw std::runtime_error("input size limit exceeded");
        }
        return c;
    }

    static bool is_space(int c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    int next_nonspace() {
        int c;
        do {
            c = get();
        } while (is_space(c));
        return c;
    }

public:
    explicit Input(std::istream& stream) : stream_(stream) {}

    std::string token(const char* field) {
        std::string value;
        for (int c = next_nonspace(); c != std::char_traits<char>::eof() && !is_space(c); c = get()) {
            if (value.size() == label_size)
                throw std::runtime_error(std::string(field) + ": token too long");
            value.push_back(static_cast<char>(c));
        }
        if (value.empty())
            throw std::runtime_error(std::string("missing ") + field);
        return value;
    }

    std::size_t number(const char* field, std::size_t min, std::size_t max) {
        const auto text = token(field);
        const char* end = text.data() + text.size();
        std::size_t value = 0;
        const auto result = std::from_chars(text.data(), end, value);
        if (result.ec != std::errc{} || result.ptr != end || value < min || value > max)
            throw std::runtime_error(std::string(field) + ": expected decimal integer in " +
                                     std::to_string(min) + ".." + std::to_string(max));
        return value;
    }

    void finish() {
        if (next_nonspace() != std::char_traits<char>::eof())
            throw std::runtime_error("unexpected trailing data");
    }
};

lr::ByteSet parse_bytes(const std::string& label) {
    constexpr auto label_error = "edge label: expected '-' or 64 lowercase hex digits";
    if (label.size() != label_size)
        throw std::runtime_error(label_error);

    lr::ByteSet bytes;
    for (std::size_t group = 0; group < label_size; ++group) {
        const char c = label[group];
        unsigned nibble;
        if (c >= '0' && c <= '9')
            nibble = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = static_cast<unsigned>(c - 'a' + 10);
        else
            throw std::runtime_error(label_error);
        for (unsigned bit = 0; bit < 4; ++bit)
            if (nibble & (1u << bit))
                bytes.set(4 * group + bit);
    }
    return bytes;
}

lr::NFA read_nfa(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error(std::string("cannot open input: ") + path);

    Input input(stream);
    const auto count = input.number("state count", 1, max_nfa_states);
    const auto start = input.number("start state", 0, count - 1);
    lr::NFA nfa;
    for (std::size_t i = 1; i < count; ++i)
        nfa.add_state();
    nfa.set_start(start);

    std::size_t remaining_edges = max_nfa_edges;
    for (lr::StateId state = 0; state < count; ++state) {
        if (input.number("acceptance flag", 0, 1))
            nfa.set_acceptance(state, lr::Acceptance{});
        const auto edges = input.number("edge count", 0, remaining_edges);
        remaining_edges -= edges;
        for (std::size_t edge = 0; edge < edges; ++edge) {
            const auto target = input.number("edge target", 0, count - 1);
            const auto label = input.token("edge label");
            if (label == "-")
                nfa.add_epsilon(state, target);
            else
                nfa.add_transition(state, parse_bytes(label), target);
        }
    }
    input.finish();
    return nfa;
}

void write_dfa(const lr::DFA& dfa, const char* path) {
    const auto& states = dfa.states();
    if (states.size() > max_output_states)
        throw std::runtime_error("output state limit exceeded");

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error(std::string("cannot open output: ") + path);
    stream << states.size() << ' ' << dfa.start() << '\n';
    for (const auto& state : states) {
        stream << (state.acceptance ? '1' : '0');
        for (auto target : state.transitions) {
            stream << ' ';
            if (target == lr::no_state)
                stream << "-1";
            else
                stream << target;
        }
        stream << '\n';
    }
    stream.close();
    if (!stream)
        throw std::runtime_error(std::string("cannot write output: ") + path);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: aurocks-liblr INPUT_PATH OUTPUT_PATH\n";
        return EXIT_FAILURE;
    }
    try {
        const auto nfa = read_nfa(argv[1]);
        limit_expansion();
        const auto dfa = lr::minimize(lr::determinize(nfa));
        write_dfa(dfa, argv[2]);
    } catch (const std::bad_alloc&) {
        std::cerr << "aurocks-liblr: not enough memory to process automaton\n";
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "aurocks-liblr: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
