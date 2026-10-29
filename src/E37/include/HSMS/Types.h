#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace glove::e37 {

    using Bytes         = std::vector<std::uint8_t>;
    using SessionId     = std::uint16_t;
    using TransactionId = std::uint64_t;

    enum class Mode { Generic, HsmsSs };
    enum class ConnectMode { Active, Passive };
    enum class State { NotConnected, NotSelected, Selected };
    enum class SType : std::uint8_t {
        Data             = 0,
        SelectRequest    = 1,
        SelectResponse   = 2,
        DeselectRequest  = 3,
        DeselectResponse = 4,
        LinktestRequest  = 5,
        LinktestResponse = 6,
        Reject           = 7,
        Separate         = 9
    };
    enum class ErrorCode {
        InvalidConfig,
        InvalidMessage,
        InvalidState,
        Unsupported,
        Backpressure,
        Transport,
        Disconnected,
        Cancelled,
        T3Timeout,
        T6Timeout,
        T7Timeout,
        T8Timeout,
        Rejected,
        SelectFailed,
        DeselectFailed,
        TransactionNotFound
    };

    struct Header {
        SessionId     session_id   = 0;
        std::uint8_t  byte2        = 0;
        std::uint8_t  byte3        = 0;
        std::uint8_t  ptype        = 0;
        std::uint8_t  stype        = 0;
        std::uint32_t system_bytes = 0;

        bool         is_data() const;
        bool         is_primary() const;
        bool         reply_expected() const;
        std::uint8_t stream() const;
        bool         operator==(const Header&) const noexcept;
    };

    struct Message {
        Header header;
        Bytes  body;
        bool   operator==(const Message&) const;
    };
    using DataMessage = Message;

    struct ErrorInfo {
        ErrorCode                   code;
        std::string                 message;
        std::error_code             transport;
        std::optional<Header>       header;
        std::optional<std::uint8_t> status;
    };

    template<class T>
    using Result          = std::expected<T, ErrorInfo>;
    using Completion      = std::function<void(Result<void>)>;
    using ReplyCompletion = std::function<void(Result<DataMessage>)>;

    ErrorInfo error(ErrorCode code, std::string message);

    struct Timeouts {
        std::chrono::seconds t3{45};
        std::chrono::seconds t5{10};
        std::chrono::seconds t6{5};
        std::chrono::seconds t7{10};
        std::chrono::seconds t8{5};
    };

    struct Config {
        Mode                   mode         = Mode::Generic;
        ConnectMode            connect_mode = ConnectMode::Active;
        std::string            address      = "127.0.0.1";
        std::uint16_t          port         = 5000;
        std::vector<SessionId> session_ids{0};
        Timeouts               timeouts;
        std::uint32_t          max_receive_length = 16 * 1024 * 1024;
        std::uint32_t          max_send_length    = 16 * 1024 * 1024;
        std::size_t            max_queued_bytes   = 32 * 1024 * 1024;
        std::size_t            max_transactions   = 1024;
        std::size_t            max_connections    = 1;
        bool                   reconnect          = true;
        std::chrono::seconds   connect_timeout{10};
        std::chrono::seconds   linktest_interval{0};
    };

    Result<void> validate_config(const Config& config);

    struct StateChange {
        State                    previous;
        State                    current;
        std::optional<ErrorInfo> reason;
    };

    struct SessionChange {
        SessionId session_id;
        bool      selected;
    };

    struct RejectInfo {
        Header       header;
        std::uint8_t rejected_type;
        std::uint8_t reason;
    };

    struct TraceEvent {
        bool        outgoing;
        Header      header;
        std::size_t body_size;
    };

} // namespace glove::e37
