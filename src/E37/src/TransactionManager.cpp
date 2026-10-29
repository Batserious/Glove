#include <HSMS/TransactionManager.h>

#include <limits>

namespace glove::e37 {

    TransactionManager::TransactionManager(asio::any_io_executor executor, Timeouts timeouts, std::size_t limit) :
        executor_(std::move(executor)), timeouts_(timeouts), limit_(limit) {}

    TransactionManager::Pending::Pending(asio::any_io_executor executor, TransactionId token, Header header,
                                         ReplyCompletion callback) :
        timer(std::move(executor)), id(token), request(header), completion(std::move(callback)) {}

    std::size_t TransactionManager::size() const { return entries_.size(); }

    Result<std::uint32_t> TransactionManager::allocate_system_bytes() {
        if (reserved_.size() >= std::numeric_limits<std::uint32_t>::max() - 1) {
            return std::unexpected(error(ErrorCode::Backpressure, "System Bytes space exhausted"));
        }
        do {
            ++next_;
        } while (reserved_.contains(next_) || last_completed_ == next_);
        reserved_.insert(next_);
        return next_;
    }

    void TransactionManager::release_system_bytes(std::uint32_t system_bytes) {
        if (reserved_.erase(system_bytes) != 0) {
            last_completed_ = system_bytes;
        }
    }

    Result<void> TransactionManager::add(TransactionId id, const Header& request, ReplyCompletion completion) {
        if (!completion || entries_.size() >= limit_ || entries_.contains(request.system_bytes) ||
            !reserved_.contains(request.system_bytes)) {
            return std::unexpected(error(ErrorCode::Backpressure, "Transaction limit reached or invalid registration"));
        }
        entries_.emplace(request.system_bytes,
                         std::make_shared<Pending>(executor_, id, request, std::move(completion)));
        return {};
    }

    void TransactionManager::write_started(std::uint32_t system_bytes) {
        if (const auto it = entries_.find(system_bytes); it != entries_.end()) {
            it->second->started = true;
        }
    }

    void TransactionManager::written(std::uint32_t system_bytes) {
        const auto it = entries_.find(system_bytes);
        if (it == entries_.end())
            return;
        const auto pending = it->second;
        const bool data    = pending->request.is_data();
        pending->timer.expires_after(data ? timeouts_.t3 : timeouts_.t6);
        pending->timer.async_wait([weak = weak_from_this(), pending, data](std::error_code ec) {
            if (ec == asio::error::operation_aborted)
                return;
            if (const auto self = weak.lock()) {
                auto failure   = error(data ? ErrorCode::T3Timeout : ErrorCode::T6Timeout,
                                       data ? "T3 data reply timeout" : "T6 control transaction timeout");
                failure.header = pending->request;
                if (ec) {
                    failure           = error(ErrorCode::Transport, "Transaction timer failed");
                    failure.transport = ec;
                }
                self->fail(pending->request.system_bytes, std::move(failure));
            }
        });
    }

    bool TransactionManager::receive(const Message& response, Mode mode) {
        const auto& h  = response.header;
        const auto  it = entries_.find(h.system_bytes);
        if (it == entries_.end() || !it->second->started)
            return false;
        const auto& request = it->second->request;
        if (h.stype == 7) {
            const auto rejected_type    = h.byte3 == 2 ? request.ptype : request.stype;
            const auto expected_session = mode == Mode::HsmsSs ? SessionId{0xffff} : request.session_id;
            if (h.session_id != expected_session || h.byte2 != rejected_type)
                return false;
            auto failure   = error(ErrorCode::Rejected, "Remote entity rejected transaction");
            failure.header = h;
            failure.status = h.byte3;
            finish(h.system_bytes, std::unexpected(std::move(failure)));
            return true;
        }
        if (h.session_id != request.session_id || h.ptype != request.ptype)
            return false;
        if (request.is_data()) {
            if (!h.is_data() || h.is_primary() || h.reply_expected() || h.stream() != request.stream() ||
                (h.byte3 != 0 && unsigned(h.byte3) != unsigned(request.byte3) + 1))
                return false;
        }
        else if (h.stype != request.stype + 1) {
            return false;
        }
        finish(h.system_bytes, response);
        return true;
    }

    void TransactionManager::finish(std::uint32_t system_bytes, Result<Message> result) {
        const auto it = entries_.find(system_bytes);
        if (it == entries_.end())
            return;
        auto pending = std::move(it->second);
        entries_.erase(it);
        pending->timer.cancel();
        release_system_bytes(system_bytes);
        pending->completion(std::move(result));
    }

    bool TransactionManager::fail(std::uint32_t system_bytes, ErrorInfo failure) {
        if (!entries_.contains(system_bytes))
            return false;
        finish(system_bytes, std::unexpected(std::move(failure)));
        return true;
    }

    bool TransactionManager::cancel(TransactionId id) {
        for (const auto& [system_bytes, pending] : entries_) {
            if (pending->id == id) {
                return fail(system_bytes, error(ErrorCode::Cancelled, "Transaction cancelled locally"));
            }
        }
        return false;
    }

    void TransactionManager::cancel_all(const ErrorInfo& failure) {
        while (!entries_.empty()) {
            finish(entries_.begin()->first, std::unexpected(failure));
        }
        reserved_.clear();
    }

    void TransactionManager::cancel_data(SessionId id, Mode mode, const ErrorInfo& failure) {
        std::vector<std::uint32_t> cancelled;
        for (const auto& [system_bytes, pending] : entries_) {
            if (pending->request.is_data() && (mode == Mode::HsmsSs || pending->request.session_id == id)) {
                cancelled.push_back(system_bytes);
            }
        }
        for (auto system_bytes : cancelled)
            fail(system_bytes, failure);
    }

} // namespace glove::e37
