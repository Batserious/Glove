#pragma once

#include "MessageCodec.h"
#include "ProtocolStateMachine.h"
#include "TransactionManager.h"

#include <array>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <deque>

namespace glove::e37 {

    class Connection;
    class Endpoint;

    struct IncomingPrimary {
        DataMessage message;

    private:
        struct Ticket {
            std::weak_ptr<Connection> owner;
            Header                    header;
            bool                      used = false;
        };
        std::shared_ptr<Ticket> ticket;
        friend class Connection;
    };

    struct Callbacks {
        std::function<void(Connection&, const StateChange&)>    on_state_changed;
        std::function<void(Connection&, const SessionChange&)>  on_session_state_changed;
        std::function<void(Connection&, IncomingPrimary)>       on_primary_received;
        std::function<void(Connection&, const ErrorInfo&)>      on_error;
        std::function<std::uint8_t(Connection&, const Header&)> on_select_requested;
        std::function<std::uint8_t(Connection&, const Header&)> on_deselect_requested;
        std::function<void(Connection&, const RejectInfo&)>     on_reject_received;
        std::function<void(Connection&, const DataMessage&)>    on_unmatched_reply;
        std::function<void(Connection&, const TraceEvent&)>     on_trace;
    };

    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        State state() const;
        void  async_session_selected(SessionId id, std::function<void(Result<bool>)> completion);
        const asio::ip::tcp::endpoint& local_endpoint() const;
        const asio::ip::tcp::endpoint& remote_endpoint() const;

        void          async_select(SessionId id, Completion completion);
        void          async_deselect(SessionId id, Completion completion);
        void          async_linktest(Completion completion);
        void          async_separate(SessionId id, Completion completion);
        void          async_send(DataMessage primary, Completion completion);
        TransactionId async_request(DataMessage primary, ReplyCompletion completion);
        void          async_reply(IncomingPrimary primary, std::uint8_t function, Bytes body, Completion completion);
        void          cancel(TransactionId id, Completion completion);
        void          async_stop(Completion completion);

    private:
        friend class Endpoint;
        Connection(asio::ip::tcp::socket socket, Config config, Callbacks callbacks, std::function<void()> on_closed);
        void         start();
        void         read();
        void         receive(Message message);
        void         control(std::uint8_t stype, SessionId id, Completion completion);
        Result<void> enqueue(Message message, Completion completion, TransactionId id = 0, bool transaction = false);
        void         write();
        void         reject(const Header& header, std::uint8_t reason);
        void         notify(std::optional<ErrorInfo> reason = {});
        void         report(const ErrorInfo& failure);
        void         close(ErrorInfo reason, bool report_error);
        void         arm_t7();
        void         arm_t8();
        void         arm_linktest();
        void         arm_shutdown();
        void         finish(Completion& completion, Result<void> result);
        void         finish(ReplyCompletion& completion, Result<DataMessage> result);

        struct Outgoing {
            Bytes         bytes;
            Header        header;
            Completion    completion;
            TransactionId id;
            bool          transaction;
        };
        asio::ip::tcp::socket                 socket_;
        Config                                config_;
        Callbacks                             callbacks_;
        std::function<void()>                 on_closed_;
        asio::ip::tcp::endpoint               local_endpoint_;
        asio::ip::tcp::endpoint               remote_endpoint_;
        MessageCodec                          codec_;
        ProtocolStateMachine                  machine_;
        std::shared_ptr<TransactionManager>   transactions_;
        asio::steady_timer                    t7_;
        asio::steady_timer                    t8_;
        asio::steady_timer                    heartbeat_;
        asio::steady_timer                    shutdown_;
        std::array<std::uint8_t, 8192>        read_buffer_{};
        std::deque<std::shared_ptr<Outgoing>> queue_;
        std::size_t                           queued_bytes_ = 0;
        std::atomic<State>                    public_state_{State::NotConnected};
        State                                 published_state_ = State::NotConnected;
        std::atomic<TransactionId>            next_id_{1};
        std::uint64_t                         t7_generation_        = 0;
        std::uint64_t                         t8_generation_        = 0;
        std::uint64_t                         heartbeat_generation_ = 0;
        std::set<SessionId>                   published_sessions_;
        std::set<SessionId>                   separating_;
        bool                                  closed_   = false;
        bool                                  writing_  = false;
        bool                                  stopping_ = false;
        std::vector<Completion>               stop_completions_;
    };

} // namespace glove::e37
