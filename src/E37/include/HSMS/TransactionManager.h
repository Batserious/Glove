#pragma once

#include "Types.h"

#include <asio/any_io_executor.hpp>
#include <asio/steady_timer.hpp>
#include <map>
#include <memory>
#include <set>

namespace glove::e37 {

    class TransactionManager : public std::enable_shared_from_this<TransactionManager> {
    public:
        TransactionManager(asio::any_io_executor executor, Timeouts timeouts, std::size_t limit);
        Result<std::uint32_t> allocate_system_bytes();
        void                  release_system_bytes(std::uint32_t system_bytes);
        Result<void>          add(TransactionId id, const Header& request, ReplyCompletion completion);
        void                  write_started(std::uint32_t system_bytes);
        void                  written(std::uint32_t system_bytes);
        bool                  receive(const Message& response, Mode mode);
        bool                  fail(std::uint32_t system_bytes, ErrorInfo failure);
        bool                  cancel(TransactionId id);
        void                  cancel_all(const ErrorInfo& failure);
        void                  cancel_data(SessionId id, Mode mode, const ErrorInfo& failure);
        std::size_t           size() const;

    private:
        struct Pending {
            Pending(asio::any_io_executor executor, TransactionId token, Header header, ReplyCompletion callback);
            asio::steady_timer timer;
            TransactionId      id;
            Header             request;
            ReplyCompletion    completion;
            bool               started = false;
        };
        void                                              finish(std::uint32_t system_bytes, Result<Message> result);
        asio::any_io_executor                             executor_;
        Timeouts                                          timeouts_;
        std::size_t                                       limit_;
        std::uint32_t                                     next_ = 0;
        std::optional<std::uint32_t>                      last_completed_;
        std::set<std::uint32_t>                           reserved_;
        std::map<std::uint32_t, std::shared_ptr<Pending>> entries_;
    };

} // namespace glove::e37
