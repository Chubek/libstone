#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mahak/Json.hpp"

namespace moonstone::mahak::coordination {

// Message classes from the Mahak development protocol. Every worker-to-worker
// message is an Envelope routed through the broker.
enum class MessageType {
    Request,
    Response,
    Ack,
    Blocked,
    Contract,
    Review,
    Status,
    Error,
    Complete
};

const char *name(MessageType type) noexcept;
std::optional<MessageType> parse_message_type(std::string_view text) noexcept;

inline constexpr std::size_t max_envelope_bytes = 480; // 9 queues x 512 B fit the 8 KiB host budget

struct Envelope {
    std::string id;            // unique; "<sender>-<sequence>"
    std::string sender;        // worker id, or "broker"
    std::string recipient;     // worker id, "broker", or "*" for broadcast
    std::uint64_t sequence = 0;
    std::string correlation_id;
    MessageType type = MessageType::Request;
    int priority = 0;          // 0..31, higher is delivered first
    std::uint64_t timestamp_ns = 0;
    Json payload = Json::object();
};

// Schema: {"v":1,"id":...,"sender":...,"recipient":...,"sequence":...,
//          "correlation":...,"type":"REQUEST","priority":..., "timestamp_ns":...,
//          "payload":{...}}
// The encoded form is strict and bounded. Payload must be a JSON object.
Result<std::string> encode_envelope(const Envelope &envelope);
Result<Envelope> decode_envelope(std::string_view text);
Result<void> validate_envelope(const Envelope &envelope);

// Stable, collision-resistant session ids are short enough for mq names.
std::string make_session_id();
bool valid_session_id(std::string_view session) noexcept;
bool valid_worker_id(std::string_view worker) noexcept;
// /mahak-<session>-<suffix>; suffix is "broker" or "w<worker>".
Result<std::string> mailbox_name(std::string_view session, std::string_view suffix);
Result<std::string> blotter_root(std::string_view session);

} // namespace moonstone::mahak::coordination
