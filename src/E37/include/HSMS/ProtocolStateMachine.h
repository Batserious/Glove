#pragma once

#include "Types.h"

#include <set>

namespace glove::e37 {

    class ProtocolStateMachine {
    public:
        explicit ProtocolStateMachine(const Config& config);
        State                      state() const;
        const std::set<SessionId>& selected_sessions() const;
        bool                       session_selected(SessionId id) const;
        void                       connected();
        void                       disconnected();
        Result<void>               validate_outgoing(const Header& header) const;
        Result<void>               validate_incoming(const Header& header) const;
        std::uint8_t               reject_reason(const Header& header) const;
        void                       request_started(const Header& header);
        void                       request_finished(const Header& header);
        Header                     respond(const Header& request, std::uint8_t policy_status = 0);
        void                       response_received(const Header& request, const Header& response);
        void                       separated(SessionId id);

    private:
        bool                supports(SessionId id) const;
        Mode                mode_;
        ConnectMode         connect_mode_;
        std::set<SessionId> supported_;
        std::set<SessionId> selected_;
        std::set<SessionId> selecting_;
        std::set<SessionId> deselecting_;
        bool                connected_ = false;
    };

} // namespace glove::e37
