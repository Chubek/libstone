#include "mahak/coordination/Protocol.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace moonstone::mahak::coordination {
namespace {

Error invalid(std::string message) {
    Error error;
    error.category = ErrorCategory::InvalidInput;
    error.message = std::move(message);
    return error;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool simple_id(std::string_view text) {
    if (text.empty() || text.size() > 64)
        return false;
    for (char c : text) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (!(std::isalnum(value) || c == '-' || c == '_'))
            return false;
    }
    return true;
}

} // namespace

const char *name(MessageType type) noexcept {
    switch (type) {
    case MessageType::Request:
        return "REQUEST";
    case MessageType::Response:
        return "RESPONSE";
    case MessageType::Ack:
        return "ACK";
    case MessageType::Blocked:
        return "BLOCKED";
    case MessageType::Contract:
        return "CONTRACT";
    case MessageType::Review:
        return "REVIEW";
    case MessageType::Status:
        return "STATUS";
    case MessageType::Error:
        return "ERROR";
    case MessageType::Complete:
        return "COMPLETE";
    }
    return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::string_view text) noexcept {
    if (text == "REQUEST")
        return MessageType::Request;
    if (text == "RESPONSE")
        return MessageType::Response;
    if (text == "ACK")
        return MessageType::Ack;
    if (text == "BLOCKED")
        return MessageType::Blocked;
    if (text == "CONTRACT")
        return MessageType::Contract;
    if (text == "REVIEW")
        return MessageType::Review;
    if (text == "STATUS")
        return MessageType::Status;
    if (text == "ERROR")
        return MessageType::Error;
    if (text == "COMPLETE")
        return MessageType::Complete;
    return std::nullopt;
}

Result<void> validate_envelope(const Envelope &envelope) {
    if (envelope.id.empty() || envelope.id.size() > 128)
        return invalid("envelope id must be 1..128 bytes");
    if (!simple_id(envelope.sender))
        return invalid("invalid sender id");
    if (envelope.recipient != "broker" && envelope.recipient != "*" &&
        !simple_id(envelope.recipient))
        return invalid("invalid recipient id");
    if (envelope.sequence == 0)
        return invalid("sequence must be positive");
    if (envelope.priority < 0 || envelope.priority > 31)
        return invalid("priority must be 0..31");
    if (!envelope.payload.is_object())
        return invalid("payload must be a JSON object");
    if (envelope.timestamp_ns == 0)
        return invalid("timestamp_ns must be set");
    return {};
}

Result<std::string> encode_envelope(const Envelope &input) {
    Envelope envelope = input;
    if (envelope.timestamp_ns == 0)
        envelope.timestamp_ns = now_ns();
    if (envelope.id.empty())
        envelope.id = envelope.sender + "-" + std::to_string(envelope.sequence);
    if (auto valid = validate_envelope(envelope); !valid)
        return valid.error();
    Json root = Json::object();
    root.set("v", 1);
    root.set("id", envelope.id);
    root.set("sender", envelope.sender);
    root.set("recipient", envelope.recipient);
    root.set("sequence", envelope.sequence);
    root.set("correlation", envelope.correlation_id);
    root.set("type", name(envelope.type));
    root.set("priority", envelope.priority);
    root.set("timestamp_ns", envelope.timestamp_ns);
    root.set("payload", envelope.payload);
    const std::string text = root.dump();
    if (text.empty())
        return invalid("failed to serialize envelope");
    if (text.size() > max_envelope_bytes) {
        Error error;
        error.category = ErrorCategory::BudgetExhausted;
        error.message = "envelope exceeds the POSIX queue message budget (" +
                        std::to_string(text.size()) + " > " +
                        std::to_string(max_envelope_bytes) + " bytes)";
        return error;
    }
    return text;
}

Result<Envelope> decode_envelope(std::string_view text) {
    if (text.empty())
        return invalid("empty envelope");
    if (text.size() > max_envelope_bytes)
        return invalid("envelope exceeds message budget");
    auto parsed = Json::parse(text, "<mq-envelope>");
    if (!parsed)
        return parsed.error();
    const Json &root = parsed.value();
    if (!root.is_object())
        return invalid("envelope is not an object");
    const Json *version = root.find("v");
    if (!version || version->as_int64().value_or(0) != 1)
        return invalid("unsupported envelope version");
    static const char *allowed[] = {"v",       "id",        "sender",  "recipient",
                                    "sequence", "correlation", "type",  "priority",
                                    "timestamp_ns", "payload"};
    for (const auto &[key, value] : *root.as_object()) {
        bool known = false;
        for (const char *candidate : allowed)
            known = known || key == candidate;
        if (!known)
            return invalid("unknown envelope key: " + key);
        (void)value;
    }
    Envelope envelope;
    auto id = root.find("id");
    auto sender = root.find("sender");
    auto recipient = root.find("recipient");
    auto sequence = root.find("sequence");
    auto correlation = root.find("correlation");
    auto type = root.find("type");
    auto priority = root.find("priority");
    auto timestamp = root.find("timestamp_ns");
    auto payload = root.find("payload");
    if (!id || !sender || !recipient || !sequence || !type || !priority || !timestamp ||
        !payload)
        return invalid("envelope is missing a required field");
    envelope.id = id->as_string() ? *id->as_string() : "";
    envelope.sender = sender->as_string() ? *sender->as_string() : "";
    envelope.recipient = recipient->as_string() ? *recipient->as_string() : "";
    envelope.sequence = sequence->as_uint64().value_or(0);
    if (correlation) {
        if (const auto *value = correlation->as_string())
            envelope.correlation_id = *value;
    }
    const auto *type_name = type->as_string();
    if (!type_name)
        return invalid("type must be a string");
    const auto parsed_type = parse_message_type(*type_name);
    if (!parsed_type)
        return invalid("unknown message type: " + *type_name);
    envelope.type = *parsed_type;
    envelope.priority = static_cast<int>(priority->as_int64().value_or(0));
    envelope.timestamp_ns = timestamp->as_uint64().value_or(0);
    envelope.payload = *payload;
    if (auto valid = validate_envelope(envelope); !valid)
        return valid.error();
    return envelope;
}

std::string make_session_id() {
    std::array<unsigned char, 8> bytes{};
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0)
                break;
            offset += static_cast<std::size_t>(count);
        }
        ::close(fd);
    }
    if (bytes[0] == 0 && bytes[1] == 0) {
        const std::uint64_t seed = now_ns() ^ (static_cast<std::uint64_t>(::getpid()) << 32);
        std::mt19937_64 generator(seed);
        for (auto &byte : bytes)
            byte = static_cast<unsigned char>(generator());
    }
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(16);
    for (unsigned char byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

bool valid_session_id(std::string_view session) noexcept {
    if (session.size() < 4 || session.size() > 64)
        return false;
    for (char c : session) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool valid_worker_id(std::string_view worker) noexcept {
    return simple_id(worker) && worker != "broker" && worker != "*";
}

Result<std::string> mailbox_name(std::string_view session, std::string_view suffix) {
    if (!valid_session_id(session))
        return invalid("invalid session id");
    if (suffix.empty() || suffix.size() > 32)
        return invalid("invalid mailbox suffix");
    std::string name = "/mahak-" + std::string(session) + "-" + std::string(suffix);
    if (name.size() > 200)
        return invalid("mailbox name is too long");
    return name;
}

Result<std::string> blotter_root(std::string_view session) {
    if (!valid_session_id(session))
        return invalid("invalid session id");
    return "/dev/shm/mahak-" + std::string(session);
}

} // namespace moonstone::mahak::coordination
