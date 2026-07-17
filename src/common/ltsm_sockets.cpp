/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#ifdef __UNIX__
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#endif

#ifdef __APPLE__
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#endif

#ifdef __WIN32__
#include <winsock2.h>
#include <winsock.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <future>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_application.h"
#include "ltsm_tools.h"
#include "ltsm_sockets.h"

using namespace std::chrono_literals;

namespace LTSM {
    /* NetworkStream */
    bool NetworkStream::hasInput(int fd, int timeoutMS /* 1ms */) {
#if defined(__WIN32__)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = timeoutMS;

        int ret = select(fd + 1 /* max fd + 1 */, & fds /* in */, nullptr /* out */, nullptr, & tv);

        if(0 > ret) {
            // interrupted system call
            if(errno == EINTR) {
                return hasInput(fd, timeoutMS);
            }

            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "poll", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }

        // A value of 0 indicates that the call timed out and no file descriptors were ready
        if(0 == ret) {
            return false;
        }

        return FD_ISSET(fd, &fds);

#else // pool verson

        if(0 > fd) {
            return false;
        }

        struct pollfd fds = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0
        };

        int ret = poll(& fds, 1, timeoutMS);

        if(0 > ret) {
            // interrupted system call
            if(errno == EINTR) {
                return hasInput(fd, timeoutMS);
            }

            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "poll", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }

        // A value of 0 indicates that the call timed out and no file descriptors were ready
        if(0 == ret) {
            return false;
        }

        return (fds.revents & POLLIN);
#endif
    }

    size_t NetworkStream::hasData(int fd) {
        if(0 > fd) {
            return 0;
        }

#if defined(__WIN32__)
        long unsigned int count = 0;

        if(0 > ioctlsocket(fd, FIONREAD, & count)) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "ioctlsocket", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }

#else
        int count;

        if(0 > ioctl(fd, FIONREAD, & count)) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "ioctl", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }

#endif
        return count < 0 ? 0 : count;
    }

    NetworkStream::NetworkStream() {
        tp = std::chrono::steady_clock::now();
    }

    double calcBandwithMb(size_t bytes, size_t time) {
        auto res = bytes / static_cast<double>(time * 1024 * 1024);
        if(res < 0.0001) {
            res = 0.0001;
        }
        return res;
    }

    NetworkStream::~NetworkStream() {
        if(showStatistic) {
            if(auto dt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tp); dt.count()) {
                if(bytesIn) {
                    Application::info("{}: recv {} bytes, bandwith: {:.5} MBits/sec", "NetworkStatistic", bytesIn, calcBandwithMb(bytesIn, dt.count()));
                }

                if(bytesOut) {
                    Application::info("{}: send {} bytes, bandwith: {:.5} MBits/sec", "NetworkStatistic", bytesOut, calcBandwithMb(bytesOut, dt.count()));
                }
            }
        }
    }

    NetworkStream & NetworkStream::sendInt8(uint8_t val) {
        sendRaw(& val, 1);
        return *this;
    }

    NetworkStream & NetworkStream::sendInt16(uint16_t val) {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE16(val);
#else
        return sendIntBE16(val);
#endif
    }

    NetworkStream & NetworkStream::sendInt32(uint32_t val) {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE32(val);
#else
        return sendIntBE32(val);
#endif
    }

    NetworkStream & NetworkStream::sendInt64(uint64_t val) {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE64(val);
#else
        return sendIntBE64(val);
#endif
    }

    uint8_t NetworkStream::recvInt8(void) const {
        uint8_t byte;
        recvData(& byte, 1);
        return byte;
    }

    uint16_t NetworkStream::recvInt16(void) const {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE16();
#else
        return recvIntBE16();
#endif
    }

    uint32_t NetworkStream::recvInt32(void) const {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE32();
#else
        return recvIntBE32();
#endif
    }

    uint64_t NetworkStream::recvInt64(void) const {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE64();
#else
        return recvIntBE64();
#endif
    }

    void NetworkStream::recvSkip(size_t length) const {
        while(length--) {
            recvInt8();
        }
    }

    NetworkStream & NetworkStream::sendZero(size_t length) {
        while(length--) {
            sendInt8(0);
        }

        return *this;
    }

    NetworkStream & NetworkStream::sendData(const std::vector<uint8_t> & v) {
        sendRaw(v.data(), v.size());
        return *this;
    }

    NetworkStream & NetworkStream::sendString(std::string_view str) {
        sendRaw(str.data(), str.size());
        return *this;
    }

    std::vector<uint8_t> NetworkStream::recvData(size_t length) const {
        std::vector<uint8_t> res(length, 0);

        if(length) {
            recvData(res.data(), res.size());
        }

        return res;
    }

    void NetworkStream::recvData(void* ptr, size_t len) const {
        recvRaw(ptr, len);
    }

    std::string NetworkStream::recvString(size_t length) const {
        if(length) {
            std::string res(length, 0);
            recvData(res.data(), res.size());
            return res;
        }

        return {};
    }

    void NetworkStream::recvFrom(int fd, void* ptr, ssize_t len) {
        while(true) {
            ssize_t real = recv(fd, ptr, len, 0);

            if(len == real) {
                break;
            }

            if(0 < real && real < len) {
                ptr = static_cast<uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
            if(0 == real) {
                Application::warning("{}: {}", NS_FuncNameV, "end stream");
                throw network_error(NS_FuncNameS);
            }

            // error
            if(EAGAIN == errno || EINTR == errno) {
                continue;
            }

            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "recv", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }
    }

    void NetworkStream::sendTo(int fd, const void* ptr, ssize_t len) {
        while(true) {
#ifdef __WIN32__
            ssize_t real = send(fd, ptr, len, 0);
#else
            ssize_t real = send(fd, ptr, len, MSG_NOSIGNAL);
#endif

            if(len == real) {
                break;
            }

            if(0 < real && real < len) {
                ptr = static_cast<const uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
            if(0 == real) {
                Application::warning("{}: {}", NS_FuncNameV, "end stream");
                throw network_error(NS_FuncNameS);
            }

            // error
            if(EAGAIN == errno || EINTR == errno) {
                continue;
            }

            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "send", strerror(errno), errno);
            throw network_error(NS_FuncNameS);
        }
    }

    /* SocketStream */
    SocketStream::SocketStream(int fd, bool statistic) : sock(fd) {
        useStatistic(statistic);
    }

    SocketStream::~SocketStream() {
        reset();
    }

    void SocketStream::reset(void) {
        if(0 <= sock) {
#ifdef __WIN32__
            shutdown(sock, SD_BOTH);
#else
            shutdown(sock, SHUT_RDWR);
#endif
            close(sock);
            sock = -1;
        }
    }

    bool SocketStream::hasInput(void) const {
        return NetworkStream::hasInput(sock);
    }

    size_t SocketStream::hasData(void) const {
        return NetworkStream::hasData(sock);
    }

    void SocketStream::recvRaw(void* ptr, size_t len) const {
        recvFrom(sock, ptr, len);
        incrBytesIn(len);
    }

    void SocketStream::sendRaw(const void* ptr, size_t len) {
        assertm(ptr && len, "invalid pointer");
        sendTo(sock, ptr, len);
        incrBytesOut(len);
    }

    /* InetStream */
    InetStream::InetStream() : SocketStream(dup(STDIN_FILENO)) {
    }

    /* ProxySocket */
    ProxySocket::~ProxySocket() {
        proxyShutdown();
    }

    void ProxySocket::proxyShutdown(void) {
        Application::info("{}: client {}, bridge: {}", NS_FuncNameV, clientSock, bridgeSock);
        loopTransmission = false;
        SocketStream::reset();

        if(0 < bridgeSock) {
            close(bridgeSock);
            bridgeSock = -1;
        }

        if(0 < clientSock) {
            close(clientSock);
            clientSock = -1;
        }

        if(loopThread.joinable()) {
            loopThread.join();
        }

        std::error_code err;
        std::filesystem::remove(socketPath, err);

        if(err) {
            Application::warning("{}: {} failed, code: {}, error: {}, path: `{}'",
                    NS_FuncNameV, "remove", err.value(), err.message(), socketPath.string());
        }
    }

    int ProxySocket::proxyClientSocket(void) const {
        return clientSock;
    }

    bool ProxySocket::proxyRunning(void) const {
        return loopTransmission;
    }

    void ProxySocket::proxyStopEventLoop(void) {
        loopTransmission = false;
    }

    void ProxySocket::proxyStartEventLoop(void) {
        loopTransmission = true;
        Application::notice("{}: client: {}, bridge: {}", NS_FuncNameV, clientSock, bridgeSock);
        loopThread = std::thread([this] {
            while(this->loopTransmission) {
                try {
                    if(! this->transmitDataIteration()) {
                        this->loopTransmission = false;
                    }
                } catch(const std::exception & err) {
                    Application::error("proxy exception: {}", err.what());
                    this->loopTransmission = false;
                } catch(...) {
                    this->loopTransmission = false;
                }

                std::this_thread::sleep_for(1ms);
            }
            Application::notice("{}: client {}, bridge: {}", "proxy stopped", this->clientSock, this->bridgeSock);
        });
    }

    bool ProxySocket::transmitDataIteration(void) {
        if(! SocketStream::isValid()) {
            return false;
        }

        size_t dataSz = 0;

        // inetFd -> bridgeSock
        if(SocketStream::hasInput()) {
            dataSz = SocketStream::hasData();

            if(0 < dataSz) {
                auto buf = recvData(dataSz);
                sendTo(bridgeSock, buf.data(), buf.size());

                if(Application::isDebugLevel(DebugLevel::Trace)) {
                    std::string str = Tools::hexString(buf, 2);
                    Application::trace(DebugType::Sock, "from remote: [{}]", str);
                }
            }
        }

        if(! SocketStream::isValid()) {
            return false;
        }

        // bridgeSock -> inetFd
        if(NetworkStream::hasInput(bridgeSock)) {
            dataSz = NetworkStream::hasData(bridgeSock);

            if(0 < dataSz) {
                std::vector<uint8_t> buf(dataSz);
                recvFrom(bridgeSock, buf.data(), buf.size());
                sendRaw(buf.data(), buf.size());
                sendFlush();

                if(Application::isDebugLevel(DebugLevel::Trace)) {
                    std::string str = Tools::hexString(buf, 2);
                    Application::trace(DebugType::Sock, "from local: [{}]", str);
                }
            }
        }

        // no action
        if(dataSz == 0) {
            std::this_thread::sleep_for(1ms);
        }

        return true;
    }

#ifdef __UNIX__
    int TCPSocket::listen(uint16_t port, int conn) {
        return listen("any", port, conn);
    }

    int TCPSocket::listen(const std::string & ipaddr, uint16_t port, int conn) {
        int fd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        if(0 > fd) {
            Application::error("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "socket", strerror(errno), errno, ipaddr, port);
            return -1;
        }

        int reuse = 1;
        int err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, & reuse, sizeof(reuse));

        if(0 > err) {
            Application::warning("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "socket reuseaddr", strerror(errno), err, ipaddr, port);
        }

        struct sockaddr_in sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_in));

        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        sockaddr.sin_addr.s_addr = ipaddr == "any" ? htonl(INADDR_ANY) : inet_addr(ipaddr.c_str());

        Application::debug(DebugType::Sock, "{}: bind addr: `{}', port: {}", NS_FuncNameV, ipaddr, port);

        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_in))) {
            Application::error("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "bind", strerror(errno), errno, ipaddr, port);
            return -1;
        }

        Application::debug(DebugType::Sock, "{}: listen: {}, conn: {}", NS_FuncNameV, fd, conn);

        if(0 != ::listen(fd, conn)) {
            Application::error("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "listen", strerror(errno), errno, ipaddr, port);
            close(fd);
            return -1;
        }

        return fd;
    }

    int TCPSocket::accept(int fd) {
        int sock = ::accept(fd, nullptr, nullptr);

        if(0 > sock) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "accept", strerror(errno), errno);
        } else {
            Application::debug(DebugType::Sock, "{}: conected client, fd: {}", NS_FuncNameV, sock);
        }

        return sock;
    }

    std::string TCPSocket::resolvAddress(const std::string & ipaddr) {
        struct in_addr in;

        if(0 == inet_aton(ipaddr.c_str(), &in)) {
            Application::error("{}: invalid ip address: `{}'", NS_FuncNameV, ipaddr);
        } else {
            std::vector<char> strbuf(1024, 0);
            struct hostent st = {};
            struct hostent* res = nullptr;
            int h_errnop = 0;

            if(0 == gethostbyaddr_r(& in.s_addr, sizeof(in.s_addr), AF_INET, & st, strbuf.data(), strbuf.size(), & res, & h_errnop)) {
                if(res) {
                    return std::string(res->h_name);
                }
            } else {
                Application::error("{}: error: {}, ipaddr: `{}'", NS_FuncNameV, hstrerror(h_errno), ipaddr);
            }
        }

        return "";
    }

    std::list<std::string> TCPSocket::resolvHostname2(const std::string & hostname) {
        std::list<std::string> list;
        std::vector<char> strbuf(1024, 0);

        struct hostent st = {};
        struct hostent* res = nullptr;
        int h_errnop = 0;

        if(0 == gethostbyname_r(hostname.c_str(), & st, strbuf.data(), strbuf.size(), & res, & h_errnop)) {
            if(res) {
                struct in_addr in;

                while(res->h_addr_list && *res->h_addr_list) {
                    std::copy_n(*res->h_addr_list, sizeof(in_addr), (char*) & in);
                    list.emplace_back(inet_ntoa(in));
                    res->h_addr_list++;
                }
            }
        } else {
            Application::error("{}: error: {}, hostname: `{}'", NS_FuncNameV, hstrerror(h_errno), hostname);
        }

        return list;
    }

    std::string TCPSocket::resolvHostname(const std::string & hostname) {
        std::vector<char> strbuf(1024, 0);

        struct hostent st = {};
        struct hostent* res = nullptr;
        int h_errnop = 0;

        if(0 == gethostbyname_r(hostname.c_str(), & st, strbuf.data(), strbuf.size(), & res, & h_errnop)) {
            if(res) {
                struct in_addr in;

                if(res->h_addr_list && *res->h_addr_list) {
                    std::copy_n(*res->h_addr_list, sizeof(in_addr), (char*) & in);
                    return std::string(inet_ntoa(in));
                }
            }
        } else {
            Application::error("{}: error: {}, hostname: `{}'", NS_FuncNameV, hstrerror(h_errno), hostname);
        }

        return "";
    }
#endif // __UNIX__

#if defined(__WIN32__) || defined(__APPLE__)
    std::string TCPSocket::resolvHostname(const std::string & hostname) {
        if(auto res = gethostbyname(hostname.c_str())) {
            struct in_addr in;

            if(res->h_addr_list && *res->h_addr_list) {
                std::copy_n(*res->h_addr_list, sizeof(in_addr), (char*) & in);
                return std::string(inet_ntoa(in));
            }
        } else {
            Application::error("{}: error: {}, hostname: `{}'", NS_FuncNameV, "gethostbyname", hostname);
        }

        return "";
    }
#endif // __WIN32__

    int TCPSocket::connect(const std::string & ipaddr, uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);

        if(0 > sock) {
            Application::error("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "socket", strerror(errno), errno, ipaddr, port);
            return -1;
        }

        struct sockaddr_in sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_in));
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_addr.s_addr = inet_addr(ipaddr.c_str());
        sockaddr.sin_port = htons(port);

        Application::debug(DebugType::Sock, "{}: ipaddr: `{}', port: {}", NS_FuncNameV, ipaddr, port);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_in))) {
            Application::error("{}: {} failed, error: {}, code: {}, addr `{}', port: {}", NS_FuncNameV, "connect", strerror(errno), errno, ipaddr, port);
            close(sock);
            sock = -1;
        } else {
            Application::debug(DebugType::Sock, "{}: fd: {}", NS_FuncNameV, sock);
        }

        return sock;
    }

#ifdef __UNIX__
    int UnixSocket::connect(const std::filesystem::path & path) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        if(0 > sock) {
            Application::error("{}: {} failed, error: {}, code: {}, path: `{}'", NS_FuncNameV, "socket", strerror(errno), errno, path);
            return -1;
        }

        struct sockaddr_un sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;

        const std::string & native = path.native();

        if(native.size() > sizeof(sockaddr.sun_path) - 1) {
            Application::warning("{}: unix path is long, truncated to size: {}", NS_FuncNameV, sizeof(sockaddr.sun_path) - 1);
        }

        std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);

        Application::debug(DebugType::Sock, "{}: path: {}", NS_FuncNameV, sockaddr.sun_path);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un))) {
            Application::error("{}: {} failed, error: {}, code: {}, path: `{}'", NS_FuncNameV, "connect", strerror(errno), errno, path);
            close(sock);
            sock = -1;
        } else {
            Application::debug(DebugType::Sock, "{}: fd: {}", NS_FuncNameV, sock);
        }

        return sock;
    }

    int UnixSocket::listen(const std::filesystem::path & path, int conn) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        if(0 > fd) {
            Application::error("{}: {} failed, error: {}, code: {}, path: `{}'", NS_FuncNameV, "socket", strerror(errno), errno, path);
            return -1;
        }

        std::error_code err;
        std::filesystem::remove(path, err);

        if(err) {
            Application::warning("{}: {} failed, code: {}, error: {}, path: `{}'",
                    NS_FuncNameV, "remove", err.value(), err.message(), path.string());
        }

        struct sockaddr_un sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;
        const std::string & native = path.native();

        if(native.size() > sizeof(sockaddr.sun_path) - 1) {
            Application::warning("{}: unix path is long, truncated to size: {}", NS_FuncNameV, sizeof(sockaddr.sun_path) - 1);
        }

        std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);

        Application::debug(DebugType::Sock, "{}: bind path: {}", NS_FuncNameV, sockaddr.sun_path);

        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un))) {
            Application::error("{}: {} failed, error: {}, code: {}, path: `{}'", NS_FuncNameV, "bind", strerror(errno), errno, path);
            close(fd);
            return -1;
        }

        Application::debug(DebugType::Sock, "{}: listen: {}, conn: {}", NS_FuncNameV, fd, conn);

        if(0 != ::listen(fd, conn)) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "listen", strerror(errno), errno);
            close(fd);
            return -1;
        }

        return fd;
    }

    int UnixSocket::accept(int fd) {
        int sock = ::accept(fd, nullptr, nullptr);

        if(0 > sock) {
            Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "accept", strerror(errno), errno);
        } else {
            Application::debug(DebugType::Sock, "{}: conected client, fd: {}", NS_FuncNameV, sock);
        }

        return sock;
    }

    bool ProxySocket::proxyInitUnixSockets(const std::filesystem::path & path) {
        int srvfd = UnixSocket::listen(path);

        if(0 > srvfd) {
            return false;
        }

        std::error_code err;

        if(! std::filesystem::is_socket(path, err)) {
            Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                    NS_FuncNameV, "is_socket", err.value(), err.message(), path.string());
            return false;
        }

        socketPath = path;
        std::promise<int> promise;
        auto job = promise.get_future();

        std::thread([srvfd,promise=std::move(promise)]() mutable {
            promise.set_value(UnixSocket::accept(srvfd));
        }).detach();

        bridgeSock = -1;
        // socket fd: client part
        clientSock = UnixSocket::connect(socketPath);

        if(0 > clientSock) {
            close(srvfd);
            return false;
        }

        // socket fd: server part
        bridgeSock = job.get();
        close(srvfd);

        if(0 > bridgeSock) {
            return false;
        }

        fcntl(bridgeSock, F_SETFL, fcntl(bridgeSock, F_GETFL, 0) | O_NONBLOCK);
        return true;
    }
#endif // __UNIX__
} // LTSM
