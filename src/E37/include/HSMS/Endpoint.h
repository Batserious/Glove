#pragma once

#include "Connection.h"

#include <asio/ip/tcp.hpp>
#include <asio/strand.hpp>
#include <map>

namespace glove::e37 {

    struct EndpointCallbacks {
        std::function<void(std::shared_ptr<Connection>)> on_connection;
        std::function<void(const ErrorInfo&)>            on_error;
        Callbacks                                        connection;
    };

    struct StartInfo {
        ConnectMode                            connect_mode;
        std::optional<asio::ip::tcp::endpoint> listening_endpoint;
    };

    class Endpoint : public std::enable_shared_from_this<Endpoint> {
    public:
        static Result<std::shared_ptr<Endpoint>> create(asio::any_io_executor executor, Config config,
                                                        EndpointCallbacks callbacks = {});
        void                                     async_start(std::function<void(Result<StartInfo>)> completion);
        void                                     async_stop(Completion completion);

    private:
        Endpoint(asio::any_io_executor executor, Config config, EndpointCallbacks callbacks);
        void accept();
        void connect();
        void schedule_reconnect();
        void adopt(asio::ip::tcp::socket socket);
        void connection_closed(std::uint64_t id);
        void report(const ErrorInfo& failure);
        void finish_stop();

        asio::strand<asio::any_io_executor>                  executor_;
        Config                                               config_;
        EndpointCallbacks                                    callbacks_;
        asio::ip::tcp::acceptor                              acceptor_;
        asio::ip::tcp::resolver                              resolver_;
        asio::steady_timer                                   retry_;
        asio::steady_timer                                   connect_deadline_;
        std::shared_ptr<asio::ip::tcp::socket>               connecting_;
        std::map<std::uint64_t, std::shared_ptr<Connection>> connections_;
        std::chrono::steady_clock::time_point                next_connect_{};
        std::uint64_t                                        next_connection_ = 1;
        std::uint64_t                                        generation_      = 0;
        bool                                                 running_         = false;
        bool                                                 stopping_        = false;
        bool                                                 accepting_       = false;
        std::vector<Completion>                              stop_completions_;
        std::optional<ErrorInfo>                             stop_error_;
    };

} // namespace glove::e37
