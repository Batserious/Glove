#include <HSMS/MessageCodec.h>

#include <algorithm>
#include <limits>

namespace glove::e37 {

    MessageCodec::MessageCodec(std::uint32_t max_length) : max_length_(max_length) {}

    bool MessageCodec::has_partial_message() const { return !buffer_.empty(); }

    namespace {

        std::uint32_t read32(const std::uint8_t* bytes) {
            return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) | (std::uint32_t{bytes[2]} << 8) |
                   bytes[3];
        }

        Result<void> validate(const Message& message) {
            const auto& h = message.header;
            if (h.ptype != 0) {
                return {};
            }
            if (h.is_data()) {
                if (!h.is_primary() && h.reply_expected()) {
                    return std::unexpected(error(ErrorCode::InvalidMessage, "Reply messages must clear W"));
                }
                return {};
            }
            switch (static_cast<SType>(h.stype)) {
                case SType::SelectRequest:
                case SType::DeselectRequest:
                case SType::Separate:
                case SType::LinktestRequest:
                case SType::LinktestResponse:
                    if (h.byte2 != 0 || h.byte3 != 0) {
                        return std::unexpected(
                                error(ErrorCode::InvalidMessage, "Nonzero reserved control header fields"));
                    }
                    break;
                case SType::SelectResponse:
                case SType::DeselectResponse:
                    if (h.byte2 != 0) {
                        return std::unexpected(
                                error(ErrorCode::InvalidMessage, "Nonzero reserved response header field"));
                    }
                    break;
                case SType::Reject:
                    if (h.byte3 == 0) {
                        return std::unexpected(error(ErrorCode::InvalidMessage, "Reject reason must be nonzero"));
                    }
                    break;
                default:
                    return {};
            }
            if (!message.body.empty()) {
                return std::unexpected(
                        error(ErrorCode::InvalidMessage, "Standard control messages cannot contain a body"));
            }
            if ((h.stype == 5 || h.stype == 6) && h.session_id != 0xffff) {
                return std::unexpected(error(ErrorCode::InvalidMessage, "Linktest Session ID must be 0xffff"));
            }
            return {};
        }

    } // namespace

    std::array<std::uint8_t, 10> MessageCodec::encode_header(const Header& h) {
        return {static_cast<std::uint8_t>(h.session_id >> 8),
                static_cast<std::uint8_t>(h.session_id),
                h.byte2,
                h.byte3,
                h.ptype,
                h.stype,
                static_cast<std::uint8_t>(h.system_bytes >> 24),
                static_cast<std::uint8_t>(h.system_bytes >> 16),
                static_cast<std::uint8_t>(h.system_bytes >> 8),
                static_cast<std::uint8_t>(h.system_bytes)};
    }

    Result<Bytes> MessageCodec::encode(const Message& message, std::uint32_t max_length) {
        if (max_length < 10 || message.body.size() > max_length - 10 ||
            message.body.size() > std::numeric_limits<std::uint32_t>::max() - 10) {
            return std::unexpected(error(ErrorCode::InvalidMessage, "Outgoing message exceeds maximum length"));
        }
        if (auto result = validate(message); !result) {
            return std::unexpected(result.error());
        }
        const auto length = static_cast<std::uint32_t>(message.body.size() + 10);
        Bytes      bytes{static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
                         static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
        const auto header = encode_header(message.header);
        bytes.insert(bytes.end(), header.begin(), header.end());
        bytes.insert(bytes.end(), message.body.begin(), message.body.end());
        return bytes;
    }

    Result<std::vector<Message>> MessageCodec::feed(std::span<const std::uint8_t> bytes) {
        std::vector<Message> messages;
        while (!bytes.empty()) {
            const auto count = std::min(target_ - buffer_.size(), bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + count);
            bytes = bytes.subspan(count);
            if (buffer_.size() != target_) {
                continue;
            }
            if (target_ == 4) {
                const auto length = read32(buffer_.data());
                if (length < 10 || length > max_length_ ||
                    std::uint64_t{length} + 4 > std::numeric_limits<std::size_t>::max()) {
                    reset();
                    return std::unexpected(
                            error(ErrorCode::InvalidMessage, "Incoming message length is outside limits"));
                }
                target_ = static_cast<std::size_t>(length) + 4;
                continue;
            }
            const auto* p = buffer_.data() + 4;
            Message     message{
                    {static_cast<SessionId>((std::uint16_t{p[0]} << 8) | p[1]), p[2], p[3], p[4], p[5], read32(p + 6)},
                    {}};
            message.body.assign(buffer_.begin() + 14, buffer_.end());
            reset();
            if (auto result = validate(message); !result) {
                auto failure   = result.error();
                failure.header = message.header;
                return std::unexpected(std::move(failure));
            }
            messages.push_back(std::move(message));
        }
        return messages;
    }

    void MessageCodec::reset() {
        buffer_.clear();
        target_ = 4;
    }

} // namespace glove::e37
