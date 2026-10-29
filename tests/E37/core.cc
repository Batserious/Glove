#include <gtest/gtest.h>

#include <HSMS/MessageCodec.h>
#include <HSMS/ProtocolStateMachine.h>
#include <HSMS/TransactionManager.h>
#include <asio/io_context.hpp>

using namespace glove::e37;
using namespace std::chrono_literals;

TEST(E37Config, DefaultsToHsmsSS) {
    const Config config;
    EXPECT_EQ(config.mode, Mode::HsmsSS);
    EXPECT_TRUE(validate_config(config));

    ProtocolStateMachine machine(config);
    machine.connected();
    EXPECT_TRUE(machine.validate_outgoing({0xffff, 0, 0, 0, 1, 0}));
    EXPECT_FALSE(machine.validate_outgoing({0, 0, 0, 0, 1, 0}));
    EXPECT_FALSE(machine.validate_outgoing({0xffff, 0, 0, 0, 5, 0}));
    machine.response_received({0xffff, 0, 0, 0, 1, 0}, {0xffff, 0, 0, 0, 2, 0});
    EXPECT_TRUE(machine.session_selected(0));
    EXPECT_FALSE(machine.validate_outgoing({0xffff, 0, 0, 0, 3, 0}));
}

TEST(E37Config, ValidatesLimitsAndBothModes) {
    Config config;
    config.mode = Mode::Generic;
    EXPECT_TRUE(validate_config(config));
    config.timeouts.t3 = 120s;
    config.timeouts.t5 = 240s;
    config.timeouts.t6 = 240s;
    config.timeouts.t7 = 240s;
    config.timeouts.t8 = 120s;
    EXPECT_TRUE(validate_config(config));
    config.timeouts.t8 = 121s;
    EXPECT_FALSE(validate_config(config));
    config.timeouts.t8 = 1s;
    config.timeouts.t3 = 0s;
    EXPECT_FALSE(validate_config(config));
    config.timeouts.t3        = 1s;
    config.max_receive_length = 9;
    EXPECT_FALSE(validate_config(config));
    config.max_receive_length = 10;
    config.session_ids        = {0xffff};
    EXPECT_TRUE(validate_config(config));
    config.mode = Mode::HsmsSS;
    EXPECT_FALSE(validate_config(config));
    config.session_ids = {0x7fff};
    EXPECT_TRUE(validate_config(config));
    config.session_ids = {1, 1};
    EXPECT_FALSE(validate_config(config));
    config.session_ids = {};
    EXPECT_FALSE(validate_config(config));
}

TEST(E37Codec, EncodesExactNetworkBytesAndHeader) {
    Message     message{{0x1234, 0x81, 1, 0, 0, 0x12345678}, {0xab, 0xcd}};
    const Bytes expected{0, 0, 0, 12, 0x12, 0x34, 0x81, 1, 0, 0, 0x12, 0x34, 0x56, 0x78, 0xab, 0xcd};
    const auto  encoded = MessageCodec::encode(message, 12);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, expected);
    EXPECT_FALSE(MessageCodec::encode(message, 11));
    const auto header = MessageCodec::encode_header(message.header);
    EXPECT_EQ(Bytes(header.begin(), header.end()), Bytes(expected.begin() + 4, expected.begin() + 14));
}

TEST(E37Codec, EverySplitAndCoalescedFrames) {
    Message    message{{0, 0x81, 1, 0, 0, 123}, {1, 2, 3, 4}};
    const auto encoded = MessageCodec::encode(message, 1024);
    ASSERT_TRUE(encoded);
    for (std::size_t split = 1; split < encoded->size(); ++split) {
        MessageCodec codec(1024);
        auto         first = codec.feed(std::span(*encoded).first(split));
        ASSERT_TRUE(first);
        EXPECT_TRUE(first->empty());
        EXPECT_TRUE(codec.has_partial_message());
        auto second = codec.feed(std::span(*encoded).subspan(split));
        ASSERT_TRUE(second);
        ASSERT_EQ(second->size(), 1);
        EXPECT_EQ(second->front(), message);
        EXPECT_FALSE(codec.has_partial_message());
    }
    MessageCodec         codec(1024);
    std::vector<Message> messages;
    for (auto byte : *encoded) {
        auto result = codec.feed(std::span(&byte, 1));
        ASSERT_TRUE(result);
        for (auto& item : *result)
            messages.push_back(std::move(item));
    }
    EXPECT_EQ(messages, std::vector<Message>{message});
    auto joined = *encoded;
    joined.insert(joined.end(), encoded->begin(), encoded->end());
    auto result = codec.feed(joined);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->size(), 2);
}

TEST(E37Codec, RejectsMalformedLengthsBeforeBodyAllocation) {
    MessageCodec codec(100);
    EXPECT_FALSE(codec.feed(Bytes{0, 0, 0, 9}));
    EXPECT_FALSE(codec.feed(Bytes{0, 0, 0, 101}));
    EXPECT_FALSE(codec.feed(Bytes{255, 255, 255, 255}));
    EXPECT_FALSE(codec.has_partial_message());
}

TEST(E37Codec, ValidatesAllControlShapesAndReplyWBit) {
    for (auto stype : {1, 2, 3, 4, 5, 6, 7, 9}) {
        Message message{
                {0xffff, 0, static_cast<std::uint8_t>(stype == 7 ? 1 : 0), 0, static_cast<std::uint8_t>(stype), 123},
                {}};
        auto encoded = MessageCodec::encode(message, 10);
        ASSERT_TRUE(encoded);
        EXPECT_EQ(encoded->size(), 14);
        MessageCodec codec(10);
        auto         decoded = codec.feed(*encoded);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->front(), message);
        message.body = {1};
        EXPECT_FALSE(MessageCodec::encode(message, 1024));
    }
    EXPECT_FALSE(MessageCodec::encode({{0xffff, 1, 0, 0, 1, 0}, {}}, 10));
    EXPECT_FALSE(MessageCodec::encode({{1, 0, 0, 0, 5, 0}, {}}, 10));
    EXPECT_FALSE(MessageCodec::encode({{0xffff, 0, 0, 0, 7, 0}, {}}, 10));
    EXPECT_FALSE(MessageCodec::encode({{0, 0x81, 2, 0, 0, 0}, {}}, 10));
    // Unsupported types must reach the protocol layer so it can send Reject.
    EXPECT_TRUE(MessageCodec::encode({{0, 0, 0, 1, 42, 0}, {1}}, 100));
}

TEST(E37State, GenericAllowsBilateralSelectAndMultipleIndependentSessions) {
    Config config;
    config.mode         = Mode::Generic;
    config.connect_mode = ConnectMode::Passive;
    config.session_ids  = {1, 2};
    ProtocolStateMachine machine(config);
    EXPECT_FALSE(machine.validate_outgoing({1, 0, 0, 0, 1, 0}));
    machine.connected();
    EXPECT_EQ(machine.state(), State::NotSelected);
    EXPECT_TRUE(machine.validate_outgoing({0xffff, 0, 0, 0, 5, 0}));
    Header request{1, 0, 0, 0, 1, 0};
    EXPECT_TRUE(machine.validate_outgoing(request));
    machine.request_started(request);
    EXPECT_EQ(machine.respond(request, 2).byte3, 0);
    EXPECT_TRUE(machine.session_selected(1));
    EXPECT_FALSE(machine.session_selected(2));
    machine.response_received(request, {1, 0, 0, 0, 2, 0});
    EXPECT_EQ(machine.respond(request).byte3, 1);
    EXPECT_EQ(machine.respond({2, 0, 0, 0, 1, 1}).byte3, 0);
    machine.separated(1);
    EXPECT_EQ(machine.state(), State::Selected);
    machine.separated(2);
    EXPECT_EQ(machine.state(), State::NotSelected);
    machine.disconnected();
    EXPECT_EQ(machine.state(), State::NotConnected);
}

TEST(E37State, SimultaneousDeselectOverridesBusyPolicy) {
    Config config;
    config.mode = Mode::Generic;
    ProtocolStateMachine machine(config);
    machine.connected();
    machine.respond({0, 0, 0, 0, 1, 1});
    Header request{0, 0, 0, 0, 3, 2};
    machine.request_started(request);
    EXPECT_EQ(machine.respond(request, 2).byte3, 0);
    EXPECT_EQ(machine.state(), State::NotSelected);
    machine.response_received(request, {0, 0, 0, 0, 4, 2});
    EXPECT_EQ(machine.state(), State::NotSelected);
}

TEST(E37State, HsmsSSEnforcesProfileWithoutConflatingDeviceAndSelectionIds) {
    Config config;
    config.mode        = Mode::HsmsSS;
    config.session_ids = {1, 2};
    ProtocolStateMachine active(config);
    active.connected();
    EXPECT_TRUE(active.validate_outgoing({0xffff, 0, 0, 0, 1, 0}));
    EXPECT_FALSE(active.validate_outgoing({1, 0, 0, 0, 1, 0}));
    EXPECT_FALSE(active.validate_outgoing({0xffff, 0, 0, 0, 5, 0}));
    EXPECT_FALSE(active.validate_incoming({0xffff, 0, 0, 0, 1, 0}));
    active.response_received({0xffff, 0, 0, 0, 1, 0}, {0xffff, 0, 0, 0, 2, 0});
    EXPECT_TRUE(active.session_selected(1));
    EXPECT_TRUE(active.session_selected(2));
    EXPECT_FALSE(active.validate_outgoing({0xffff, 0, 0, 0, 1, 0}));
    EXPECT_FALSE(active.validate_outgoing({0xffff, 0, 0, 0, 3, 0}));
    EXPECT_FALSE(active.validate_incoming({0xffff, 0, 0, 0, 3, 0}));
    EXPECT_TRUE(active.validate_outgoing({0xffff, 0, 0, 0, 5, 0}));
    EXPECT_TRUE(active.validate_outgoing({0xffff, 0, 0, 0, 9, 0}));
    config.connect_mode = ConnectMode::Passive;
    ProtocolStateMachine passive(config);
    passive.connected();
    EXPECT_FALSE(passive.validate_outgoing({0xffff, 0, 0, 0, 1, 0}));
    EXPECT_TRUE(passive.validate_incoming({0xffff, 0, 0, 0, 1, 0}));
}

TEST(E37State, RejectReasonsAreDistinct) {
    Config config;
    config.mode = Mode::Generic;
    ProtocolStateMachine machine(config);
    machine.connected();
    EXPECT_EQ(machine.reject_reason({0, 1, 1, 0, 0, 1}), 4);
    EXPECT_EQ(machine.reject_reason({0, 0, 0, 0, 8, 1}), 1);
    EXPECT_EQ(machine.reject_reason({0, 0, 0, 1, 1, 1}), 2);
}

TEST(E37Transactions, MatchesAllReplyFieldsAndAllowsFunctionZero) {
    asio::io_context io;
    auto             manager = std::make_shared<TransactionManager>(io.get_executor(), Timeouts{}, 8);
    const auto       system  = manager->allocate_system_bytes();
    ASSERT_TRUE(system);
    Header request{1, 0x81, 1, 0, 0, *system};
    int    completed = 0;
    ASSERT_TRUE(manager->add(1, request, [&](Result<Message> result) {
        ASSERT_TRUE(result);
        EXPECT_EQ(result->header.byte3, 0);
        ++completed;
    }));
    Message response{{1, 1, 2, 0, 0, *system}, {}};
    EXPECT_FALSE(manager->receive(response, Mode::Generic));
    manager->write_started(*system);
    for (int field = 0; field < 5; ++field) {
        auto bad = response;
        if (field == 0)
            bad.header.session_id = 2;
        if (field == 1)
            bad.header.byte2 = 2;
        if (field == 2)
            bad.header.byte3 = 4;
        if (field == 3)
            ++bad.header.system_bytes;
        if (field == 4)
            bad.header.byte2 |= 0x80;
        EXPECT_FALSE(manager->receive(bad, Mode::Generic));
    }
    response.header.byte3 = 0;
    EXPECT_TRUE(manager->receive(response, Mode::Generic));
    EXPECT_FALSE(manager->receive(response, Mode::Generic));
    manager->written(*system);
    EXPECT_EQ(completed, 1);
    EXPECT_EQ(manager->size(), 0);
    auto next = manager->allocate_system_bytes();
    ASSERT_TRUE(next);
    EXPECT_NE(*next, *system);
}

TEST(E37Transactions, ControlsRequireMatchingResponseTypeAndSession) {
    asio::io_context io;
    auto             manager   = std::make_shared<TransactionManager>(io.get_executor(), Timeouts{}, 1);
    const auto       system    = *manager->allocate_system_bytes();
    int              completed = 0;
    ASSERT_TRUE(manager->add(1, {1, 0, 0, 0, 1, system}, [&](Result<Message> result) {
        EXPECT_TRUE(result);
        ++completed;
    }));
    manager->write_started(system);
    EXPECT_FALSE(manager->receive({{2, 0, 0, 0, 2, system}, {}}, Mode::Generic));
    EXPECT_FALSE(manager->receive({{1, 0, 0, 0, 4, system}, {}}, Mode::Generic));
    EXPECT_TRUE(manager->receive({{1, 0, 0, 0, 2, system}, {}}, Mode::Generic));
    EXPECT_EQ(completed, 1);
}

TEST(E37Transactions, TimersStartAfterWriteAndCompleteExactlyOnce) {
    asio::io_context io;
    Timeouts         timeouts;
    timeouts.t3    = 1s;
    timeouts.t6    = 1s;
    auto manager   = std::make_shared<TransactionManager>(io.get_executor(), timeouts, 8);
    int  completed = 0;
    for (auto stype : {0, 1}) {
        const auto system = *manager->allocate_system_bytes();
        ASSERT_TRUE(manager->add(
                system, {0, 0x81, 1, 0, static_cast<std::uint8_t>(stype), system}, [&, stype](Result<Message> result) {
                    ASSERT_FALSE(result);
                    EXPECT_EQ(result.error().code, stype == 0 ? ErrorCode::T3Timeout : ErrorCode::T6Timeout);
                    EXPECT_TRUE(result.error().header.has_value());
                    ++completed;
                }));
        manager->write_started(system);
    }
    io.run_for(20ms);
    EXPECT_EQ(completed, 0);
    EXPECT_EQ(manager->size(), 2);
    manager->written(1);
    manager->written(2);
    io.restart();
    io.run_for(1200ms);
    EXPECT_EQ(completed, 2);
    manager->cancel_all(error(ErrorCode::Cancelled, "test cleanup"));
    EXPECT_EQ(completed, 2);
}

TEST(E37Transactions, CapacityCancellationAndReject) {
    asio::io_context io;
    auto             manager   = std::make_shared<TransactionManager>(io.get_executor(), Timeouts{}, 1);
    const auto       first     = *manager->allocate_system_bytes();
    int              completed = 0;
    ASSERT_TRUE(manager->add(11, {0, 0x81, 1, 0, 0, first}, [&](Result<Message> result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
        ++completed;
    }));
    const auto second = *manager->allocate_system_bytes();
    EXPECT_FALSE(manager->add(12, {0, 0x81, 1, 0, 0, second}, [](Result<Message>) {}));
    EXPECT_TRUE(manager->cancel(11));
    EXPECT_FALSE(manager->cancel(11));
    EXPECT_EQ(completed, 1);
    ASSERT_TRUE(manager->add(12, {0, 0x81, 1, 0, 0, second}, [&](Result<Message> result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::Rejected);
        EXPECT_EQ(result.error().status, 4);
        ++completed;
    }));
    manager->write_started(second);
    EXPECT_TRUE(manager->receive({{0xffff, 0, 4, 0, 7, second}, {}}, Mode::HsmsSS));
    EXPECT_EQ(completed, 2);
}
