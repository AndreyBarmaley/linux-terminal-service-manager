#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <exception>
#include <iostream>
#include <cassert>
#include <future>
#include <thread>
#include <chrono>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"

using namespace std::chrono_literals;
using namespace LTSM;

int main()
{
    auto path = "test2.sock";

    auto fd0 = UnixSocket::listen(path);
    if(0 == fd0)
    {
        std::cerr << "listen socket failed" << std::endl;
        return -1;
    }
    std::cout << "listen socket: " << fd0 << std::endl;

    // accept
    std::future<int> job = std::async(std::launch::async, [=](){ return accept(fd0, nullptr, nullptr); });
    std::this_thread::sleep_for(100ms);

    auto fd2 = UnixSocket::connect(path);
    if(0 == fd2)
    {
        std::cerr << "client socket failed" << std::endl;
        return -1;
    }
    std::cout << "client socket: " << fd2 << std::endl;


    // wait server socket
    while(job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);
    auto fd1 = job.get();

    if(0 == fd1)
    {
        std::cerr << "server socket failed" << std::endl;
        return -1;
    }
    std::cout << "server socket: " << fd1 << std::endl;

    // close listen
    close(fd0);

    // server stream
    SocketStream sock1(fd1);
    // client stream
    SocketStream sock2(fd2);

    sock1.sendIntBE16(0x1234);
    sock1.sendIntBE32(0x12345678);
    sock1.sendIntBE64(0x1234567898765432);
    sock1.sendFlush();

    std::cout << "test1 socket::sendInt16BE/recvInt16BE" << std::endl;
    assert(sock2.recvIntBE16() == 0x1234);
    std::cout << "test1 socket::sendInt32BE/recvInt32BE" << std::endl;
    assert(sock2.recvIntBE32() == 0x12345678);
    std::cout << "test1 socket::sendInt64BE/recvInt64BE" << std::endl;
    assert(sock2.recvIntBE64() == 0x1234567898765432);

    sock2.sendIntLE64(0x1234567898765432);
    sock2.sendIntLE32(0x12345678);
    sock2.sendIntLE16(0x1234);
    sock2.sendFlush();

    std::cout << "test2 socket::sendInt64LE/recvInt64LE" << std::endl;
    assert(sock1.recvIntLE64() == 0x1234567898765432);
    std::cout << "test2 socket::sendInt32LE/recvInt32LE" << std::endl;
    assert(sock1.recvIntLE32() == 0x12345678);
    std::cout << "test2 socket::sendInt16LE/recvInt16LE" << std::endl;
    assert(sock1.recvIntLE16() == 0x1234);

    // zlib part
    auto zlib1 = std::make_unique<ZLib::DeflateStream>();
    for(int it = 0; it < 100; it++)
        zlib1->sendIntLE64(0x1234567898765432);

    auto buf1 = zlib1->deflateFlush();
    sock1.sendIntBE32(buf1.size()).sendData(buf1).sendFlush();

    auto len = sock2.recvIntBE32();
    auto buf2 = sock2.recvData(len);
    auto zlib2 = std::make_unique<ZLib::InflateStream>();
    zlib2->appendData(buf2);

    std::cout << "test zlib socket::sendInt64LE/recvInt64LE" << std::endl;
    for(int it = 0; it < 100; it++)
    {
        auto val = zlib2->recvIntLE64();
        assert(val == 0x1234567898765432);
    }

    zlib1.reset();
    zlib2.reset();

    // tls part
    auto priority = "NORMAL:+ANON-ECDH:+ANON-DH";

    auto tls1 = std::make_unique<TLS::Stream>(& sock1);
    bool srvMode = true;
    std::future<bool> job2 = std::async(std::launch::async, [&](){ return tls1->initAnonHandshake(priority, srvMode, 0); });
    std::this_thread::sleep_for(10ms);

    auto tls2 = std::make_unique<TLS::Stream>(& sock2);
    if(! tls2->initAnonHandshake(priority, ! srvMode, 0))
    {
        std::cerr << "tls2 init failed" << std::endl;
        return -1;
    }

    // wait init tls1
    while(job2.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);
    if(! job2.get())
    {
        std::cerr << "tls1 init failed" << std::endl;
        return -1;
    }


    for(int it = 0; it < 100; it++)
        tls1->sendIntLE64(0x1234567898765432);

    tls1->sendFlush();

    std::cout << "tls socket::sendInt64LE/recvInt64LE" << std::endl;
    for(int it = 0; it < 100; it++)
    {
        auto val = tls2->recvIntLE64();
        assert(val == 0x1234567898765432);
    }

    tls1.reset();
    tls2.reset();

    std::filesystem::remove(path);
    return 0;
}
