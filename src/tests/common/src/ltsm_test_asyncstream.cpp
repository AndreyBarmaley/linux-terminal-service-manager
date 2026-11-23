// Google Gemini AI

#include <span>
#include <vector>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ltsm_application.h"
#include "ltsm_async_stream.h"

using namespace LTSM;

using ::testing::_;
using ::testing::Invoke;

class MockAsyncStream : public AsyncStreamBase {
  public:
    MOCK_METHOD(bool, isOpen, (), (const, override));
    MOCK_METHOD(void, close, (), (override));
    MOCK_METHOD(boost::asio::any_io_executor, get_executor, (), (override));

    MOCK_METHOD(void, sync_recv_buf, (void* ptr, std::size_t len), (override));
    MOCK_METHOD(void, sync_send_buf, (const void* ptr, std::size_t len), (override));
    MOCK_METHOD(void, sync_send_buffers, (std::span<const boost::asio::const_buffer> buffers), (override));

    MOCK_METHOD(boost::asio::awaitable<void>, async_recv_buf, (void* ptr, std::size_t len), (override));
    MOCK_METHOD(boost::asio::awaitable<std::size_t>, async_read_some, (boost::asio::mutable_buffer buffer), (override));
    MOCK_METHOD(boost::asio::awaitable<void>, async_recv_buffers,
                (std::span<boost::asio::mutable_buffer> buffers), (override));

    MOCK_METHOD(boost::asio::awaitable<void>, async_send_buf, (const boost::asio::const_buffer& buf), (override));
    MOCK_METHOD(boost::asio::awaitable<void>, async_send_buffers,
                (std::span<const boost::asio::const_buffer> buffers), (override));
};

class AsyncStreamTest : public ::testing::Test {
  protected:
    boost::asio::io_context ctx;
    MockAsyncStream mock_sock;

    template <typename Awaitable>
    void run_coro(Awaitable&& coro) {
        std::exception_ptr eptr;
        boost::asio::co_spawn(
            ctx,
        [&]() -> boost::asio::awaitable<void> {
            try {
                co_await std::forward<Awaitable>(coro);
            } catch(...) {
                eptr = std::current_exception();
            }
        },
        boost::asio::detached
        );
        ctx.restart();
        ctx.run();

        if(eptr) {
            std::rethrow_exception(eptr);
        }
    }
};

TEST_F(AsyncStreamTest, ValueToBufferWithIntegralAndContainers) {
    uint32_t val = 0x11223344;
    auto buf1 = value_to_buffer(val);
    EXPECT_EQ(buf1.data(), &val);
    EXPECT_EQ(buf1.size(), sizeof(val));

    std::vector<uint8_t> vec(16, 0xAA);
    auto buf2 = value_to_buffer(vec);
    EXPECT_EQ(buf2.data(), vec.data());
    EXPECT_EQ(buf2.size(), vec.size());
}

TEST_F(AsyncStreamTest, ValueToConstBufferWithStrings) {
    std::string str = "Hello LTSM";
    auto buf1 = value_to_const_buffer(str);
    EXPECT_EQ(buf1.data(), str.data());
    EXPECT_EQ(buf1.size(), str.size());

    std::string_view sv = "ViewTest";
    auto buf2 = value_to_const_buffer(sv);
    EXPECT_EQ(buf2.data(), sv.data());
    EXPECT_EQ(buf2.size(), sv.size());
}

TEST_F(AsyncStreamTest, AsyncRecvValuesFillsVariables) {
    EXPECT_CALL(mock_sock, async_recv_buffers(_))
    .WillOnce([](std::span<boost::asio::mutable_buffer> list) -> boost::asio::awaitable<void> {
        EXPECT_EQ(list.size(), 3u);

        if(list.size() == 3) {
            *static_cast<uint16_t*>(list[0].data()) = 0x1234;
            *static_cast<uint8_t*>(list[1].data())  = 0x99;
            std::memcpy(list[2].data(), "ABCD", 4);
        }
        co_return;
    });

    uint16_t out1 = 0;
    uint8_t  out2 = 0;
    std::string out3(4, '\0');

    run_coro(mock_sock.async_recv_values(out1, out2, out3));

    EXPECT_EQ(out1, 0x1234);
    EXPECT_EQ(out2, 0x99);
    EXPECT_EQ(out3, "ABCD");
}

TEST_F(AsyncStreamTest, EndiannessRecvAndSend) {
    EXPECT_CALL(mock_sock, async_recv_buf(_, sizeof(uint32_t)))
    .WillOnce([](void* ptr, std::size_t) -> boost::asio::awaitable<void> {
        const uint8_t raw[4] = {0x01, 0x02, 0x03, 0x04};
        std::memcpy(ptr, raw, 4);
        co_return;
    });

    uint32_t be_val = 0;
    run_coro([&]() -> boost::asio::awaitable<void> {
        be_val = co_await mock_sock.async_recv_be32();
    }());
    EXPECT_EQ(be_val, 0x01020304u);

    EXPECT_CALL(mock_sock, async_send_buf(_))
    .WillOnce([](const boost::asio::const_buffer & buf) -> boost::asio::awaitable<void> {
        EXPECT_EQ(buf.size(), 2u);
        const auto* bytes = static_cast<const uint8_t*>(buf.data());
        EXPECT_EQ(bytes[0], 0xCD);
        EXPECT_EQ(bytes[1], 0xAB);
        co_return;
    });

    run_coro(mock_sock.async_send_le16(0xABCD));
}

TEST(AsyncStreamModeTest, ThrowsOnInvalidDirection) {
    boost::asio::io_context local_ctx;

    AsyncStream<boost::asio::ip::tcp::socket, StreamMode::IRead> ro_stream(local_ctx.get_executor());
    AsyncStream<boost::asio::ip::tcp::socket, StreamMode::IWrite> wo_stream(local_ctx.get_executor());

    uint8_t dummy = 0;
    EXPECT_THROW(ro_stream.sync_send_buf(&dummy, 1), std::logic_error);
    EXPECT_THROW(wo_stream.sync_recv_buf(&dummy, 1), std::logic_error);
}

TEST(AsyncTcpStreamTest, LoopbackSendAndRecv) {
    boost::asio::io_context local_ctx;
    using tcp = boost::asio::ip::tcp;

    tcp::acceptor acceptor(local_ctx, tcp::endpoint(tcp::v4(), 0));
    const auto port = acceptor.local_endpoint().port();

    AsyncTcpStream client(local_ctx.get_executor());
    EXPECT_FALSE(client.isOpen());

    client.stream().connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    AsyncTcpStream server(acceptor.accept());

    EXPECT_TRUE(client.isOpen());
    EXPECT_TRUE(server.isOpen());

    std::string received_str;
    uint32_t received_num = 0;

    boost::asio::co_spawn(
        local_ctx,
    [&]() -> boost::asio::awaitable<void> {
        co_await client.async_send_be32(0x2026CAFE);
        std::string msg = "HelloAsync";
        co_await client.async_send_values(msg);
    },
    boost::asio::detached
    );

    boost::asio::co_spawn(
        local_ctx,
    [&]() -> boost::asio::awaitable<void> {
        received_num = co_await server.async_recv_be32();
        received_str = co_await server.async_recv_string(10);
    },
    boost::asio::detached
    );

    local_ctx.run();

    EXPECT_EQ(received_num, 0x2026CAFEu);
    EXPECT_EQ(received_str, "HelloAsync");

    EXPECT_NO_THROW(client.close());
    EXPECT_NO_THROW(server.close());
    EXPECT_FALSE(client.isOpen());
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_asyncsocket.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
