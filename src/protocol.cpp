// Persistent Execution Fabric - control-plane wire protocol.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/protocol.hpp"

#include <cstring>

namespace pef {
namespace {

void write_le16(std::uint8_t* p, std::uint16_t value) noexcept {
    p[0] = static_cast<std::uint8_t>(value & 0xFFu);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void write_le32(std::uint8_t* p, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

void write_le64(std::uint8_t* p, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
    }
}

[[nodiscard]] std::uint16_t read_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

[[nodiscard]] std::uint32_t read_le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::uint64_t read_le64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

}  // namespace

std::string_view message_type_name(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello: return "HELLO";
        case MessageType::Ping: return "PING";
        case MessageType::Shutdown: return "SHUTDOWN";
        case MessageType::CreateExecution: return "CREATE_EXECUTION";
        case MessageType::BindWorker: return "BIND_WORKER";
        case MessageType::Start: return "START";
        case MessageType::BeginAction: return "BEGIN_ACTION";
        case MessageType::CompleteAction: return "COMPLETE_ACTION";
        case MessageType::FailAction: return "FAIL_ACTION";
        case MessageType::ReportSideEffect: return "REPORT_SIDE_EFFECT";
        case MessageType::RegisterCheckpoint: return "CHECKPOINT";
        case MessageType::SuspendBegin: return "SUSPEND_BEGIN";
        case MessageType::SuspendCommit: return "SUSPEND_COMMIT";
        case MessageType::Resume: return "RESUME";
        case MessageType::Fence: return "FENCE";
        case MessageType::Cancel: return "CANCEL";
        case MessageType::Retire: return "RETIRE";
        case MessageType::Drain: return "DRAIN";
        case MessageType::CompleteExecution: return "COMPLETE_EXECUTION";
        case MessageType::AdvanceGeneration: return "ADVANCE_GENERATION";
        case MessageType::UpdatePolicy: return "UPDATE_POLICY";
        case MessageType::Classify: return "CLASSIFY";
        case MessageType::Recover: return "RECOVER";
        case MessageType::ResolveAmbiguity: return "RESOLVE_AMBIGUITY";
        case MessageType::RevalidateContinuation: return "VALIDATE_CONTINUATION";
        case MessageType::Query: return "QUERY";
        case MessageType::ListExecutions: return "LIST_EXECUTIONS";
        case MessageType::Snapshot: return "SNAPSHOT";
        case MessageType::Audit: return "AUDIT";
    }
    return "UNKNOWN_MESSAGE";
}

std::optional<MessageType> parse_message_type(std::uint16_t raw) noexcept {
    const std::uint16_t base = static_cast<std::uint16_t>(raw & ~kReplyBit);
    if (base < 1 || base > kMaxMessageType) {
        return std::nullopt;
    }
    return static_cast<MessageType>(raw);
}

Bytes encode_frame(const Frame& frame) {
    const std::size_t payload_size = frame.payload.size();
    Bytes out(kFrameHeaderSize + payload_size + kFrameTrailerSize, std::byte{0});
    auto* p = reinterpret_cast<std::uint8_t*>(out.data());
    write_le32(p, kFrameMagic);
    write_le16(p + 4, frame.version);
    write_le16(p + 6, static_cast<std::uint16_t>(frame.type));
    write_le64(p + 8, frame.request.value());
    write_le32(p + 16, static_cast<std::uint32_t>(payload_size));
    write_le32(p + 20, frame.flags);
    write_le32(p + 24, static_cast<std::uint32_t>(crc64_ecma(p, 24) & 0xFFFFFFFFu));
    if (payload_size > 0) {
        std::memcpy(p + kFrameHeaderSize, frame.payload.data(), payload_size);
    }
    write_le64(p + kFrameHeaderSize + payload_size,
               crc64_ecma(p, kFrameHeaderSize + payload_size));
    return out;
}

bool decode_frame_header(const std::uint8_t* header, std::size_t size, Frame& out,
                         std::string& reason) {
    if (size < kFrameHeaderSize) {
        reason = "frame header is shorter than the fixed header size";
        return false;
    }
    if (read_le32(header) != kFrameMagic) {
        reason = "frame magic mismatch";
        return false;
    }
    const std::uint16_t version = read_le16(header + 4);
    if (version != kProtocolVersion) {
        reason = "unsupported protocol version";
        return false;
    }
    const std::uint16_t raw_type = read_le16(header + 6);
    const auto type = parse_message_type(raw_type);
    if (!type.has_value()) {
        reason = "unknown message type";
        return false;
    }
    const std::uint32_t flags = read_le32(header + 20);
    if (flags != 0) {
        reason = "frame carries non-zero reserved flags";
        return false;
    }
    if (read_le32(header + 24) != static_cast<std::uint32_t>(crc64_ecma(header, 24) & 0xFFFFFFFFu)) {
        reason = "frame header checksum mismatch";
        return false;
    }
    const std::uint32_t payload_size = read_le32(header + 16);
    if (payload_size > kMaxFramePayload) {
        reason = "frame payload exceeds the protocol limit";
        return false;
    }
    out.version = version;
    out.type = *type;
    out.request = RequestId{read_le64(header + 8)};
    out.flags = flags;
    out.payload.clear();
    return true;
}

bool decode_frame(const Bytes& bytes, Frame& out, std::string& reason) {
    if (bytes.size() < kFrameHeaderSize + kFrameTrailerSize) {
        reason = "frame is shorter than the minimum frame size";
        return false;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (!decode_frame_header(p, bytes.size(), out, reason)) {
        return false;
    }
    const std::uint32_t payload_size = read_le32(p + 16);
    if (bytes.size() != kFrameHeaderSize + static_cast<std::size_t>(payload_size) +
                            kFrameTrailerSize) {
        reason = "frame length does not match the declared payload length";
        return false;
    }
    if (read_le64(p + kFrameHeaderSize + payload_size) !=
        crc64_ecma(p, kFrameHeaderSize + payload_size)) {
        reason = "frame checksum mismatch";
        return false;
    }
    out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize),
                       bytes.begin() +
                           static_cast<std::ptrdiff_t>(kFrameHeaderSize + payload_size));
    return true;
}

Frame make_error_reply(RequestId request, Code code, std::string message) {
    Frame frame;
    frame.type = static_cast<MessageType>(static_cast<std::uint16_t>(MessageType::Ping) |
                                          kReplyBit);
    frame.request = request;
    StatusPayload payload;
    payload.code = code;
    payload.message = std::move(message);
    frame.payload = encode_payload(payload);
    return frame;
}

bool reply_status(const Frame& frame, StatusPayload& out) {
    ByteReader reader(frame.payload);
    return StatusPayload::decode(reader, out);
}

#define PEF_WIRE_IO(Type)                                                  \
    void Type::encode(ByteWriter& w) const { encode_record(w, *this); }    \
    bool Type::decode(ByteReader& r, Type& out) { return decode_record(r, out); }

PEF_WIRE_IO(StatusPayload)
PEF_WIRE_IO(CreateExecutionReply)
PEF_WIRE_IO(BindWorkerReply)
PEF_WIRE_IO(BeginActionReply)
PEF_WIRE_IO(CompleteActionReply)
PEF_WIRE_IO(CheckpointReply)
PEF_WIRE_IO(ResumeReply)
PEF_WIRE_IO(HelloRequest)
PEF_WIRE_IO(HelloReply)
PEF_WIRE_IO(PingRequest)
PEF_WIRE_IO(PingReply)
PEF_WIRE_IO(SimpleReply)
PEF_WIRE_IO(ExecutionIdRequest)
PEF_WIRE_IO(ExecutionIdReply)
PEF_WIRE_IO(UpdatePolicyRequest)
PEF_WIRE_IO(ClassifyReply)
PEF_WIRE_IO(RecoverReply)
PEF_WIRE_IO(ResolveAmbiguityReply)
PEF_WIRE_IO(QueryReply)
PEF_WIRE_IO(ListExecutionsReply)
PEF_WIRE_IO(AuditReply)
PEF_WIRE_IO(ValidateContinuationReply)
PEF_WIRE_IO(SnapshotRequest)
PEF_WIRE_IO(SnapshotReply)

#undef PEF_WIRE_IO

}  // namespace pef
