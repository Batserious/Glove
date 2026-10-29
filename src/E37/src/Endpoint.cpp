#include <HSMS/Endpoint.h>

#include <asio/connect.hpp>
#include <asio/post.hpp>
#include <spdlog/spdlog.h>

namespace glove::e37 {

    Result<std::shared_ptr<Endpoint>> Endpoint::create(asio::any_io_executor executor, Config config,
                                                       EndpointCallbacks callbacks) {
        if (auto valid = validate_config(config); !valid)
            return std::unexpected(valid.error());
        return std::shared_ptr<Endpoint>(new Endpoint(std::move(executor), std::move(config), std::move(callbacks)));
    }

    Endpoint::Endpoint(asio::any_io_executor executor, Config config, EndpointCallbacks callbacks) :
        executor_(asio::make_strand(std::move(executor))), config_(std::move(config)), callbacks_(std::move(callbacks)),
        acceptor_(executor_), resolver_(executor_), retry_(executor_), connect_deadline_(executor_) {}

    void Endpoint::report(const ErrorInfo& failure) {
        if (callbacks_.on_error)
            callbacks_.on_error(failure);
        else
            spdlog::error("E37 endpoint: {} ({})", failure.message, failure.transport.message());
    }

    void Endpoint::async_start(std::function<void(Result<StartInfo>)> completion) {
        asio::post(executor_, [self = shared_from_this(), completion = std::move(completion)]() mutable {
            const auto done = [&self, &completion](Result<StartInfo> result) {
                if (completion)
                    completion(std::move(result));
                else if (!result)
                    self->report(result.error());
            };
            if (self->running_ || self->stopping_) {
                done(std::unexpected(error(ErrorCode::InvalidState, "Endpoint is already running or stopping")));
                return;
            }
            ++self->generation_;
            if (self->config_.connect_mode == ConnectMode::Passive) {
                std::error_code ec;
                const auto      address = asio::ip::make_address(self->config_.address, ec);
                if (ec) {
                    auto failure      = error(ErrorCode::InvalidConfig, "Passive address must be an IP literal");
                    failure.transport = ec;
                    done(std::unexpected(std::move(failure)));
                    return;
                }
                const asio::ip::tcp::endpoint address_port(address, self->config_.port);
                self->acceptor_.open(address_port.protocol(), ec);
                if (!ec)
                    self->acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
                if (!ec)
                    self->acceptor_.bind(address_port, ec);
                if (!ec)
                    self->acceptor_.listen(asio::socket_base::max_listen_connections, ec);
                auto bound = ec ? asio::ip::tcp::endpoint{} : self->acceptor_.local_endpoint(ec);
                if (ec) {
                    auto failure      = error(ErrorCode::Transport, "Could not start TCP listener");
                    failure.transport = ec;
                    std::error_code close_error;
                    self->acceptor_.close(close_error);
                    if (close_error) {
                        auto cleanup      = error(ErrorCode::Transport, "Listener cleanup failed");
                        cleanup.transport = close_error;
                        self->report(cleanup);
                    }
                    done(std::unexpected(std::move(failure)));
                    return;
                }
                self->running_ = true;
                self->accept();
                done(StartInfo{ConnectMode::Passive, bound});
            }
            else {
                self->running_ = true;
                self->schedule_reconnect();
                // Active startup acknowledges scheduling, not establishment of a connection.
                done(StartInfo{ConnectMode::Active, std::nullopt});
            }
        });
    }

    void Endpoint::accept() {
        if (!running_ || accepting_ || connections_.size() >= config_.max_connections)
            return;
        accepting_ = true;
        acceptor_.async_accept(executor_, [self       = shared_from_this(),
                                           generation = generation_](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!self->running_ || generation != self->generation_)
                return;
            self->accepting_ = false;
            if (ec) {
                auto failure      = error(ErrorCode::Transport, "TCP accept failed");
                failure.transport = ec;
                self->report(failure);
                self->retry_.expires_after(self->config_.timeouts.t5);
                self->retry_.async_wait([self, generation](std::error_code timer_error) {
                    if (timer_error == asio::error::operation_aborted || !self->running_ ||
                        generation != self->generation_)
                        return;
                    if (timer_error) {
                        auto failure      = error(ErrorCode::Transport, "Accept retry timer failed");
                        failure.transport = timer_error;
                        self->report(failure);
                        return;
                    }
                    self->accept();
                });
                return;
            }
            self->adopt(std::move(socket));
            self->accept();
        });
    }

    void Endpoint::adopt(asio::ip::tcp::socket socket) {
        const auto id         = next_connection_++;
        auto       connection = std::shared_ptr<Connection>(
                new Connection(std::move(socket), config_, callbacks_.connection, [weak = weak_from_this(), id] {
                    if (auto self = weak.lock())
                        self->connection_closed(id);
                }));
        connections_.emplace(id, connection);
        if (callbacks_.on_connection)
            callbacks_.on_connection(connection);
        connection->start();
    }

    void Endpoint::connect() {
        if (!running_ || connecting_ || !connections_.empty())
            return;
        const auto generation = generation_;
        connecting_           = std::make_shared<asio::ip::tcp::socket>(executor_);
        const auto socket     = connecting_;
        connect_deadline_.expires_after(config_.connect_timeout);
        connect_deadline_.async_wait([self = shared_from_this(), generation, socket](std::error_code ec) {
            if (ec == asio::error::operation_aborted || !self->running_ || generation != self->generation_ ||
                self->connecting_ != socket)
                return;
            self->resolver_.cancel();
            std::error_code close_error;
            socket->close(close_error);
            self->connecting_.reset();
            self->next_connect_ = std::chrono::steady_clock::now() + self->config_.timeouts.t5;
            auto failure        = error(ErrorCode::Transport, "TCP resolve/connect deadline expired");
            failure.transport   = ec ? ec : close_error;
            self->report(failure);
            if (self->config_.reconnect)
                self->schedule_reconnect();
        });
        resolver_.async_resolve(
                config_.address, std::to_string(config_.port),
                [self = shared_from_this(), generation, socket](std::error_code                       ec,
                                                                asio::ip::tcp::resolver::results_type results) {
                    if (!self->running_ || generation != self->generation_ || self->connecting_ != socket)
                        return;
                    const auto connected = [self, generation, socket](std::error_code result,
                                                                      const asio::ip::tcp::endpoint&) {
                        if (!self->running_ || generation != self->generation_ || self->connecting_ != socket)
                            return;
                        self->connect_deadline_.cancel();
                        self->connecting_.reset();
                        self->next_connect_ = std::chrono::steady_clock::now() + self->config_.timeouts.t5;
                        if (result) {
                            auto failure      = error(ErrorCode::Transport, "TCP resolve/connect failed");
                            failure.transport = result;
                            self->report(failure);
                            if (self->config_.reconnect)
                                self->schedule_reconnect();
                        }
                        else {
                            self->adopt(std::move(*socket));
                        }
                    };
                    if (ec)
                        connected(ec, {});
                    else
                        asio::async_connect(*socket, results, connected);
                });
    }

    void Endpoint::schedule_reconnect() {
        if (!running_)
            return;
        retry_.expires_at(std::max(next_connect_, std::chrono::steady_clock::now()));
        retry_.async_wait([self = shared_from_this(), generation = generation_](std::error_code ec) {
            if (ec == asio::error::operation_aborted || !self->running_ || generation != self->generation_)
                return;
            if (ec) {
                auto failure      = error(ErrorCode::Transport, "Connect retry timer failed");
                failure.transport = ec;
                self->report(failure);
                return;
            }
            self->connect();
        });
    }

    void Endpoint::connection_closed(std::uint64_t id) {
        connections_.erase(id);
        if (stopping_) {
            finish_stop();
        }
        else if (running_ && config_.connect_mode == ConnectMode::Active && config_.reconnect) {
            next_connect_ = std::max(next_connect_, std::chrono::steady_clock::now() + config_.timeouts.t5);
            schedule_reconnect();
        }
        else if (running_ && config_.connect_mode == ConnectMode::Passive) {
            accept();
        }
    }

    void Endpoint::async_stop(Completion completion) {
        asio::post(executor_, [self = shared_from_this(), completion = std::move(completion)]() mutable {
            self->stop_completions_.push_back(std::move(completion));
            if (self->stopping_)
                return;
            self->stopping_  = true;
            self->running_   = false;
            self->accepting_ = false;
            ++self->generation_;
            self->retry_.cancel();
            self->connect_deadline_.cancel();
            self->resolver_.cancel();
            std::error_code ec;
            self->acceptor_.close(ec);
            if (ec) {
                self->stop_error_            = error(ErrorCode::Transport, "Listener close failed");
                self->stop_error_->transport = ec;
            }
            if (self->connecting_) {
                self->connecting_->close(ec);
                if (ec) {
                    self->stop_error_            = error(ErrorCode::Transport, "Connecting socket close failed");
                    self->stop_error_->transport = ec;
                }
                self->connecting_.reset();
                self->next_connect_ = std::chrono::steady_clock::now() + self->config_.timeouts.t5;
            }
            for (const auto& [id, connection] : self->connections_) {
                (void)id;
                connection->async_stop([weak = self->weak_from_this()](Result<void> result) {
                    if (auto endpoint = weak.lock(); endpoint && !result)
                        endpoint->stop_error_ = result.error();
                });
            }
            self->finish_stop();
        });
    }

    void Endpoint::finish_stop() {
        if (!stopping_ || !connections_.empty())
            return;
        stopping_              = false;
        auto       completions = std::exchange(stop_completions_, {});
        const auto failure     = std::exchange(stop_error_, {});
        for (auto& completion : completions) {
            if (completion)
                completion(failure ? Result<void>{std::unexpected(*failure)} : Result<void>{});
            else if (failure)
                report(*failure);
        }
    }

} // namespace glove::e37
