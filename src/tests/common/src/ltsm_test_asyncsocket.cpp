#include <vector>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ltsm_application.h"
#include "ltsm_async_socket.h"

using namespace LTSM;

using ::testing::_;
using ::testing::Invoke;

class MockAsyncSocket : public LTSM::AsyncSocketBase {
public:
    MOCK_METHOD(void, closeSocket, (), (override));
    MOCK_METHOD(void, sync_recv_buf, (void* ptr, size_t len), (const, override));
    MOCK_METHOD(void, sync_send_buf, (const void* ptr, size_t len), (const, override));
    
    MOCK_METHOD(boost::asio::awaitable<void>, async_recv_buf, (void* ptr, size_t len), (const, override));
    MOCK_METHOD(boost::asio::awaitable<void>, async_recv_buffers, 
                (std::initializer_list<boost::asio::mutable_buffer> list), (const, override));
    
    MOCK_METHOD(boost::asio::awaitable<void>, async_send_buf, (const boost::asio::const_buffer& buf), (const, override));
    MOCK_METHOD(boost::asio::awaitable<void>, async_send_buffers, 
                (std::initializer_list<boost::asio::const_buffer> list), (const, override));
};

class AsyncSocketTest : public ::testing::Test {
protected:
    boost::asio::io_context ctx;
    MockAsyncSocket mock_sock;
};

TEST_F(AsyncSocketTest, ValueToBufferWithIntegral) {
    uint32_t val = 0x11223344;
    boost::asio::mutable_buffer buf = LTSM::value_to_buffer(val);
    
    EXPECT_EQ(buf.data(), &val);
    EXPECT_EQ(buf.size(), sizeof(val));
}

TEST_F(AsyncSocketTest, ValueToConstBufferWithStdString) {
    std::string str = "Hello";
    boost::asio::const_buffer buf = LTSM::value_to_const_buffer(str);
    
    EXPECT_EQ(buf.data(), str.data());
    EXPECT_EQ(buf.size(), str.size());
}

TEST_F(AsyncSocketTest, AsyncRecvValuesFillsVariables) {
    EXPECT_CALL(mock_sock, async_recv_buffers(_))
        .WillOnce(Invoke([](std::initializer_list<boost::asio::mutable_buffer> list) -> boost::asio::awaitable<void> {
            auto it = list.begin();
            if (it != list.end()) {
                *static_cast<uint16_t*>(it->data()) = 0x1234;
                it++;
            }
            if (it != list.end()) {
                *static_cast<uint8_t*>(it->data()) = 0x99;
            }
            
            co_return;
        }));

    uint16_t out1 = 0;
    uint8_t  out2 = 0;

    boost::asio::co_spawn(ctx, [&]() -> boost::asio::awaitable<void> {
        co_await mock_sock.async_recv_values(out1, out2);
        co_return;
    }, boost::asio::detached);

    ctx.run();

    EXPECT_EQ(out1, 0x1234);
    EXPECT_EQ(out2, 0x99);
}

TEST(AsyncTcpStreamTest, SocketAccessAndLifecycle) {
    boost::asio::io_context local_ctx;
    
    LTSM::AsyncTcpStream stream(local_ctx.get_executor());
    
    boost::asio::ip::tcp::socket& raw_socket = stream.socket();
    EXPECT_FALSE(raw_socket.is_open());
    
    EXPECT_NO_THROW(stream.closeSocket());
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_asyncsocket.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
