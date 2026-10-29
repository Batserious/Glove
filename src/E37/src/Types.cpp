#include <HSMS/Types.h>

#include <algorithm>
#include <set>
#include <utility>

namespace glove::e37 {

    bool Header::is_data() const { return stype == 0; }

    bool Header::is_primary() const { return is_data() && (byte3 & 1) != 0; }

    bool Header::reply_expected() const { return (byte2 & 0x80) != 0; }

    std::uint8_t Header::stream() const { return byte2 & 0x7f; }

    bool Header::operator==(const Header&) const noexcept = default;

    bool Message::operator==(const Message&) const = default;

    ErrorInfo error(ErrorCode code, std::string message) { return {code, std::move(message), {}, {}, {}}; }

    Result<void> validate_config(const Config& config) {
        const auto  valid_time = [](auto value, int maximum) { return value.count() >= 1 && value.count() <= maximum; };
        const auto& t          = config.timeouts;
        if (!valid_time(t.t3, 120) || !valid_time(t.t5, 240) || !valid_time(t.t6, 240) || !valid_time(t.t7, 240) ||
            !valid_time(t.t8, 120)) {
            return std::unexpected(error(ErrorCode::InvalidConfig, "T3/T8 must be 1..120s; T5/T6/T7 must be 1..240s"));
        }
        if (config.address.empty() || (config.connect_mode == ConnectMode::Active && config.port == 0) ||
            config.connect_timeout.count() <= 0 || config.linktest_interval.count() < 0) {
            return std::unexpected(
                    error(ErrorCode::InvalidConfig, "Invalid address, port or connection/heartbeat interval"));
        }
        if (config.max_receive_length < 10 || config.max_send_length < 10 || config.max_queued_bytes < 14 ||
            config.max_transactions == 0 || config.max_connections == 0) {
            return std::unexpected(
                    error(ErrorCode::InvalidConfig, "Invalid message, queue, transaction or connection limit"));
        }
        if (config.session_ids.empty() ||
            std::set<SessionId>(config.session_ids.begin(), config.session_ids.end()).size() !=
                    config.session_ids.size()) {
            return std::unexpected(error(ErrorCode::InvalidConfig, "Session IDs must be nonempty and unique"));
        }
        if (config.mode == Mode::HsmsSs &&
            std::ranges::any_of(config.session_ids, [](SessionId id) { return id > 0x7fff; })) {
            return std::unexpected(error(ErrorCode::InvalidConfig, "HSMS-SS Device IDs must fit in 15 bits"));
        }
        return {};
    }

} // namespace glove::e37
