#include <HSMS/Connection.h>

#include <asio/post.hpp>
#include <asio/write.hpp>
#include <spdlog/spdlog.h>

namespace glove::e37 {

    Connection::Connection(asio::ip::tcp::socket socket, Config config, Callbacks callbacks,
                           std::function<void()> on_closed) :
        socket_(std::move(socket)), config_(std::move(config)), callbacks_(std::move(callbacks)),
        on_closed_(std::move(on_closed)), local_endpoint_(socket_.local_endpoint()),
        remote_endpoint_(socket_.remote_endpoint()), codec_(config_.max_receive_length), machine_(config_),
        transactions_(std::make_shared<TransactionManager>(socket_.get_executor(), config_.timeouts,
                                                           config_.max_transactions)),
        t7_(socket_.get_executor()), t8_(socket_.get_executor()), heartbeat_(socket_.get_executor()),
        shutdown_(socket_.get_executor()) {}

    State Connection::state() const { return public_state_.load(); }

    const asio::ip::tcp::endpoint& Connection::local_endpoint() const { return local_endpoint_; }

    const asio::ip::tcp::endpoint& Connection::remote_endpoint() const { return remote_endpoint_; }

    void Connection::finish(Completion& completion, Result<void> result) {
        if (completion)
            completion(std::move(result));
        else if (!result)
            report(result.error());
    }

    void Connection::finish(ReplyCompletion& completion, Result<DataMessage> result) {
        if (completion)
            completion(std::move(result));
        else if (!result)
            report(result.error());
    }

    void Connection::report(const ErrorInfo& failure) {
        if (callbacks_.on_error)
            callbacks_.on_error(*this, failure);
        else
            spdlog::error("E37: {} ({})", failure.message, failure.transport.message());
    }

    void Connection::start() {
        machine_.connected();
        notify();
        read();
        if (config_.mode == Mode::HsmsSs && config_.connect_mode == ConnectMode::Active) {
            control(1, 0xffff, {});
        }
    }

    void Connection::notify(std::optional<ErrorInfo> reason) {
        const auto previous = std::exchange(published_state_, machine_.state());
        const auto current  = machine_.state();
        public_state_.store(current);
        const auto sessions     = machine_.selected_sessions();
        const auto old_sessions = std::exchange(published_sessions_, sessions);
        if (previous != current) {
            ++t7_generation_;
            ++heartbeat_generation_;
            t7_.cancel();
            heartbeat_.cancel();
            if (current == State::NotSelected &&
                (config_.mode == Mode::Generic || config_.connect_mode == ConnectMode::Passive))
                arm_t7();
            if (current == State::Selected)
                arm_linktest();
            if (callbacks_.on_state_changed)
                callbacks_.on_state_changed(*this, {previous, current, reason});
        }
        if (callbacks_.on_session_state_changed) {
            for (auto id : old_sessions) {
                if (!sessions.contains(id))
                    callbacks_.on_session_state_changed(*this, {id, false});
            }
            for (auto id : sessions) {
                if (!old_sessions.contains(id))
                    callbacks_.on_session_state_changed(*this, {id, true});
            }
        }
        for (auto id : old_sessions) {
            if (!sessions.contains(id)) {
                transactions_->cancel_data(
                        id, config_.mode,
                        reason.value_or(error(ErrorCode::InvalidState, "Session is no longer selected")));
            }
        }
    }

    void Connection::arm_t7() {
        t7_.expires_after(config_.timeouts.t7);
        t7_.async_wait([self = shared_from_this(), generation = t7_generation_](std::error_code ec) {
            if (ec == asio::error::operation_aborted || self->closed_ || generation != self->t7_generation_)
                return;
            if (ec) {
                auto failure      = error(ErrorCode::Transport, "T7 timer failed");
                failure.transport = ec;
                self->close(std::move(failure), true);
            }
            else {
                self->close(error(ErrorCode::T7Timeout, "T7 selection timeout"), true);
            }
        });
    }

    void Connection::arm_t8() {
        ++t8_generation_;
        t8_.cancel();
        if (!codec_.has_partial_message())
            return;
        t8_.expires_after(config_.timeouts.t8);
        t8_.async_wait([self = shared_from_this(), generation = t8_generation_](std::error_code ec) {
            if (ec == asio::error::operation_aborted || self->closed_ || generation != self->t8_generation_)
                return;
            if (ec) {
                auto failure      = error(ErrorCode::Transport, "T8 timer failed");
                failure.transport = ec;
                self->close(std::move(failure), true);
            }
            else {
                self->close(error(ErrorCode::T8Timeout, "T8 incomplete message timeout"), true);
            }
        });
    }

    void Connection::arm_linktest() {
        if (config_.linktest_interval.count() == 0 || closed_ || stopping_ || machine_.state() != State::Selected)
            return;
        heartbeat_.expires_after(config_.linktest_interval);
        heartbeat_.async_wait([self = shared_from_this(), generation = heartbeat_generation_](std::error_code ec) {
            if (ec == asio::error::operation_aborted || self->closed_ || self->stopping_ ||
                generation != self->heartbeat_generation_)
                return;
            if (ec) {
                auto failure      = error(ErrorCode::Transport, "Linktest timer failed");
                failure.transport = ec;
                self->close(std::move(failure), true);
                return;
            }
            self->control(5, 0xffff, [weak = self->weak_from_this()](Result<void> result) {
                if (auto connection = weak.lock(); connection && !connection->closed_) {
                    if (!result)
                        connection->report(result.error());
                    connection->arm_linktest();
                }
            });
        });
    }

    void Connection::read() {
        socket_.async_read_some(asio::buffer(read_buffer_),
                                [self = shared_from_this()](std::error_code ec, std::size_t n) {
                                    if (self->closed_)
                                        return;
                                    if (ec) {
                                        auto failure      = error(ErrorCode::Transport, "TCP receive failed");
                                        failure.transport = ec;
                                        self->close(std::move(failure), true);
                                        return;
                                    }
                                    auto messages = self->codec_.feed(std::span(self->read_buffer_.data(), n));
                                    if (!messages) {
                                        self->close(messages.error(), true);
                                        return;
                                    }
                                    self->arm_t8();
                                    for (auto& message : *messages) {
                                        self->receive(std::move(message));
                                        if (self->closed_)
                                            return;
                                    }
                                    self->read();
                                });
    }

    void Connection::reject(const Header& h, std::uint8_t reason) {
        Header rejection{config_.mode == Mode::HsmsSs ? SessionId{0xffff} : h.session_id,
                         reason == 2 ? h.ptype : h.stype,
                         reason,
                         0,
                         7,
                         h.system_bytes};
        auto   result = enqueue({rejection, {}}, {});
        if (!result)
            close(result.error(), true);
    }

    void Connection::receive(Message message) {
        const auto h = message.header;
        if (callbacks_.on_trace)
            callbacks_.on_trace(*this, {false, h, message.body.size()});
        if (auto valid = machine_.validate_incoming(h); !valid) {
            auto failure   = valid.error();
            failure.header = h;
            close(std::move(failure), true);
            return;
        }
        if (const auto reason = machine_.reject_reason(h); reason != 0) {
            reject(h, reason);
            return;
        }
        if (h.is_data()) {
            if (h.is_primary()) {
                if (callbacks_.on_primary_received) {
                    IncomingPrimary incoming;
                    incoming.message = std::move(message);
                    incoming.ticket  = std::make_shared<IncomingPrimary::Ticket>(
                            IncomingPrimary::Ticket{weak_from_this(), h, false});
                    callbacks_.on_primary_received(*this, std::move(incoming));
                }
                else {
                    auto failure   = error(ErrorCode::Unsupported, "No handler for incoming primary");
                    failure.header = h;
                    report(failure);
                }
            }
            else if (!transactions_->receive(message, config_.mode)) {
                if (callbacks_.on_unmatched_reply)
                    callbacks_.on_unmatched_reply(*this, message);
                else {
                    auto failure   = error(ErrorCode::TransactionNotFound, "Unmatched or late data reply");
                    failure.header = h;
                    report(failure);
                }
            }
            return;
        }
        if (h.stype == 7) {
            if (callbacks_.on_reject_received)
                callbacks_.on_reject_received(*this, {h, h.byte2, h.byte3});
            const bool matched = transactions_->receive(message, config_.mode);
            if (!matched && !callbacks_.on_reject_received) {
                auto failure   = error(ErrorCode::Rejected, "Unmatched remote Reject");
                failure.header = h;
                failure.status = h.byte3;
                report(failure);
            }
        }
        else if (h.stype == 2 || h.stype == 4 || h.stype == 6) {
            if (!transactions_->receive(message, config_.mode))
                reject(h, 3);
        }
        else if (h.stype == 9) {
            if (config_.mode == Mode::HsmsSs) {
                close(error(ErrorCode::Disconnected, "Remote Separate"), false);
            }
            else {
                machine_.separated(h.session_id);
                notify();
            }
        }
        else {
            std::uint8_t status = 0;
            if (h.stype == 1 && callbacks_.on_select_requested)
                status = callbacks_.on_select_requested(*this, h);
            if (h.stype == 3 && callbacks_.on_deselect_requested)
                status = callbacks_.on_deselect_requested(*this, h);
            const auto response      = machine_.respond(h, status);
            const bool failed_select = config_.mode == Mode::HsmsSs && h.stype == 1 && response.byte3 != 0;
            auto       sent = enqueue({response, {}}, [weak = weak_from_this(), failed_select](Result<void> result) {
                if (const auto self = weak.lock(); self && !self->closed_) {
                    if (!result)
                        self->close(result.error(), true);
                    else if (failed_select)
                        self->close(error(ErrorCode::SelectFailed, "HSMS-SS Select refused"), false);
                }
            });
            if (!sent)
                close(sent.error(), true);
            else
                notify();
        }
    }

    Result<void> Connection::enqueue(Message message, Completion completion, TransactionId id, bool transaction) {
        if (closed_)
            return std::unexpected(error(ErrorCode::Disconnected, "Connection is closed"));
        const auto remaining = config_.max_queued_bytes - queued_bytes_;
        if (remaining < 14 || message.body.size() > remaining - 14) {
            return std::unexpected(error(ErrorCode::Backpressure, "Send queue byte limit reached"));
        }
        auto bytes = MessageCodec::encode(message, config_.max_send_length);
        if (!bytes)
            return std::unexpected(bytes.error());
        const bool idle = queue_.empty();
        queued_bytes_ += bytes->size();
        queue_.push_back(std::make_shared<Outgoing>(
                Outgoing{std::move(*bytes), message.header, std::move(completion), id, transaction}));
        if (idle)
            write();
        return {};
    }

    void Connection::write() {
        if (closed_ || writing_ || queue_.empty())
            return;
        auto item = queue_.front();
        if (item->header.is_data() && !machine_.session_selected(item->header.session_id)) {
            queued_bytes_ -= item->bytes.size();
            queue_.pop_front();
            auto failure = error(ErrorCode::InvalidState, "Session ended before queued data could be sent");
            if (item->transaction)
                transactions_->fail(item->header.system_bytes, failure);
            finish(item->completion, std::unexpected(std::move(failure)));
            asio::post(socket_.get_executor(), [self = shared_from_this()] { self->write(); });
            return;
        }
        if (item->transaction)
            transactions_->write_started(item->header.system_bytes);
        writing_ = true;
        asio::async_write(socket_, asio::buffer(item->bytes),
                          [self = shared_from_this(), item](std::error_code ec, std::size_t) {
                              if (self->closed_)
                                  return;
                              self->writing_ = false;
                              if (ec) {
                                  auto failure      = error(ErrorCode::Transport, "TCP send failed");
                                  failure.transport = ec;
                                  self->close(std::move(failure), true);
                                  return;
                              }
                              self->queued_bytes_ -= item->bytes.size();
                              self->queue_.pop_front();
                              if (item->transaction)
                                  self->transactions_->written(item->header.system_bytes);
                              if (self->callbacks_.on_trace) {
                                  self->callbacks_.on_trace(*self, {true, item->header, item->bytes.size() - 14});
                              }
                              self->finish(item->completion, {});
                              self->write();
                          });
    }

    void Connection::control(std::uint8_t stype, SessionId id, Completion completion) {
        if (stopping_ || separating_.contains(id)) {
            finish(completion, std::unexpected(error(ErrorCode::InvalidState, "Connection/session is stopping")));
            return;
        }
        Header request{id, 0, 0, 0, stype, 0};
        if (auto valid = machine_.validate_outgoing(request); !valid) {
            finish(completion, std::unexpected(valid.error()));
            return;
        }
        auto system_bytes = transactions_->allocate_system_bytes();
        if (!system_bytes) {
            finish(completion, std::unexpected(system_bytes.error()));
            return;
        }
        request.system_bytes = *system_bytes;
        auto callback        = [weak = weak_from_this(), request, completion](Result<Message> result) mutable {
            const auto self = weak.lock();
            if (!self)
                return;
            self->machine_.request_finished(request);
            if (!result) {
                const auto failure    = result.error();
                const bool must_close = !self->closed_ && (failure.code == ErrorCode::T6Timeout ||
                                                           (self->config_.mode == Mode::HsmsSs && request.stype == 1));
                self->finish(completion, std::unexpected(failure));
                if (must_close)
                    self->close(failure, true);
                return;
            }
            self->machine_.response_received(request, result->header);
            self->notify();
            if (result->header.byte3 != 0) {
                auto failure   = error(request.stype == 1 ? ErrorCode::SelectFailed : ErrorCode::DeselectFailed,
                                       "Remote control response has nonzero status");
                failure.header = result->header;
                failure.status = result->header.byte3;
                self->finish(completion, std::unexpected(failure));
                if (self->config_.mode == Mode::HsmsSs && request.stype == 1)
                    self->close(failure, true);
            }
            else {
                self->finish(completion, {});
            }
        };
        auto added = transactions_->add(next_id_++, request, std::move(callback));
        if (!added) {
            transactions_->release_system_bytes(*system_bytes);
            finish(completion, std::unexpected(added.error()));
            return;
        }
        machine_.request_started(request);
        auto queued = enqueue({request, {}}, {}, 0, true);
        if (!queued)
            transactions_->fail(*system_bytes, queued.error());
    }

    void Connection::async_session_selected(SessionId id, std::function<void(Result<bool>)> completion) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), id, completion = std::move(completion)] {
            if (!completion) {
                self->report(error(ErrorCode::InvalidConfig, "Session query requires a completion callback"));
                return;
            }
            completion(self->machine_.session_selected(id));
        });
    }

    void Connection::async_select(SessionId id, Completion completion) {
        asio::post(socket_.get_executor(),
                   [self = shared_from_this(), id, completion = std::move(completion)]() mutable {
                       self->control(1, id, std::move(completion));
                   });
    }

    void Connection::async_deselect(SessionId id, Completion completion) {
        asio::post(socket_.get_executor(),
                   [self = shared_from_this(), id, completion = std::move(completion)]() mutable {
                       self->control(3, id, std::move(completion));
                   });
    }

    void Connection::async_linktest(Completion completion) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), completion = std::move(completion)]() mutable {
            self->control(5, 0xffff, std::move(completion));
        });
    }

    void Connection::async_send(DataMessage primary, Completion completion) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), primary = std::move(primary),
                                            completion = std::move(completion)]() mutable {
            primary.header.byte2 &= 0x7f;
            if (!primary.header.is_primary() || self->stopping_ ||
                self->separating_.contains(primary.header.session_id)) {
                self->finish(completion,
                             std::unexpected(error(ErrorCode::InvalidState, "Cannot send this primary now")));
                return;
            }
            if (auto valid = self->machine_.validate_outgoing(primary.header); !valid) {
                self->finish(completion, std::unexpected(valid.error()));
                return;
            }
            auto system_bytes = self->transactions_->allocate_system_bytes();
            if (!system_bytes) {
                self->finish(completion, std::unexpected(system_bytes.error()));
                return;
            }
            primary.header.system_bytes = *system_bytes;
            auto sent = self->enqueue(std::move(primary), [weak = self->weak_from_this(), system = *system_bytes,
                                                           completion](Result<void> result) mutable {
                if (auto connection = weak.lock()) {
                    connection->transactions_->release_system_bytes(system);
                    connection->finish(completion, std::move(result));
                }
            });
            if (!sent) {
                self->transactions_->release_system_bytes(*system_bytes);
                self->finish(completion, std::unexpected(sent.error()));
            }
        });
    }

    TransactionId Connection::async_request(DataMessage primary, ReplyCompletion completion) {
        const auto id = next_id_++;
        asio::post(socket_.get_executor(), [self = shared_from_this(), id, primary = std::move(primary),
                                            completion = std::move(completion)]() mutable {
            primary.header.byte2 |= 0x80;
            if (!primary.header.is_primary() || self->stopping_ ||
                self->separating_.contains(primary.header.session_id)) {
                self->finish(completion,
                             std::unexpected(error(ErrorCode::InvalidState, "Cannot request this primary now")));
                return;
            }
            if (auto valid = self->machine_.validate_outgoing(primary.header); !valid) {
                self->finish(completion, std::unexpected(valid.error()));
                return;
            }
            auto system_bytes = self->transactions_->allocate_system_bytes();
            if (!system_bytes) {
                self->finish(completion, std::unexpected(system_bytes.error()));
                return;
            }
            primary.header.system_bytes = *system_bytes;
            auto added                  = self->transactions_->add(
                    id, primary.header, [weak = self->weak_from_this(), completion](Result<Message> result) mutable {
                        if (auto connection = weak.lock())
                            connection->finish(completion, std::move(result));
                    });
            if (!added) {
                self->transactions_->release_system_bytes(*system_bytes);
                self->finish(completion, std::unexpected(added.error()));
                return;
            }
            auto sent = self->enqueue(std::move(primary), {}, id, true);
            if (!sent)
                self->transactions_->fail(*system_bytes, sent.error());
        });
        return id;
    }

    void Connection::async_reply(IncomingPrimary primary, std::uint8_t function, Bytes body, Completion completion) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), primary = std::move(primary), function,
                                            body = std::move(body), completion = std::move(completion)]() mutable {
            if (!primary.ticket || primary.ticket->owner.lock() != self || primary.ticket->used ||
                !primary.ticket->header.reply_expected()) {
                self->finish(completion,
                             std::unexpected(error(ErrorCode::InvalidMessage, "Invalid or already used reply ticket")));
                return;
            }
            auto h = primary.ticket->header;
            if (function != 0 && unsigned(function) != unsigned(h.byte3) + 1) {
                self->finish(completion, std::unexpected(error(ErrorCode::InvalidMessage,
                                                               "Reply function does not match primary")));
                return;
            }
            h.byte2 &= 0x7f;
            h.byte3 = function;
            if (self->stopping_ || self->separating_.contains(h.session_id)) {
                self->finish(completion,
                             std::unexpected(error(ErrorCode::InvalidState, "Connection/session is stopping")));
                return;
            }
            if (auto valid = self->machine_.validate_outgoing(h); !valid) {
                self->finish(completion, std::unexpected(valid.error()));
                return;
            }
            auto sent = self->enqueue({h, std::move(body)}, completion);
            if (sent)
                primary.ticket->used = true;
            else
                self->finish(completion, std::unexpected(sent.error()));
        });
    }

    void Connection::cancel(TransactionId id, Completion completion) {
        asio::post(socket_.get_executor(), [self       = shared_from_this(), id,
                                            completion = std::move(completion)]() mutable {
            if (!self->transactions_->cancel(id)) {
                self->finish(completion,
                             std::unexpected(error(ErrorCode::TransactionNotFound, "Transaction is no longer open")));
                return;
            }
            for (auto it = self->queue_.empty() ? self->queue_.end() : std::next(self->queue_.begin());
                 it != self->queue_.end(); ++it) {
                if ((*it)->id == id) {
                    self->queued_bytes_ -= (*it)->bytes.size();
                    self->queue_.erase(it);
                    break;
                }
            }
            self->finish(completion, {});
        });
    }

    void Connection::async_separate(SessionId id, Completion completion) {
        asio::post(
                socket_.get_executor(), [self = shared_from_this(), id, completion = std::move(completion)]() mutable {
                    Header header{id, 0, 0, 0, 9, 0};
                    if (self->stopping_ || self->separating_.contains(id)) {
                        self->finish(completion,
                                     std::unexpected(error(ErrorCode::InvalidState, "Connection/session is stopping")));
                        return;
                    }
                    if (auto valid = self->machine_.validate_outgoing(header); !valid) {
                        self->finish(completion, std::unexpected(valid.error()));
                        return;
                    }
                    auto system_bytes = self->transactions_->allocate_system_bytes();
                    if (!system_bytes) {
                        self->finish(completion, std::unexpected(system_bytes.error()));
                        return;
                    }
                    header.system_bytes = *system_bytes;
                    self->separating_.insert(id);
                    if (self->config_.mode == Mode::HsmsSs) {
                        self->stopping_ = true;
                        self->heartbeat_.cancel();
                        self->arm_shutdown();
                    }
                    auto sent = self->enqueue({header, {}}, [weak = self->weak_from_this(), header,
                                                             completion](Result<void> result) mutable {
                        if (const auto connection = weak.lock()) {
                            connection->transactions_->release_system_bytes(header.system_bytes);
                            connection->separating_.erase(header.session_id);
                            const bool separate_succeeded = result.has_value() && !connection->closed_;
                            if (separate_succeeded) {
                                connection->machine_.separated(header.session_id);
                                connection->notify();
                            }
                            connection->finish(completion, std::move(result));
                            if (separate_succeeded &&
                                (connection->config_.mode == Mode::HsmsSs ||
                                 (connection->stopping_ && connection->machine_.state() == State::NotSelected))) {
                                connection->close(error(ErrorCode::Disconnected, "Local Separate"), false);
                            }
                        }
                    });
                    if (!sent) {
                        self->separating_.erase(id);
                        if (self->config_.mode == Mode::HsmsSs) {
                            self->stopping_ = false;
                            self->shutdown_.cancel();
                            self->arm_linktest();
                        }
                        self->transactions_->release_system_bytes(*system_bytes);
                        self->finish(completion, std::unexpected(sent.error()));
                    }
                });
    }

    void Connection::arm_shutdown() {
        shutdown_.expires_after(config_.timeouts.t6);
        shutdown_.async_wait([self = shared_from_this()](std::error_code ec) {
            if (ec == asio::error::operation_aborted || self->closed_ || !self->stopping_)
                return;
            auto failure =
                    error(ErrorCode::Transport, ec ? "Shutdown timer failed" : "Graceful shutdown deadline expired");
            failure.transport = ec;
            self->close(std::move(failure), true);
        });
    }

    void Connection::async_stop(Completion completion) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), completion = std::move(completion)]() mutable {
            if (self->closed_) {
                self->finish(completion, {});
                return;
            }
            self->stop_completions_.push_back(std::move(completion));
            if (self->stopping_)
                return;
            self->stopping_ = true;
            self->heartbeat_.cancel();
            if (self->machine_.state() != State::Selected) {
                self->close(error(ErrorCode::Cancelled, "Connection stopped"), false);
                return;
            }
            self->arm_shutdown();
            const auto sessions = self->machine_.selected_sessions();
            for (auto id : sessions) {
                if (self->separating_.contains(id))
                    continue;
                auto system_bytes = self->transactions_->allocate_system_bytes();
                if (!system_bytes) {
                    self->close(system_bytes.error(), true);
                    return;
                }
                Header header{id, 0, 0, 0, 9, *system_bytes};
                auto   sent = self->enqueue({header, {}}, [weak = self->weak_from_this(), header](Result<void> result) {
                    if (auto connection = weak.lock(); connection && !connection->closed_) {
                        connection->transactions_->release_system_bytes(header.system_bytes);
                        if (!result)
                            connection->close(result.error(), true);
                        else {
                            connection->machine_.separated(header.session_id);
                            connection->notify();
                            if (connection->machine_.state() == State::NotSelected)
                                connection->close(error(ErrorCode::Cancelled, "Connection stopped"), false);
                        }
                    }
                });
                if (!sent) {
                    self->close(sent.error(), true);
                    return;
                }
            }
        });
    }

    void Connection::close(ErrorInfo reason, bool report_error) {
        if (closed_)
            return;
        closed_ = true;
        t7_.cancel();
        t8_.cancel();
        heartbeat_.cancel();
        shutdown_.cancel();
        std::error_code ec;
        socket_.close(ec);
        if (ec) {
            auto failure      = error(ErrorCode::Transport, "TCP close failed");
            failure.transport = ec;
            report(failure);
        }
        machine_.disconnected();
        public_state_.store(State::NotConnected);
        transactions_->cancel_all(reason);
        auto queue    = std::exchange(queue_, {});
        queued_bytes_ = 0;
        for (auto& item : queue)
            finish(item->completion, std::unexpected(reason));
        notify(reason);
        if (report_error)
            report(reason);
        auto completions = std::exchange(stop_completions_, {});
        for (auto& completion : completions) {
            finish(completion, report_error ? Result<void>{std::unexpected(reason)} : Result<void>{});
        }
        if (on_closed_)
            std::exchange(on_closed_, {})();
    }

} // namespace glove::e37
