#include <HSMS/ProtocolStateMachine.h>

namespace glove::e37 {

    ProtocolStateMachine::ProtocolStateMachine(const Config& config) :
        mode_(config.mode), connect_mode_(config.connect_mode),
        supported_(config.session_ids.begin(), config.session_ids.end()) {}

    State ProtocolStateMachine::state() const {
        return !connected_ ? State::NotConnected : selected_.empty() ? State::NotSelected : State::Selected;
    }

    const std::set<SessionId>& ProtocolStateMachine::selected_sessions() const { return selected_; }

    bool ProtocolStateMachine::supports(SessionId id) const { return supported_.contains(id); }

    bool ProtocolStateMachine::session_selected(SessionId id) const {
        return supports(id) && (mode_ == Mode::HsmsSS ? !selected_.empty() : selected_.contains(id));
    }

    void ProtocolStateMachine::connected() { connected_ = true; }

    void ProtocolStateMachine::disconnected() {
        connected_ = false;
        selected_.clear();
        selecting_.clear();
        deselecting_.clear();
    }

    Result<void> ProtocolStateMachine::validate_outgoing(const Header& h) const {
        if (!connected_) {
            return std::unexpected(error(ErrorCode::InvalidState, "TCP is not connected"));
        }
        if (h.ptype != 0 || (h.is_data() && !supports(h.session_id))) {
            return std::unexpected(error(ErrorCode::Unsupported, "Unsupported PType or Session ID"));
        }
        if (h.is_data()) {
            if (!session_selected(h.session_id)) {
                return std::unexpected(error(ErrorCode::InvalidState, "Session is not selected"));
            }
            return {};
        }
        if (mode_ == Mode::HsmsSS && h.session_id != 0xffff) {
            return std::unexpected(error(ErrorCode::InvalidMessage, "HSMS-SS controls require Session ID 0xffff"));
        }
        if (h.stype == 1) {
            if (selecting_.contains(h.session_id) || deselecting_.contains(h.session_id) ||
                (mode_ == Mode::HsmsSS && (connect_mode_ != ConnectMode::Active || state() != State::NotSelected))) {
                return std::unexpected(error(ErrorCode::InvalidState, "Select is not permitted now"));
            }
            if (mode_ == Mode::Generic && !supports(h.session_id)) {
                return std::unexpected(error(ErrorCode::Unsupported, "Unsupported Session ID"));
            }
        }
        else if (h.stype == 3) {
            if (mode_ == Mode::HsmsSS || !session_selected(h.session_id) || deselecting_.contains(h.session_id) ||
                selecting_.contains(h.session_id)) {
                return std::unexpected(error(ErrorCode::InvalidState, "Deselect is not permitted now"));
            }
        }
        else if (h.stype == 5) {
            if (mode_ == Mode::HsmsSS && state() != State::Selected) {
                return std::unexpected(error(ErrorCode::InvalidState, "HSMS-SS Linktest requires Selected"));
            }
        }
        else if (h.stype == 9) {
            if (mode_ == Mode::Generic && !session_selected(h.session_id)) {
                return std::unexpected(error(ErrorCode::InvalidState, "Separate requires a selected session"));
            }
        }
        else {
            return std::unexpected(error(ErrorCode::Unsupported, "Not an application-initiated control request"));
        }
        return {};
    }

    Result<void> ProtocolStateMachine::validate_incoming(const Header& h) const {
        if (!connected_) {
            return std::unexpected(error(ErrorCode::InvalidState, "Message received without TCP connection"));
        }
        if (mode_ == Mode::HsmsSS) {
            if ((h.is_data() && h.session_id > 0x7fff) || (!h.is_data() && h.session_id != 0xffff)) {
                return std::unexpected(error(ErrorCode::InvalidMessage, "Invalid HSMS-SS Session ID"));
            }
            if (state() == State::NotSelected) {
                const auto expected = connect_mode_ == ConnectMode::Active ? 2 : 1;
                if (h.stype != expected && h.stype != 9) {
                    return std::unexpected(
                            error(ErrorCode::InvalidState, "Unexpected message during HSMS-SS selection"));
                }
            }
            else if (h.stype == 1 || h.stype == 2) {
                return std::unexpected(
                        error(ErrorCode::InvalidState, "HSMS-SS Select is only allowed before selection"));
            }
            if (h.stype == 3 || h.stype == 4) {
                return std::unexpected(error(ErrorCode::Unsupported, "HSMS-SS forbids Deselect"));
            }
        }
        if (h.is_data() && !supports(h.session_id)) {
            return std::unexpected(error(ErrorCode::Unsupported, "Unconfigured data Session ID"));
        }
        return {};
    }

    std::uint8_t ProtocolStateMachine::reject_reason(const Header& h) const {
        switch (static_cast<SType>(h.stype)) {
            case SType::Data:
            case SType::SelectRequest:
            case SType::SelectResponse:
            case SType::DeselectRequest:
            case SType::DeselectResponse:
            case SType::LinktestRequest:
            case SType::LinktestResponse:
            case SType::Reject:
            case SType::Separate:
                break;
            default:
                return 1;
        }
        if (h.ptype != 0) {
            return 2;
        }
        if (h.is_data() && !session_selected(h.session_id)) {
            return 4;
        }
        return 0;
    }

    void ProtocolStateMachine::request_started(const Header& h) {
        if (h.stype == 1)
            selecting_.insert(h.session_id);
        if (h.stype == 3)
            deselecting_.insert(h.session_id);
    }

    void ProtocolStateMachine::request_finished(const Header& h) {
        if (h.stype == 1)
            selecting_.erase(h.session_id);
        if (h.stype == 3)
            deselecting_.erase(h.session_id);
    }

    Header ProtocolStateMachine::respond(const Header& request, std::uint8_t policy_status) {
        auto response = request;
        ++response.stype;
        response.byte3 = 0;
        if (request.stype == 1) {
            if (selecting_.contains(request.session_id)) {
                response.byte3 = 0;
            }
            else if (selected_.contains(request.session_id)) {
                response.byte3 = 1;
            }
            else if (mode_ == Mode::Generic && !supports(request.session_id)) {
                response.byte3 = 2;
            }
            else {
                response.byte3 = policy_status;
            }
            if (response.byte3 == 0)
                selected_.insert(request.session_id);
        }
        else if (request.stype == 3) {
            response.byte3 = deselecting_.contains(request.session_id) ? 0
                             : !selected_.contains(request.session_id) ? 1
                                                                       : policy_status;
            if (response.byte3 == 0)
                selected_.erase(request.session_id);
        }
        return response;
    }

    void ProtocolStateMachine::response_received(const Header& request, const Header& response) {
        if (response.byte3 == 0) {
            if (request.stype == 1)
                selected_.insert(request.session_id);
            if (request.stype == 3)
                selected_.erase(request.session_id);
        }
        request_finished(request);
    }

    void ProtocolStateMachine::separated(SessionId id) { selected_.erase(id); }

} // namespace glove::e37
