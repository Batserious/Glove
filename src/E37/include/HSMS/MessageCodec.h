#pragma once

#include "Types.h"

#include <array>
#include <span>

namespace glove::e37 {

    class MessageCodec {
    public:
        explicit MessageCodec(std::uint32_t max_length);

        static Result<Bytes>                encode(const Message& message, std::uint32_t max_length);
        Result<std::vector<Message>>        feed(std::span<const std::uint8_t> bytes);
        bool                                has_partial_message() const;
        void                                reset();
        static std::array<std::uint8_t, 10> encode_header(const Header& header);

    private:
        std::uint32_t max_length_;
        std::size_t   target_ = 4;
        Bytes         buffer_;
    };

} // namespace glove::e37
