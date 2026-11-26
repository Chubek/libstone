// POSIX message-queue transport for ASDL development sessions.
#include <mqueue.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
using namespace std::literals;
constexpr std::array roles{"cpp"sv, "grammar"sv, "codegen"sv, "lua"sv,
    "loader"sv, "stdlib"sv, "logistics"sv, "integration"sv, "abi"sv};
constexpr std::array kinds{"REQUEST"sv, "RESPONSE"sv, "PROPOSAL"sv, "ACCEPT"sv,
    "REJECT"sv, "BLOCKED"sv, "READY"sv, "REVIEW"sv, "PATCH"sv, "HEARTBEAT"sv,
    "DONE"sv, "ABORT"sv};
constexpr auto version = "ASDL-MQ/1"sv;

[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void require(bool valid, const char* reason) {
    if (!valid) throw std::invalid_argument(reason);
}

bool token(std::string_view text, std::size_t max) {
    return !text.empty() && text.size() <= max &&
        std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}

bool role(std::string_view name) {
    return std::find(roles.begin(), roles.end(), name) != roles.end();
}

std::string name(std::string_view session, std::string_view recipient) {
    require(token(session, 48), "invalid session identifier");
    require(role(recipient), "invalid recipient role");
    return "/asdl_" + std::string(session) + "_" + std::string(recipient);
}

long number(std::string_view text, long min, long max) {
    long value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(error == std::errc{} && end == text.data() + text.size() &&
        value >= min && value <= max, "integer argument out of range");
    return value;
}

struct Queue {
    const mqd_t handle;
    explicit Queue(const std::string& name, int flags, mq_attr* settings = nullptr)
        : handle(settings ? mq_open(name.c_str(), flags, 0600, settings)
                          : mq_open(name.c_str(), flags)) {
        if (handle == static_cast<mqd_t>(-1)) fail("mq_open");
    }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    ~Queue() {
        if (mq_close(handle) == -1) std::perror("mq_close");
    }
    mq_attr attributes() const {
        mq_attr attr{};
        if (mq_getattr(handle, &attr) == -1) fail("mq_getattr");
        return attr;
    }
};

timespec deadline(long milliseconds) {
    timespec time{};
    if (clock_gettime(CLOCK_REALTIME, &time) == -1) fail("clock_gettime");
    time.tv_sec += milliseconds / 1000;
    time.tv_nsec += (milliseconds % 1000) * 1000000;
    time.tv_sec += time.tv_nsec / 1000000000;
    time.tv_nsec %= 1000000000;
    return time;
}

using Header = std::array<std::string_view, 7>;

long validate(const Header& fields, std::string_view recipient) {
    const auto& [wire, sender, target, id, correlation, kind, priority] = fields;
    require(wire == version, "unsupported wire version");
    require(role(sender) && target == recipient, "invalid sender or recipient");
    require(token(id, 64) && token(correlation, 64), "invalid message or correlation ID");
    require(std::find(kinds.begin(), kinds.end(), kind) != kinds.end(), "invalid message kind");
    // POSIX guarantees at least 32 distinct priorities.
    return number(priority, 0, 31);
}

void validate_frame(std::string_view frame, std::string_view recipient, unsigned priority) {
    const auto end = frame.find('\n');
    require(end != frame.npos, "missing frame header");
    auto line = frame.substr(0, end);
    Header fields;
    for (std::size_t i = 0; i < fields.size() - 1; ++i) {
        const auto tab = line.find('\t');
        require(tab != line.npos, "malformed frame header");
        fields[i] = line.substr(0, tab);
        line.remove_prefix(tab + 1);
    }
    fields.back() = line;
    require(validate(fields, recipient) == priority, "kernel priority disagrees with frame");
}
} // namespace

int main(int argc, char** argv) try {
    if (argc < 4) {
        std::cerr <<
            "Usage: mqctl create SESSION ROLE [DEPTH [SIZE]]\n"
            "       mqctl inspect|cleanup SESSION ROLE\n"
            "       mqctl receive SESSION ROLE [TIMEOUT_MS]\n"
            "       mqctl send SESSION ROLE SENDER ID CORRELATION KIND PRIORITY [TIMEOUT_MS] <PAYLOAD\n";
        return 2;
    }
    std::string_view command = argv[1], recipient = argv[3];
    const auto queue_name = name(argv[2], recipient);
    if (command == "create") {
        require(argc <= 6, "too many arguments");
        mq_attr attr{};
        attr.mq_maxmsg = argc > 4 ? number(argv[4], 1, 65536) : 4;
        attr.mq_msgsize = argc > 5 ? number(argv[5], 128, 1048576) : 2048;
        Queue queue(queue_name, O_CREAT | O_EXCL | O_RDONLY, &attr);
        std::cout << queue_name << '\n';
    } else if (command == "cleanup") {
        require(argc == 4, "too many arguments");
        if (mq_unlink(queue_name.c_str()) == -1 && errno != ENOENT) fail("mq_unlink");
    } else if (command == "inspect") {
        require(argc == 4, "too many arguments");
        const auto attr = Queue(queue_name, O_RDONLY).attributes();
        std::cout << queue_name << " depth=" << attr.mq_maxmsg
                  << " size=" << attr.mq_msgsize << " pending=" << attr.mq_curmsgs << '\n';
    } else if (command == "receive") {
        require(argc <= 5, "too many arguments");
        const auto ms = argc > 4 ? number(argv[4], 0, 300000) : 1000;
        const Queue queue(queue_name, O_RDONLY);
        std::string buffer(static_cast<std::size_t>(queue.attributes().mq_msgsize), '\0');
        unsigned priority{};
        const auto until = deadline(ms);
        ssize_t received;
        do {
            received = mq_timedreceive(queue.handle, buffer.data(), buffer.size(), &priority, &until);
        } while (received == -1 && errno == EINTR);
        if (received == -1) fail("mq_timedreceive");
        buffer.resize(static_cast<std::size_t>(received));
        validate_frame(buffer, recipient, priority);
        std::cout << buffer;
    } else if (command == "send") {
        require(argc == 9 || argc == 10, "wrong number of arguments");
        const auto ms = argc > 9 ? number(argv[9], 0, 300000) : 1000;
        Header fields{version, argv[4], recipient, argv[5], argv[6], argv[7], argv[8]};
        const auto priority = static_cast<unsigned>(validate(fields, recipient));
        std::string frame;
        for (const auto field : fields) {
            frame.append(field);
            frame.push_back('\t');
        }
        frame.back() = '\n';
        const Queue queue(queue_name, O_WRONLY);
        auto max = static_cast<std::size_t>(queue.attributes().mq_msgsize);
        for (char c; frame.size() <= max && std::cin.get(c);) frame.push_back(c);
        if (std::cin.bad() || (std::cin.fail() && !std::cin.eof()))
            throw std::runtime_error("reading payload failed");
        require(frame.size() <= max, "frame exceeds mq_msgsize; send an artifact reference");
        const auto until = deadline(ms);
        int result;
        do {
            result = mq_timedsend(queue.handle, frame.data(), frame.size(), priority, &until);
        } while (result == -1 && errno == EINTR);
        if (result == -1) fail("mq_timedsend");
    } else {
        throw std::invalid_argument("unknown command");
    }
    if (!std::cout.flush()) throw std::runtime_error("writing output failed");
    return 0;
} catch (const std::system_error& error) {
    std::cerr << "mqctl: " << error.what() << '\n';
    return error.code() == std::errc::timed_out || error.code() == std::errc::resource_unavailable_try_again
        ? 3 : 1;
} catch (const std::invalid_argument& error) {
    std::cerr << "mqctl: " << error.what() << '\n';
    return 2;
} catch (const std::exception& error) {
    std::cerr << "mqctl: " << error.what() << '\n';
    return 1;
}
