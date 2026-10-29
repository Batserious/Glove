#include <gtest/gtest.h>

#include <HSMS/Endpoint.h>
#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/write.hpp>
#include <thread>

using namespace glove::e37;
using namespace std::chrono_literals;

namespace {

    class E37Network : public testing::TestWithParam<Mode> {
    protected:
        asio::io_context            io;
        std::shared_ptr<Endpoint>   server;
        std::shared_ptr<Endpoint>   client;
        std::shared_ptr<Connection> server_connection;
        std::shared_ptr<Connection> client_connection;
        std::vector<ErrorInfo>      errors;
        std::uint16_t               port               = 0;
        int                         incoming           = 0;
        int                         client_connections = 0;
        std::vector<State>          client_states;

        bool pump(const std::function<bool()>& condition, std::chrono::milliseconds limit = 3000ms) {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (!condition() && std::chrono::steady_clock::now() < end) {
                io.restart();
                io.run_for(5ms);
            }
            return condition();
        }

        void start_server(Callbacks callbacks = {}, std::function<void(Config&)> configure = {}) {
            Config config;
            config.mode         = GetParam();
            config.connect_mode = ConnectMode::Passive;
            config.port         = 0;
            config.timeouts.t7  = 2s;
            config.timeouts.t8  = 1s;
            if (configure)
                configure(config);
            EndpointCallbacks handlers;
            handlers.on_connection       = [&](auto connection) { server_connection = std::move(connection); };
            handlers.on_error            = [&](const ErrorInfo& failure) { errors.push_back(failure); };
            handlers.connection          = std::move(callbacks);
            handlers.connection.on_error = [&](Connection&, const ErrorInfo& failure) { errors.push_back(failure); };
            if (!handlers.connection.on_primary_received) {
                handlers.connection.on_primary_received = [&](Connection& connection, IncomingPrimary primary) {
                    ++incoming;
                    if (primary.message.header.reply_expected()) {
                        auto body = primary.message.body;
                        connection.async_reply(std::move(primary), 2, std::move(body), [&](Result<void> result) {
                            if (!result)
                                errors.push_back(result.error());
                        });
                    }
                };
            }
            const auto created = Endpoint::create(io.get_executor(), config, std::move(handlers));
            ASSERT_TRUE(created);
            server = *created;
            server->async_start([&](Result<StartInfo> result) {
                ASSERT_TRUE(result);
                ASSERT_TRUE(result->listening_endpoint);
                port = result->listening_endpoint->port();
            });
            ASSERT_TRUE(pump([&] { return port != 0; }));
        }

        void start_client(Callbacks callbacks = {}, std::function<void(Config&)> configure = {}, bool select = true) {
            Config config;
            config.mode        = GetParam();
            config.port        = port;
            config.reconnect   = false;
            config.timeouts.t3 = 1s;
            config.timeouts.t6 = 1s;
            config.timeouts.t7 = 2s;
            if (configure)
                configure(config);
            EndpointCallbacks handlers;
            handlers.on_connection = [&](auto connection) {
                ++client_connections;
                client_connection = std::move(connection);
            };
            handlers.on_error            = [&](const ErrorInfo& failure) { errors.push_back(failure); };
            handlers.connection          = std::move(callbacks);
            handlers.connection.on_error = [&](Connection&, const ErrorInfo& failure) { errors.push_back(failure); };
            const auto state_callback    = handlers.connection.on_state_changed;
            handlers.connection.on_state_changed = [&, state_callback](Connection&        connection,
                                                                       const StateChange& event) {
                client_states.push_back(event.current);
                if (state_callback)
                    state_callback(connection, event);
            };
            const auto created = Endpoint::create(io.get_executor(), config, std::move(handlers));
            ASSERT_TRUE(created);
            client       = *created;
            bool started = false;
            client->async_start([&](Result<StartInfo> result) {
                EXPECT_TRUE(result);
                if (result)
                    EXPECT_FALSE(result->listening_endpoint);
                started = true;
            });
            ASSERT_TRUE(pump([&] { return started && client_connection && server_connection; }));
            if (select && config.mode == Mode::Generic) {
                bool selected = false;
                client_connection->async_select(0, [&](Result<void> result) {
                    EXPECT_TRUE(result);
                    selected = true;
                });
                ASSERT_TRUE(pump([&] { return selected; }));
            }
            if (select)
                ASSERT_TRUE(pump([&] { return client_connection->state() == State::Selected; }));
        }

        void TearDown() override {
            int remaining = 0;
            if (client) {
                ++remaining;
                client->async_stop([&](Result<void>) { --remaining; });
            }
            if (server) {
                ++remaining;
                server->async_stop([&](Result<void>) { --remaining; });
            }
            EXPECT_TRUE(pump([&] { return remaining == 0; }, 3000ms));
            io.restart();
            io.poll();
        }
    };

    TEST_P(E37Network, ConcurrentRoundTripsOneWayAndLinktest) {
        start_server();
        start_client();
        int replies = 0;
        for (int i = 0; i < 32; ++i) {
            const Bytes body{static_cast<std::uint8_t>(i)};
            client_connection->async_request({{0, 1, 1, 0, 0, 0}, body}, [&, body](Result<Message> result) {
                ASSERT_TRUE(result);
                EXPECT_EQ(result->body, body);
                EXPECT_EQ(result->header.byte3, 2);
                EXPECT_FALSE(result->header.reply_expected());
                ++replies;
            });
        }
        bool sent = false;
        client_connection->async_send({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<void> result) {
            EXPECT_TRUE(result);
            sent = true;
        });
        bool linktest = false;
        client_connection->async_linktest([&](Result<void> result) {
            EXPECT_TRUE(result);
            linktest = true;
        });
        ASSERT_TRUE(pump([&] { return replies == 32 && sent && linktest && incoming == 33; }));
        EXPECT_EQ(client_connection->state(), State::Selected);
        EXPECT_TRUE(errors.empty());
    }

    TEST_P(E37Network, T3DoesNotDisconnectAndExposesPrimaryHeader) {
        Callbacks callbacks;
        callbacks.on_primary_received = [&](Connection&, IncomingPrimary) { ++incoming; };
        start_server(std::move(callbacks));
        start_client();
        std::optional<ErrorInfo> failure;
        client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            ASSERT_FALSE(result);
            failure = result.error();
        });
        ASSERT_TRUE(pump([&] { return failure.has_value(); }));
        EXPECT_EQ(failure->code, ErrorCode::T3Timeout);
        ASSERT_TRUE(failure->header);
        EXPECT_EQ(failure->header->byte3, 1);
        EXPECT_EQ(client_connection->state(), State::Selected);
        EXPECT_EQ(server_connection->state(), State::Selected);
    }

    TEST_P(E37Network, ReplyTicketRejectsDuplicateAndWrongFunction) {
        int       duplicate_errors = 0;
        Callbacks callbacks;
        callbacks.on_primary_received = [&](Connection& connection, IncomingPrimary primary) {
            connection.async_reply(primary, 4, {}, [&](Result<void> result) {
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error().code, ErrorCode::InvalidMessage);
                ++duplicate_errors;
            });
            connection.async_reply(primary, 0, {}, [&](Result<void> result) { EXPECT_TRUE(result); });
            connection.async_reply(primary, 2, {}, [&](Result<void> result) {
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error().code, ErrorCode::InvalidMessage);
                ++duplicate_errors;
            });
        };
        start_server(std::move(callbacks));
        start_client();
        bool replied = false;
        client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            ASSERT_TRUE(result);
            EXPECT_EQ(result->header.byte3, 0);
            replied = true;
        });
        ASSERT_TRUE(pump([&] { return replied && duplicate_errors == 2; }));
    }

    TEST_P(E37Network, CancelAndCapacityFailuresCompleteOnce) {
        Callbacks callbacks;
        callbacks.on_primary_received = [](Connection&, IncomingPrimary) {};
        start_server(std::move(callbacks));
        start_client({}, [](Config& config) { config.max_transactions = 1; });
        int        callbacks_count = 0;
        const auto first = client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
            ++callbacks_count;
        });
        client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::Backpressure);
            ++callbacks_count;
        });
        client_connection->cancel(first, [&](Result<void> result) {
            EXPECT_TRUE(result);
            ++callbacks_count;
        });
        ASSERT_TRUE(pump([&] { return callbacks_count == 3; }));
    }

    TEST_P(E37Network, StoppingCancelsOutstandingAndPublishesDisconnect) {
        Callbacks callbacks;
        callbacks.on_primary_received = [&](Connection&, IncomingPrimary) { ++incoming; };
        start_server(std::move(callbacks));
        start_client();
        int completions = 0;
        client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            EXPECT_FALSE(result);
            ++completions;
        });
        ASSERT_TRUE(pump([&] { return incoming == 1; }));
        client->async_stop([&](Result<void> result) {
            EXPECT_TRUE(result);
            EXPECT_EQ(completions, 1);
            ++completions;
        });
        ASSERT_TRUE(pump([&] { return completions == 2; }));
        EXPECT_EQ(client_connection->state(), State::NotConnected);
        ASSERT_FALSE(client_states.empty());
        EXPECT_EQ(client_states.back(), State::NotConnected);
    }

    TEST_P(E37Network, DeselectIsGenericOnlyAndSeparateClosesSs) {
        start_server();
        start_client();
        bool done = false;
        client_connection->async_deselect(GetParam() == Mode::Generic ? 0 : 0xffff, [&](Result<void> result) {
            EXPECT_EQ(result.has_value(), GetParam() == Mode::Generic);
            done = true;
        });
        ASSERT_TRUE(pump([&] { return done; }));
        if (GetParam() == Mode::Generic) {
            EXPECT_EQ(client_connection->state(), State::NotSelected);
            done = false;
            client_connection->async_select(0, [&](Result<void> result) {
                EXPECT_TRUE(result);
                done = true;
            });
            ASSERT_TRUE(pump([&] { return done; }));
        }
        done = false;
        client_connection->async_separate(GetParam() == Mode::Generic ? 0 : 0xffff, [&](Result<void> result) {
            EXPECT_TRUE(result);
            done = true;
        });
        ASSERT_TRUE(pump([&] { return done; }));
        const auto expected = GetParam() == Mode::Generic ? State::NotSelected : State::NotConnected;
        ASSERT_TRUE(pump([&] { return server_connection->state() == expected; }));
        EXPECT_EQ(client_connection->state(), expected);
    }

    TEST_P(E37Network, UnselectedConnectionExpiresT7) {
        start_server({}, [](Config& config) { config.timeouts.t7 = 1s; });
        asio::ip::tcp::socket raw(io);
        raw.connect({asio::ip::make_address("127.0.0.1"), port});
        ASSERT_TRUE(pump([&] { return server_connection != nullptr; }));
        ASSERT_TRUE(pump([&] { return server_connection->state() == State::NotConnected; }));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.front().code, ErrorCode::T7Timeout);
    }

    TEST_P(E37Network, PartialLengthPrefixExpiresT8) {
        start_server();
        asio::ip::tcp::socket raw(io);
        raw.connect({asio::ip::make_address("127.0.0.1"), port});
        const Bytes partial{0, 0};
        asio::write(raw, asio::buffer(partial));
        ASSERT_TRUE(pump([&] { return server_connection != nullptr; }));
        ASSERT_TRUE(pump([&] { return server_connection->state() == State::NotConnected; }));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.front().code, ErrorCode::T8Timeout);
    }

    TEST_P(E37Network, IdleSelectedConnectionDoesNotExpireT8) {
        start_server();
        start_client();
        io.restart();
        io.run_for(1100ms);
        EXPECT_EQ(client_connection->state(), State::Selected);
        EXPECT_EQ(server_connection->state(), State::Selected);
        EXPECT_TRUE(errors.empty());
    }

    TEST_P(E37Network, QueueLimitFailsExplicitlyWithoutDisconnecting) {
        start_server();
        start_client({}, [](Config& config) { config.max_queued_bytes = 14; });
        bool failed = false;
        client_connection->async_send({{0, 1, 1, 0, 0, 0}, {1}}, [&](Result<void> result) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::Backpressure);
            failed = true;
        });
        ASSERT_TRUE(pump([&] { return failed; }));
        EXPECT_EQ(client_connection->state(), State::Selected);
    }

    TEST_P(E37Network, MultipleDeviceIdsAndSessionQuery) {
        start_server({}, [](Config& config) { config.session_ids = {0, 1}; });
        start_client({}, [](Config& config) { config.session_ids = {0, 1}; });
        if (GetParam() == Mode::Generic) {
            bool selected = false;
            server_connection->async_select(1, [&](Result<void> result) {
                EXPECT_TRUE(result);
                selected = true;
            });
            ASSERT_TRUE(pump([&] { return selected; }));
        }
        int replies = 0;
        for (SessionId id : {0, 1}) {
            client_connection->async_session_selected(id, [&](Result<bool> result) {
                ASSERT_TRUE(result);
                EXPECT_TRUE(*result);
                ++replies;
            });
            client_connection->async_request({{id, 1, 1, 0, 0, 0}, {}}, [&, id](Result<Message> result) {
                ASSERT_TRUE(result);
                EXPECT_EQ(result->header.session_id, id);
                ++replies;
            });
        }
        ASSERT_TRUE(pump([&] { return replies == 4; }));
    }

    TEST_P(E37Network, PublicCallsAreSafeFromMultipleThreads) {
        start_server();
        start_client();
        int                      replies = 0;
        std::vector<std::thread> senders;
        for (int i = 0; i < 4; ++i) {
            senders.emplace_back([&] {
                for (int n = 0; n < 16; ++n) {
                    client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
                        EXPECT_TRUE(result);
                        ++replies;
                    });
                }
            });
        }
        for (auto& sender : senders)
            sender.join();
        ASSERT_TRUE(pump([&] { return replies == 64; }));
        EXPECT_TRUE(errors.empty());
    }

    TEST_P(E37Network, ReconnectHonorsT5AndUsesNewConnectionObject) {
        start_server();
        start_client({}, [](Config& config) {
            config.reconnect   = true;
            config.timeouts.t5 = 1s;
        });
        const auto old        = client_connection;
        const auto stopped_at = std::chrono::steady_clock::now();
        bool       stopped    = false;
        server_connection->async_stop([&](Result<void> result) {
            EXPECT_TRUE(result);
            stopped = true;
        });
        ASSERT_TRUE(pump([&] { return stopped && old->state() == State::NotConnected; }));
        ASSERT_TRUE(pump([&] { return client_connections == 2; }));
        EXPECT_GE(std::chrono::steady_clock::now() - stopped_at, 1s);
        EXPECT_NE(client_connection, old);
        if (GetParam() == Mode::HsmsSS) {
            ASSERT_TRUE(pump([&] { return client_connection->state() == State::Selected; }));
        }
    }

    TEST_P(E37Network, ControlTimeoutDisconnectsWithoutAResponse) {
        start_server({}, [](Config& config) { config.timeouts.t6 = 1s; });
        asio::ip::tcp::socket raw(io);
        raw.connect({asio::ip::make_address("127.0.0.1"), port});
        ASSERT_TRUE(pump([&] { return server_connection != nullptr; }));
        Message select{{GetParam() == Mode::Generic ? SessionId{0} : SessionId{0xffff}, 0, 0, 0, 1, 1}, {}};
        auto    bytes = MessageCodec::encode(select, 10);
        ASSERT_TRUE(bytes);
        asio::write(raw, asio::buffer(*bytes));
        ASSERT_TRUE(pump([&] { return server_connection->state() == State::Selected; }));
        bool timed_out = false;
        server_connection->async_linktest([&](Result<void> result) {
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::T6Timeout);
            timed_out = true;
        });
        ASSERT_TRUE(pump([&] { return timed_out; }, 6000ms));
        EXPECT_EQ(server_connection->state(), State::NotConnected);
    }

    TEST_P(E37Network, RejectsUnsupportedTypesAndOrphanControlResponse) {
        std::vector<Header> rejects;
        Callbacks           callbacks;
        callbacks.on_trace = [&](Connection&, const TraceEvent& event) {
            if (event.outgoing && event.header.stype == 7)
                rejects.push_back(event.header);
        };
        start_server(std::move(callbacks));
        asio::ip::tcp::socket raw(io);
        raw.connect({asio::ip::make_address("127.0.0.1"), port});
        const auto session   = GetParam() == Mode::Generic ? SessionId{0} : SessionId{0xffff};
        const auto selection = MessageCodec::encode({{session, 0, 0, 0, 1, 1}, {}}, 10);
        ASSERT_TRUE(selection);
        asio::write(raw, asio::buffer(*selection));
        ASSERT_TRUE(pump([&] { return server_connection && server_connection->state() == State::Selected; }));
        const std::vector<Message> invalid{
                {{session, 0, 0, 0, 42, 10}, {}}, {{0, 1, 1, 42, 0, 11}, {}}, {{0xffff, 0, 0, 0, 6, 12}, {}}};
        for (const auto& message : invalid) {
            auto bytes = MessageCodec::encode(message, 10);
            ASSERT_TRUE(bytes);
            asio::write(raw, asio::buffer(*bytes));
        }
        ASSERT_TRUE(pump([&] { return rejects.size() == 3; }));
        EXPECT_EQ(rejects[0].byte3, 1);
        EXPECT_EQ(rejects[0].byte2, 42);
        EXPECT_EQ(rejects[1].byte3, 2);
        EXPECT_EQ(rejects[1].byte2, 42);
        EXPECT_EQ(rejects[2].byte3, 3);
        for (std::size_t i = 0; i < rejects.size(); ++i) {
            EXPECT_EQ(rejects[i].system_bytes, 10 + i);
        }
        EXPECT_EQ(server_connection->state(), State::Selected);
    }

    TEST_P(E37Network, PassiveConnectionLimitResumesAcceptAfterDisconnect) {
        start_server();
        asio::ip::tcp::socket first(io);
        first.connect({asio::ip::make_address("127.0.0.1"), port});
        ASSERT_TRUE(pump([&] { return server_connection != nullptr; }));
        const auto            old = server_connection;
        asio::ip::tcp::socket second(io);
        second.connect({asio::ip::make_address("127.0.0.1"), port});
        io.restart();
        io.run_for(20ms);
        EXPECT_EQ(server_connection, old);
        first.close();
        ASSERT_TRUE(pump([&] { return server_connection != old; }));
        EXPECT_EQ(old->state(), State::NotConnected);
    }

    TEST_P(E37Network, SelectRefusalReturnsStatusAndSsDisconnects) {
        Callbacks callbacks;
        callbacks.on_select_requested = [](Connection&, const Header&) { return std::uint8_t{2}; };
        start_server(std::move(callbacks));
        start_client({}, {}, false);
        if (GetParam() == Mode::Generic) {
            bool failed = false;
            client_connection->async_select(0, [&](Result<void> result) {
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error().code, ErrorCode::SelectFailed);
                EXPECT_EQ(result.error().status, 2);
                failed = true;
            });
            ASSERT_TRUE(pump([&] { return failed; }));
            EXPECT_EQ(client_connection->state(), State::NotSelected);
        }
        else {
            ASSERT_TRUE(pump([&] { return client_connection->state() == State::NotConnected; }));
            EXPECT_TRUE(std::ranges::any_of(errors, [](const auto& failure) {
                return failure.code == ErrorCode::SelectFailed && failure.status == 2;
            }));
        }
    }

    TEST_P(E37Network, SeparateCompletesBeforeEndpointStop) {
        start_server();
        start_client();
        std::vector<std::string> completed;
        client_connection->async_separate(GetParam() == Mode::Generic ? 0 : 0xffff, [&](Result<void> result) {
            EXPECT_TRUE(result);
            completed.emplace_back("separate");
        });
        client->async_stop([&](Result<void> result) {
            EXPECT_TRUE(result);
            EXPECT_EQ(completed, std::vector<std::string>{"separate"});
            completed.emplace_back("stop");
        });
        ASSERT_TRUE(pump([&] { return completed.size() == 2; }));
    }

    TEST_P(E37Network, RestartDiscardsCancelledGenerationHandlers) {
        start_server();
        start_client({}, [](Config& config) { config.timeouts.t5 = 1s; });
        const auto old       = client_connection;
        bool       restarted = false;
        client->async_stop([&](Result<void> result) {
            EXPECT_TRUE(result);
            client->async_start([&](Result<StartInfo> started) {
                EXPECT_TRUE(started);
                restarted = true;
            });
        });
        ASSERT_TRUE(pump([&] { return restarted && client_connections == 2; }));
        EXPECT_NE(client_connection, old);
        EXPECT_EQ(old->state(), State::NotConnected);
        if (GetParam() == Mode::HsmsSS) {
            ASSERT_TRUE(pump([&] { return client_connection->state() == State::Selected; }));
        }
    }

    TEST_P(E37Network, SimultaneousGenericProceduresAndSsRestrictions) {
        start_server();
        start_client({}, {}, false);
        if (GetParam() == Mode::HsmsSS) {
            ASSERT_TRUE(pump([&] { return client_connection->state() == State::Selected; }));
            bool denied = false;
            server_connection->async_select(0xffff, [&](Result<void> result) {
                ASSERT_FALSE(result);
                EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
                denied = true;
            });
            ASSERT_TRUE(pump([&] { return denied; }));
            return;
        }
        int        completed = 0;
        const auto done      = [&](Result<void> result) {
            EXPECT_TRUE(result);
            ++completed;
        };
        server_connection->async_select(0, done);
        client_connection->async_select(0, done);
        ASSERT_TRUE(pump([&] { return completed == 2; }));
        EXPECT_EQ(client_connection->state(), State::Selected);
        EXPECT_EQ(server_connection->state(), State::Selected);
        server_connection->async_deselect(0, done);
        client_connection->async_deselect(0, done);
        ASSERT_TRUE(pump([&] { return completed == 4; }));
        EXPECT_EQ(client_connection->state(), State::NotSelected);
        EXPECT_EQ(server_connection->state(), State::NotSelected);
    }

    TEST_P(E37Network, EndingSessionCompletesPendingDataWithoutWaitingForT3) {
        Callbacks callbacks;
        callbacks.on_primary_received = [&](Connection&, IncomingPrimary) { ++incoming; };
        start_server(std::move(callbacks));
        start_client();
        bool failed = false;
        client_connection->async_request({{0, 1, 1, 0, 0, 0}, {}}, [&](Result<Message> result) {
            EXPECT_FALSE(result);
            failed = true;
        });
        ASSERT_TRUE(pump([&] { return incoming == 1; }));
        bool       ended      = false;
        const auto completion = [&](Result<void> result) {
            EXPECT_TRUE(result);
            ended = true;
        };
        if (GetParam() == Mode::Generic)
            client_connection->async_deselect(0, completion);
        else
            client_connection->async_separate(0xffff, completion);
        ASSERT_TRUE(pump([&] { return ended && failed; }, 500ms));
    }

    INSTANTIATE_TEST_SUITE_P(Profiles, E37Network, testing::Values(Mode::Generic, Mode::HsmsSS));

} // namespace
