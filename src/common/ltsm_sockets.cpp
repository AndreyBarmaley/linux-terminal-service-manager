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

#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef LTSM_WITH_GNUTLS
#include "gnutls/gnutls.h"
#include "gnutls/crypto.h"
#endif

#include <poll.h>
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

namespace LTSM
{
    /* NetworkStream */
    bool NetworkStream::hasInput(int fd, int timeoutMS /* 1ms */)
    {
        if(0 > fd)
        {
            return false;
        }

        struct pollfd fds = {};

        fds.fd = fd;

        fds.events = POLLIN;

        int ret = poll(& fds, 1, timeoutMS);

        // A value of 0 indicates that the call timed out and no file descriptors were ready
        if(0 == ret)
        {
            return false;
        }

        if(0 < ret && (fds.revents & POLLIN))
        {
            return true;
        }

        // interrupted system call
        if(errno == EINTR)
        {
            return hasInput(fd, timeoutMS);
        }

        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "pool", strerror(errno), errno);
        throw network_error(NS_FuncName);
    }

    size_t NetworkStream::hasData(int fd)
    {
        if(0 > fd)
        {
            return 0;
        }

        int count;

        if(0 > ioctl(fd, FIONREAD, & count))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "ioctl", strerror(errno), errno);
            throw network_error(NS_FuncName);
        }

        return count < 0 ? 0 : count;
    }

    NetworkStream & NetworkStream::sendInt8(uint8_t val)
    {
        sendRaw(& val, 1);
        return *this;
    }

    NetworkStream & NetworkStream::sendInt16(uint16_t val)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE16(val);
#else
        return sendIntBE16(val);
#endif
    }

    NetworkStream & NetworkStream::sendInt32(uint32_t val)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE32(val);
#else
        return sendIntBE32(val);
#endif
    }

    NetworkStream & NetworkStream::sendInt64(uint64_t val)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return sendIntLE64(val);
#else
        return sendIntBE64(val);
#endif
    }

    uint8_t NetworkStream::recvInt8(void) const
    {
        uint8_t byte;
        recvData(& byte, 1);
        return byte;
    }

    uint16_t NetworkStream::recvInt16(void) const
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE16();
#else
        return recvIntBE16();
#endif
    }

    uint32_t NetworkStream::recvInt32(void) const
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE32();
#else
        return recvIntBE32();
#endif
    }

    uint64_t NetworkStream::recvInt64(void) const
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return recvIntLE64();
#else
        return recvIntBE64();
#endif
    }

    void NetworkStream::recvSkip(size_t length) const
    {
        while(length--)
        {
            recvInt8();
        }
    }

    NetworkStream & NetworkStream::sendZero(size_t length)
    {
        while(length--)
        {
            sendInt8(0);
        }

        return *this;
    }

    NetworkStream & NetworkStream::sendData(const std::vector<uint8_t> & v)
    {
        sendRaw(v.data(), v.size());
        return *this;
    }

    NetworkStream & NetworkStream::sendString(std::string_view str)
    {
        sendRaw(str.data(), str.size());
        return *this;
    }

    std::vector<uint8_t> NetworkStream::recvData(size_t length) const
    {
        std::vector<uint8_t> res(length, 0);

        if(length) { recvData(res.data(), res.size()); }

        return res;
    }

    void NetworkStream::recvData(void* ptr, size_t len) const
    {
        recvRaw(ptr, len);
    }

    std::string NetworkStream::recvString(size_t length) const
    {
        std::string res(length, 0);

        if(length) { recvData(& res[0], length); }

        return res;
    }

    void NetworkStream::recvFrom(int fd, void* ptr, ssize_t len)
    {
        while(true)
        {
            ssize_t real = recv(fd, ptr, len, 0);

            if(len == real)
            {
                break;
            }

            if(0 < real && real < len)
            {
                ptr = static_cast<uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
            if(0 == real)
            {
                Application::warning("%s: %s", __FUNCTION__, "end stream");
                throw network_error(NS_FuncName);
            }

            // error
            if(EAGAIN == errno || EINTR == errno)
            {
                continue;
            }

            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "recv", strerror(errno), errno);
            throw network_error(NS_FuncName);
        }
    }

    void NetworkStream::sendTo(int fd, const void* ptr, ssize_t len)
    {
        while(true)
        {
            ssize_t real = send(fd, ptr, len, MSG_NOSIGNAL);

            if(len == real)
            {
                break;
            }

            if(0 < real && real < len)
            {
                ptr = static_cast<const uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
            if(0 == real)
            {
                Application::warning("%s: %s", __FUNCTION__, "end stream");
                throw network_error(NS_FuncName);
            }

            // error
            if(EAGAIN == errno || EINTR == errno)
            {
                continue;
            }

            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "send", strerror(errno), errno);
            throw network_error(NS_FuncName);
        }
    }

    /* SocketStream */
    SocketStream::SocketStream(int fd) : sock(fd)
    {
        buf.reserve(tcpMSS);
    }

    SocketStream::~SocketStream()
    {
        if(0 < sock)
        {
            close(sock);
        }
    }

#ifdef LTSM_WITH_GNUTLS
    void SocketStream::setupTLS(gnutls::session* sess) const
    {
        sess->set_transport_ptr(reinterpret_cast<gnutls_transport_ptr_t>(sock));
    }

#endif

    bool SocketStream::hasInput(void) const
    {
        return NetworkStream::hasInput(sock);
    }

    size_t SocketStream::hasData(void) const
    {
        return NetworkStream::hasData(sock);
    }

    void SocketStream::recvRaw(void* ptr, size_t len) const
    {
        recvFrom(sock, ptr, len);
    }

    void SocketStream::sendRaw(const void* ptr, size_t len)
    {
        if(ptr && len)
        {
            if(buf.size() + len < tcpMSS)
            {
                auto beg = static_cast<const uint8_t*>(ptr);
                buf.insert(buf.end(), beg, beg + len);
            }
            else
            {
                size_t last = tcpMSS - buf.size();
                auto beg = static_cast<const uint8_t*>(ptr);
                buf.insert(buf.end(), beg, beg + last);

                sendTo(sock, buf.data(), buf.size());
                buf.clear();

                buf.insert(buf.end(), beg + last, beg + len);
            }
        }
    }

    void SocketStream::sendFlush(void)
    {
        if(buf.size())
        {
            sendTo(sock, buf.data(), buf.size());
            buf.clear();
        }
    }

    uint8_t SocketStream::peekInt8(void) const
    {
        uint8_t res = 0;

        if(1 != recv(sock, & res, 1, MSG_PEEK))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "recv", strerror(errno), errno);
            throw network_error(NS_FuncName);
        }

        return res;
    }

    /* InetStream */
    InetStream::InetStream()
    {
        fdin = dup(fileno(stdin));
        fdout = dup(fileno(stdout));
        fin.reset(fdopen(fdin, "rb"));
        fout.reset(fdopen(fdout, "wb"));
        // reset buffering
        std::setvbuf(fin.get(), nullptr, _IONBF, 0);
        std::clearerr(fin.get());
        // set buffering, optimal for tcp mtu size
        std::setvbuf(fout.get(), fdbuf.data(), _IOFBF, fdbuf.size());
        std::clearerr(fout.get());
    }

    void InetStream::inetFdClose(void)
    {
        fin.reset();
        fout.reset();
    }

#ifdef LTSM_WITH_GNUTLS
    void InetStream::setupTLS(gnutls::session* sess) const
    {
        sess->set_transport_ptr(reinterpret_cast<gnutls_transport_ptr_t>(fdin),
                                reinterpret_cast<gnutls_transport_ptr_t>(fdout));
    }

#endif

    bool InetStream::hasInput(void) const
    {
        return ! fin || std::feof(fin.get()) || std::ferror(fin.get()) ?
               false : NetworkStream::hasInput(fdin);
    }

    size_t InetStream::hasData(void) const
    {
        return NetworkStream::hasData(fdin);
    }

    void InetStream::recvRaw(void* ptr, size_t len) const
    {
        while(true)
        {
            size_t real = std::fread(ptr, 1, len, fin.get());

            if(len == real)
            {
                break;
            }

            if(std::feof(fin.get()))
            {
                Application::warning("%s: %s", __FUNCTION__, "end stream");
                throw network_error(NS_FuncName);
            }

            if(std::ferror(fin.get()))
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fread", strerror(errno), errno);
                throw network_error(NS_FuncName);
            }

            ptr = static_cast<uint8_t*>(ptr) + real;
            len -= real;
        }
    }

    void InetStream::sendRaw(const void* ptr, size_t len)
    {
        while(true)
        {
            size_t real = std::fwrite(ptr, 1, len, fout.get());

            if(len == real)
            {
                break;
            }

            if(std::feof(fout.get()))
            {
                Application::warning("%s: %s", __FUNCTION__, "end stream");
                throw network_error(NS_FuncName);
            }

            if(std::ferror(fout.get()))
            {
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fwrite", strerror(errno), errno);
                throw network_error(NS_FuncName);
            }

            ptr = static_cast<const uint8_t*>(ptr) + real;
            len -= real;
        }
    }

    void InetStream::sendFlush(void)
    {
        std::fflush(fout.get());
    }

    bool InetStream::checkError(void) const
    {
        return ! fin || ! fout || std::ferror(fin.get()) || std::ferror(fout.get()) ||
               std::feof(fin.get()) || std::feof(fout.get());
    }

    uint8_t InetStream::peekInt8(void) const
    {
        int res = std::fgetc(fin.get());

        if(std::feof(fin.get()))
        {
            Application::warning("%s: %s", __FUNCTION__, "end stream");
            throw network_error(NS_FuncName);
        }

        if(std::ferror(fin.get()))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "fgetc", strerror(errno), errno);
            throw network_error(NS_FuncName);
        }

        std::ungetc(res, fin.get());
        return static_cast<uint8_t>(res);
    }

    /* ProxySocket */
    ProxySocket::~ProxySocket()
    {
        proxyShutdown();
    }

    void ProxySocket::proxyShutdown(void)
    {
        Application::info("%s: client %d, bridge: %d", __FUNCTION__, clientSock, bridgeSock);
        loopTransmission = false;
        inetFdClose();

        if(0 < bridgeSock)
        {
            close(bridgeSock);
            bridgeSock = -1;
        }

        if(0 < clientSock)
        {
            close(clientSock);
            clientSock = -1;
        }

        if(loopThread.joinable()) { loopThread.join(); }

        std::error_code err;
        std::filesystem::remove(socketPath, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), socketPath.c_str(), getuid());
        }
    }

    int ProxySocket::proxyClientSocket(void) const
    {
        return clientSock;
    }

    bool ProxySocket::proxyRunning(void) const
    {
        return loopTransmission;
    }

    void ProxySocket::proxyStopEventLoop(void)
    {
        loopTransmission = false;
    }

    void ProxySocket::proxyStartEventLoop(void)
    {
        loopTransmission = true;
        Application::notice("%s: client: %d, bridge: %d", __FUNCTION__, clientSock, bridgeSock);
        loopThread = std::thread([this]
        {
            while(this->loopTransmission)
            {
                try
                {
                    if(! this->transmitDataIteration())
                    {
                        this->loopTransmission = false;
                    }
                }
                catch(const std::exception & err)
                {
                    Application::error("proxy exception: %s", err.what());
                    this->loopTransmission = false;
                }
                catch(...)
                {
                    this->loopTransmission = false;
                }

                std::this_thread::sleep_for(1ms);
            }
            Application::notice("%s: client %d, bridge: %d", "proxy stopped", this->clientSock, this->bridgeSock);
        });
    }

    bool ProxySocket::transmitDataIteration(void)
    {
        if(! fin || std::feof(fin.get()) || std::ferror(fin.get()))
        {
            return false;
        }

        size_t dataSz = 0;

        // inetFd -> bridgeSock
        if(NetworkStream::hasInput(fdin) &&
                0 < (dataSz = NetworkStream::hasData(fdin)))
        {

            auto buf = recvData(dataSz);
            sendTo(bridgeSock, buf.data(), buf.size());
#ifdef LTSM_DEBUG

            if(! checkError() && Application::isDebugLevel(DebugLevel::Trace))
            {
                std::string str = Tools::buffer2hexstring(buf.begin(), buf.end(), 2);
                Application::debug("from remote: [%s]", str.c_str());
            }

#endif
        }

        if(! fout || std::feof(fout.get()) || std::ferror(fout.get()))
        {
            return false;
        }

        // bridgeSock -> inetFd
        if(NetworkStream::hasInput(bridgeSock) &&
                0 < (dataSz = NetworkStream::hasData(bridgeSock)))
        {
            std::vector<uint8_t> buf(dataSz);
            recvFrom(bridgeSock, buf.data(), buf.size());
            sendRaw(buf.data(), buf.size());
            sendFlush();
#ifdef LTSM_DEBUG

            if(! checkError() && Application::isDebugLevel(DebugLevel::Trace))
            {
                std::string str = Tools::buffer2hexstring(buf.begin(), buf.end(), 2);
                Application::debug("from local: [%s]", str.c_str());
            }

#endif
        }

        // no action
        if(dataSz == 0)
        {
            std::this_thread::sleep_for(1ms);
        }

        return true;
    }

    int TCPSocket::listen(uint16_t port, int conn)
    {
        return listen("any", port, conn);
    }

    int TCPSocket::listen(std::string_view ipaddr, uint16_t port, int conn)
    {
        int fd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        if(0 > fd)
        {
            Application::error("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "socket", strerror(errno), errno, ipaddr.data(), port);
            return -1;
        }

        int reuse = 1;
        int err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, & reuse, sizeof(reuse));

        if(0 > err)
        {
            Application::warning("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "socket reuseaddr", strerror(errno), err, ipaddr.data(), port);
        }

        struct sockaddr_in sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_in));

        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        sockaddr.sin_addr.s_addr = ipaddr == "any" ? htonl(INADDR_ANY) : inet_addr(ipaddr.data());

        Application::debug("%s: bind addr: %s, port: %" PRIu16, __FUNCTION__, ipaddr.data(), port);

        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_in)))
        {
            Application::error("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "bind", strerror(errno), errno, ipaddr.data(), port);
            return -1;
        }

        Application::debug("%s: listen: %d, conn: %d", __FUNCTION__, fd, conn);

        if(0 != ::listen(fd, conn))
        {
            Application::error("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "listen", strerror(errno), errno, ipaddr.data(), port);
            close(fd);
            return -1;
        }

        return fd;
    }

    int TCPSocket::accept(int fd)
    {
        int sock = ::accept(fd, nullptr, nullptr);

        if(0 > sock)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "accept", strerror(errno), errno);
        }
        else
        {
            Application::debug("%s: conected client, fd: %d", __FUNCTION__, sock);
        }

        return sock;
    }

    int TCPSocket::connect(std::string_view ipaddr, uint16_t port)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);

        if(0 > sock)
        {
            Application::error("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "socket", strerror(errno), errno, ipaddr.data(), port);
            return -1;
        }

        struct sockaddr_in sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_in));
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_addr.s_addr = inet_addr(ipaddr.data());
        sockaddr.sin_port = htons(port);

        Application::debug("%s: ipaddr: %s, port: %" PRIu16, __FUNCTION__, ipaddr.data(), port);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_in)))
        {
            Application::error("%s: %s failed, error: %s, code: %d, addr `%s', port: %" PRIu16, __FUNCTION__, "connect", strerror(errno), errno, ipaddr.data(), port);
            close(sock);
            sock = -1;
        }
        else
        {
            Application::debug("%s: fd: %d", __FUNCTION__, sock);
        }

        return sock;
    }

    std::string TCPSocket::resolvAddress(std::string_view ipaddr)
    {
        struct in_addr in;

        if(0 == inet_aton(ipaddr.data(), &in))
        {
            Application::error("%s: invalid ip address: %s", __FUNCTION__, ipaddr.data());
        }
        else
        {
            std::vector<char> strbuf(1024, 0);
            struct hostent st = {};
            struct hostent* res = nullptr;
            int h_errnop = 0;

            if(0 == gethostbyaddr_r(& in.s_addr, sizeof(in.s_addr), AF_INET, & st, strbuf.data(), strbuf.size(), & res, & h_errnop))
            {
                if(res)
                {
                    return std::string(res->h_name);
                }
            }
            else
            {
                Application::error("%s: error: %s, ipaddr: %s", __FUNCTION__, hstrerror(h_errno), ipaddr.data());
            }
        }

        return "";
    }

    std::list<std::string> TCPSocket::resolvHostname2(std::string_view hostname)
    {
        std::list<std::string> list;
        std::vector<char> strbuf(1024, 0);

        struct hostent st = {};
        struct hostent* res = nullptr;
        int h_errnop = 0;

        if(0 == gethostbyname_r(hostname.data(), & st, strbuf.data(), strbuf.size(), & res, & h_errnop))
        {
            if(res)
            {
                struct in_addr in;

                while(res->h_addr_list && *res->h_addr_list)
                {
                    std::copy_n(*res->h_addr_list, sizeof(in_addr), (char*) & in);
                    list.emplace_back(inet_ntoa(in));
                    res->h_addr_list++;
                }
            }
        }
        else
        {
            Application::error("%s: error: %s, hostname: %s", __FUNCTION__, hstrerror(h_errno), hostname.data());
        }

        return list;
    }

    std::string TCPSocket::resolvHostname(std::string_view hostname)
    {
        std::vector<char> strbuf(1024, 0);

        struct hostent st = {};
        struct hostent* res = nullptr;
        int h_errnop = 0;

        if(0 == gethostbyname_r(hostname.data(), & st, strbuf.data(), strbuf.size(), & res, & h_errnop))
        {
            if(res)
            {
                struct in_addr in;

                if(res->h_addr_list && *res->h_addr_list)
                {
                    std::copy_n(*res->h_addr_list, sizeof(in_addr), (char*) & in);
                    return std::string(inet_ntoa(in));
                }
            }
        }
        else
        {
            Application::error("%s: error: %s, hostname: %s", __FUNCTION__, hstrerror(h_errno), hostname.data());
        }

        return "";
    }

    int UnixSocket::connect(const std::filesystem::path & path)
    {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        if(0 > sock)
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "socket", strerror(errno), errno, path.c_str());
            return -1;
        }

        struct sockaddr_un sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;

        const std::string & native = path.native();

        if(native.size() > sizeof(sockaddr.sun_path) - 1)
        {
            Application::warning("%s: unix path is long, truncated to size: %d", __FUNCTION__, sizeof(sockaddr.sun_path) - 1);
        }

        std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);

        Application::debug("%s: path: %s", __FUNCTION__, sockaddr.sun_path);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un)))
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "connect", strerror(errno), errno, path.c_str());
            close(sock);
            sock = -1;
        }
        else
        {
            Application::debug("%s: fd: %d", __FUNCTION__, sock);
        }

        return sock;
    }

    int UnixSocket::listen(const std::filesystem::path & path, int conn)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        if(0 > fd)
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "socket", strerror(errno), errno, path.c_str());
            return -1;
        }

        std::error_code err;
        std::filesystem::remove(path, err);

        if(err)
        {
            Application::warning("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), path.c_str(), getuid());
        }

        struct sockaddr_un sockaddr;

        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;
        const std::string & native = path.native();

        if(native.size() > sizeof(sockaddr.sun_path) - 1)
        {
            Application::warning("%s: unix path is long, truncated to size: %d", __FUNCTION__, sizeof(sockaddr.sun_path) - 1);
        }

        std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);

        Application::debug("%s: bind path: %s", __FUNCTION__, sockaddr.sun_path);

        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un)))
        {
            Application::error("%s: %s failed, error: %s, code: %d, path: `%s'", __FUNCTION__, "bind", strerror(errno), errno, path.c_str());
            close(fd);
            return -1;
        }

        Application::debug("%s: listen: %d, conn: %d", __FUNCTION__, fd, conn);

        if(0 != ::listen(fd, conn))
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "listen", strerror(errno), errno);
            close(fd);
            return -1;
        }

        return fd;
    }

    int UnixSocket::accept(int fd)
    {
        int sock = ::accept(fd, nullptr, nullptr);

        if(0 > sock)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "accept", strerror(errno), errno);
        }
        else
        {
            Application::debug("%s: conected client, fd: %d", __FUNCTION__, sock);
        }

        return sock;
    }

    bool ProxySocket::proxyInitUnixSockets(const std::filesystem::path & path)
    {
        int srvfd = UnixSocket::listen(path);

        if(0 > srvfd)
        {
            return false;
        }

        std::error_code err;

        if(! std::filesystem::is_socket(path, err))
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not socket"), path.c_str(), getuid());
            return false;
        }

        socketPath = path;
        std::future<int> job = std::async(std::launch::async, UnixSocket::accept, srvfd);
        bridgeSock = -1;
        // socket fd: client part
        clientSock = UnixSocket::connect(socketPath);

        if(0 > clientSock)
        {
            close(srvfd);
            return false;
        }

        while(job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);

        // socket fd: server part
        bridgeSock = job.get();
        close(srvfd);

        if(0 > bridgeSock)
        {
            return false;
        }

        fcntl(bridgeSock, F_SETFL, fcntl(bridgeSock, F_GETFL, 0) | O_NONBLOCK);
        return true;
    }

#ifdef LTSM_WITH_GNUTLS
    namespace TLS
    {
        void gnutls_log(int level, const char* str)
        {
            // remove end line
            if(size_t len = strlen(str))
            {
                Application::info("gnutls debug: %.*s", len-1, str);
            }
        }

        /* TLS::Stream */
        Stream::Stream(const NetworkStream* bs) : layer(bs)
        {
            if(! bs)
            {
                Application::error("%s: %s", __FUNCTION__, "tls stream failed");
                throw std::invalid_argument(NS_FuncName);
            }
        }

        bool Stream::startHandshake(void)
        {
            int ret = 0;
            layer->setupTLS(session.get());

            do
            {
                ret = session->handshake();
            }
            while(ret < 0 && gnutls_error_is_fatal(ret) == 0);

            if(ret < 0)
            {
                Application::error("gnutls_handshake error: %s", gnutls_strerror(ret));
                return false;
            }

            return true;
        }

        bool Stream::initAnonHandshake(std::string_view priority, bool srvmode, int debug)
        {
            Application::info("gnutls version usage: %s", GNUTLS_VERSION);

            gnutls_global_set_log_level(debug);
            gnutls_global_set_log_function(gnutls_log);

            if(priority.empty())
            {
                priority = "NORMAL:+ANON-ECDH:+ANON-DH";
            }

            if(srvmode)
            {
                Application::debug("%s: tls server mode, priority: %s", __FUNCTION__, priority.data());
                dhparams.generate(1024);
                auto ptr = new gnutls::anon_server_credentials();
                ptr->set_dh_params(dhparams);
                cred.reset(ptr);

                session = std::make_unique<gnutls::server_session>();
            }
            else
            {
                Application::debug("%s: tls client mode, priority: %s", __FUNCTION__, priority.data());
                cred = std::make_unique<gnutls::anon_client_credentials>();
                session = std::make_unique<gnutls::client_session>();
            }

            session->set_credentials(*cred.get());
            session->set_priority(priority.data(), nullptr);

            return startHandshake();
        }

        bool Stream::initX509Handshake(std::string_view priority, bool srvmode, const std::string & caFile, const std::string & certFile, const std::string & keyFile, const std::string & crlFile, int debug)
        {
            Application::info("gnutls version usage: %s", GNUTLS_VERSION);

            gnutls_global_set_log_level(debug);
            gnutls_global_set_log_function(gnutls_log);

            if(priority.empty())
            {
                priority = "NORMAL:+ANON-ECDH:+ANON-DH";
            }


            if(certFile.empty())
            {
                Application::error("%s: %s need", __FUNCTION__, "cert file");
                return false;
            }

            if(keyFile.empty())
            {
                Application::error("%s: %s need", __FUNCTION__, "key file");
                return false;
            }

            std::error_code err;

            if(! std::filesystem::exists(certFile, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), certFile.c_str(), getuid());
                return false;
            }

            if(! std::filesystem::exists(keyFile, err))
            {
                Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), keyFile.c_str(), getuid());
                return false;
            }

            if(srvmode)
            {
                cred = std::make_unique<gnutls::certificate_server_credentials>();
                auto ptr = new gnutls::server_session();
                ptr->set_certificate_request(GNUTLS_CERT_IGNORE);
                session.reset(ptr);
            }
            else
            {
                cred = std::make_unique<gnutls::certificate_client_credentials>();
                session = std::make_unique<gnutls::client_session>();
            }

            auto certcred = static_cast<gnutls::certificate_credentials*>(cred.get());

            if(! caFile.empty())
            {
                if(std::filesystem::exists(caFile, err))
                {
                    certcred->set_x509_trust_file(caFile.c_str(), GNUTLS_X509_FMT_PEM);
                }

                if(err)
                {
                    Application::warning("%s, %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), caFile.c_str(), getuid());
                }
            }

            certcred->set_x509_key_file(certFile.c_str(), keyFile.c_str(), GNUTLS_X509_FMT_PEM);

            if(! crlFile.empty())
            {
                if(std::filesystem::exists(crlFile, err))
                {
                    certcred->set_x509_crl_file(crlFile.c_str(), GNUTLS_X509_FMT_PEM);
                }

                if(err)
                {
                    Application::warning("%s, %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), crlFile.c_str(), getuid());
                }
            }

            session->set_credentials(*cred.get());
            session->set_priority(priority.data(), nullptr);

            return startHandshake();
        }

        std::string Stream::sessionDescription(void) const
        {
            auto desc = gnutls_session_get_desc(Session::ptr(session.get()));
            std::string res(desc);
            gnutls_free(desc);
            return res;
        }

        bool Stream::hasInput(void) const
        {
            // gnutls doc: 6.5.1 Asynchronous operation
            // utilize gnutls_record_check_pending, either before the poll system call
            return 0 < session->check_pending() || layer->hasInput();
        }

        size_t Stream::hasData(void) const
        {
            return session->check_pending();
        }

        void Stream::recvRaw(void* ptr, size_t len) const
        {
            if(0 <= peek)
            {
                auto buf = static_cast<uint8_t*>(ptr);
                *buf = 0xFF & peek;
                ptr = buf + 1;
                len = len - 1;
                peek = -1;
            }

            ssize_t ret = 0;

            while((ret = session->recv(ptr, len)) < 0)
            {
                if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
                {
                    continue;
                }

                break;
            }

            if(0 > ret)
            {
                Application::error("gnutls_record_recv ret: %ld, error: %s", ret, gnutls_strerror(ret));

                if(gnutls_error_is_fatal(ret))
                {
                    throw gnutls_error(NS_FuncName);
                }
            }
            else

                // eof
                if(0 == ret)
                {
                    Application::warning("%s: %s", __FUNCTION__, "end stream");
                    throw gnutls_error(NS_FuncName);
                }

            if(ret < static_cast<ssize_t>(len))
            {
                ptr = static_cast<uint8_t*>(ptr) + ret;
                len = len - ret;
                recvRaw(ptr, len);
            }
        }

        void Stream::sendRaw(const void* ptr, size_t len)
        {
            ssize_t ret = 0;

            while((ret = session->send(ptr, len)) < 0)
            {
                if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
                {
                    continue;
                }

                break;
            }

            if(ret != static_cast<ssize_t>(len))
            {
                Application::error("gnutls_record_send ret: %ld, error: %s", ret, gnutls_strerror(ret));

                if(gnutls_error_is_fatal(ret))
                {
                    throw gnutls_error(NS_FuncName);
                }
            }
        }

        uint8_t Stream::peekInt8(void) const
        {
            if(0 > peek)
            {
                uint8_t val;
                recvRaw(& val, 1);
                peek = val;
            }

            return static_cast<uint8_t>(peek);
        }

        void Stream::sendFlush(void)
        {
            // flush send data
            gnutls_record_uncork(Session::ptr(session.get()), 0);
            // cached send data
            gnutls_record_cork(Session::ptr(session.get()));
        }

        AnonSession::AnonSession(const NetworkStream* st, std::string_view priority, bool serverMode, int debug) : Stream(st)
        {
            initAnonHandshake(priority, serverMode, debug);
        }

        X509Session::X509Session(const NetworkStream* st, const std::string & cafile, const std::string & cert, const std::string & key,
                                 const std::string & crl, std::string_view priority, bool serverMode, int debug) : Stream(st)
        {
            initX509Handshake(priority, serverMode, cafile, cert, key, crl, debug);
        }

        std::vector<uint8_t> encryptDES(const std::vector<uint8_t> & data, std::string_view str)
        {
            gnutls_cipher_hd_t ctx;
            std::vector<uint8_t> res(data);
            std::array<uint8_t, 8> _key;
            std::array<uint8_t, 8> _iv;
            std::fill(_key.begin(), _key.end(), 0);
            std::fill(_iv.begin(), _iv.end(), 0);
            std::copy_n(str.begin(), std::min(str.size(), _key.size()), _key.begin());
            gnutls_datum_t key = { _key.data(), _key.size() };
            gnutls_datum_t iv = { _iv.data(), _iv.size() };

            // Reverse the order of bits in the byte
            for(auto & val : _key)
                if(val) { val = ((val * 0x0202020202ULL & 0x010884422010ULL) % 1023) & 0xfe; }

            size_t offset = 0;

            while(offset < res.size())
            {
                if(int ret = gnutls_cipher_init(& ctx, GNUTLS_CIPHER_DES_CBC, & key, & iv))
                {
                    Application::error("%s: %s error: %s", __FUNCTION__, "gnutls_cipher_init", gnutls_strerror(ret));
                    throw gnutls_error(NS_FuncName);
                }

                if(int ret = gnutls_cipher_encrypt(ctx, res.data() + offset, std::min(_key.size(), res.size() - offset)))
                {
                    Application::error("%s: %s error: %s", __FUNCTION__, "gnutls_cipher_encrypt", gnutls_strerror(ret));
                    throw gnutls_error(NS_FuncName);
                }

                gnutls_cipher_deinit(ctx);
                offset += _key.size();
            }

            return res;
        }

        std::vector<uint8_t> randomKey(size_t keysz)
        {
            std::vector<uint8_t> res(keysz);

            if(int ret = gnutls_rnd(GNUTLS_RND_KEY, res.data(), res.size()))
            {
                Application::error("%s: %s error: %s", __FUNCTION__, "gnutls_rnd", gnutls_strerror(ret));
                throw gnutls_error(NS_FuncName);
            }

            return res;
        }

    } // TLS

#endif

    namespace ZLib
    {
        /* Zlib::DeflateBase */
        DeflateBase::DeflateBase(int level)
        {
            zs.data_type = Z_BINARY;

            if(level < Z_BEST_SPEED || Z_BEST_COMPRESSION < level)
            {
                level = Z_BEST_COMPRESSION;
            }

            int ret = deflateInit(& zs, level);
            //int ret = deflateInit2(& zs, level, Z_DEFLATED, MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);

            if(ret < Z_OK)
            {
                Application::error("%s: %s failed, error code: %d", __FUNCTION__, "deflateInit2", ret);
                throw zlib_error(NS_FuncName);
            }
        }

        DeflateBase::~DeflateBase()
        {
            deflateEnd(& zs);
        }

        std::vector<uint8_t> DeflateBase::deflateData(const void* buf, size_t len, int flushPolicy)
        {
            // zlib legacy non const
            zs.next_in = (Bytef*) buf;
            zs.avail_in = len;

            std::vector<uint8_t> res;
            res.reserve(std::max(deflateBound(& zs, len), tmp.size()));

            do
            {
                zs.next_out = tmp.data();
                zs.avail_out = tmp.size();

                int ret = deflate(& zs, flushPolicy);

                if(ret < Z_OK)
                {
                    Application::error("%s: %s failed, error code: %d", __FUNCTION__, "deflate", ret);
                    throw zlib_error(NS_FuncName);
                }

                if(zs.avail_out < tmp.size())
                {
                    res.insert(res.end(), tmp.begin(), std::next(tmp.begin(), tmp.size() - zs.avail_out));
                }
            }
            while(zs.avail_out == 0);

            return res;
        }

        /* Zlib::DeflateStream */
        DeflateStream::DeflateStream(int level) : DeflateBase(level)
        {
            bb.reserve(4096);
        }

        std::vector<uint8_t> DeflateStream::deflateFlush(void)
        {
            auto last = deflateData(nullptr, 0, Z_SYNC_FLUSH);

            if(last.size())
            {
                bb.insert(bb.end(), last.begin(), last.end());
            }

            return std::move(bb);
        }

        void DeflateStream::sendRaw(const void* ptr, size_t len)
        {
            auto data = deflateData(ptr, len, Z_NO_FLUSH);

            if(data.size())
            {
                bb.insert(bb.end(), data.begin(), data.end());
            }
        }

        void DeflateStream::recvRaw(void* ptr, size_t len) const
        {
            Application::error("%s: %s", __FUNCTION__, "disabled");
            throw zlib_error(NS_FuncName);
        }

        bool DeflateStream::hasInput(void) const
        {
            Application::error("%s: %s", __FUNCTION__, "disabled");
            throw zlib_error(NS_FuncName);
        }

        size_t DeflateStream::hasData(void) const
        {
            Application::error("%s: %s", __FUNCTION__, "disabled");
            throw zlib_error(NS_FuncName);
        }

        uint8_t DeflateStream::peekInt8(void) const
        {
            Application::error("%s: %s", __FUNCTION__, "disabled");
            throw zlib_error(NS_FuncName);
        }

        /* Zlib::InflateBase */
        InflateBase::InflateBase()
        {
            zs.data_type = Z_BINARY;

            int ret = inflateInit2(& zs, MAX_WBITS);

            if(ret < Z_OK)
            {
                Application::error("%s: %s failed, error code: %d", __FUNCTION__, "inflateInit2", ret);
                throw zlib_error(NS_FuncName);
            }
        }

        InflateBase::~InflateBase()
        {
            inflateEnd(& zs);
        }

        std::vector<uint8_t> InflateBase::inflateData(const void* buf, size_t len, int flushPolicy)
        {
            std::vector<uint8_t> res;

            if(len) { res.reserve(len * 7); }

            zs.next_in = (Bytef*) buf;
            zs.avail_in = len;

            do
            {
                zs.next_out = tmp.data();
                zs.avail_out = tmp.size();
                int ret = inflate(& zs, flushPolicy);

                if(ret < Z_OK)
                {
                    Application::error("%s: %s failed, error code: %d", __FUNCTION__, "inflate", ret);
                    throw zlib_error(NS_FuncName);
                }

                if(zs.avail_out < tmp.size())
                {
                    res.insert(res.end(), tmp.begin(), std::next(tmp.begin(), tmp.size() - zs.avail_out));
                }
            }
            while(zs.avail_in > 0);

            return res;
        }

        /* Zlib::InflateStream */
        InflateStream::InflateStream() : sb(4096 /* reserve */)
        {
        }

        void InflateStream::appendData(const std::vector<uint8_t> & zip)
        {
            sb.shrink();
            sb.write(inflateData(zip.data(), zip.size(), Z_SYNC_FLUSH));
        }

        void InflateStream::recvRaw(void* ptr, size_t len) const
        {
            if(sb.last() < len)
            {
                Application::error("%s: stream last: %u, expected: %u", __FUNCTION__, sb.last(), len);
                throw std::invalid_argument(NS_FuncName);
            }

            sb.readTo(static_cast<uint8_t*>(ptr), len);
        }

        size_t InflateStream::hasData(void) const
        {
            return sb.last();
        }

        bool InflateStream::hasInput(void) const
        {
            return sb.last();
        }

        uint8_t InflateStream::peekInt8(void) const
        {
            if(0 == sb.last())
            {
                Application::error("%s: stream empty", __FUNCTION__);
                throw zlib_error(NS_FuncName);
            }

            return sb.peek();
        }

        void InflateStream::sendRaw(const void* ptr, size_t len)
        {
            Application::error("%s: %s", __FUNCTION__, "disabled");
            throw zlib_error(NS_FuncName);
        }
    } // ZLib

#ifdef LTSM_WITH_GSSAPI
    namespace GssApi
    {
        // GssApi::BaseLayer
        BaseLayer::BaseLayer(NetworkStream* st, size_t capacity) : rcvbuf(capacity), layer(st)
        {
            sndbuf.reserve(capacity);
        }

        bool BaseLayer::hasInput(void) const
        {
            if(rcvbuf.last())
            {
                return true;
            }

            if(layer && layer->hasInput())
            {
                rcvbuf.write(recvLayer());
                return true;
            }

            return false;
        }

        size_t BaseLayer::hasData(void) const
        {
            if(layer && layer->hasInput())
            {
                rcvbuf.write(recvLayer());
            }

            return rcvbuf.last();
        }

        uint8_t BaseLayer::peekInt8(void) const
        {
            return rcvbuf.peek();
        }

        void BaseLayer::recvRaw(void* data, size_t len) const
        {
            while(rcvbuf.last() < len)
            {
                rcvbuf.write(recvLayer());
            }

            rcvbuf.readTo(static_cast<uint8_t*>(data), len);
        }

        void BaseLayer::sendRaw(const void* data, size_t len)
        {
            auto ptr = static_cast<const uint8_t*>(data);

            if(sndbuf.capacity() < sndbuf.size() + len)
            {
                auto last = sndbuf.capacity() - sndbuf.size();

                sndbuf.append(ptr, last);
                sendFlush();

                ptr += last;
                len -= last;
            }

            sndbuf.append(ptr, len);
        }

        void BaseLayer::sendFlush(void)
        {
            sendLayer(sndbuf.data(), sndbuf.size());
            sndbuf.clear();
        }

        std::vector<uint8_t> BaseLayer::recvLayer(void) const
        {
            if(! layer)
            {
                Application::error("%s: %s", __FUNCTION__, "network layer is null");
                throw gssapi_error(NS_FuncName);
            }

            auto len = layer->recvIntBE32();
            return layer->recvData(len);
        }

        void BaseLayer::sendLayer(const void* buf, size_t len)
        {
            if(! layer)
            {
                Application::error("%s: %s", __FUNCTION__, "network layer is null");
                throw gssapi_error(NS_FuncName);
            }

            layer->sendIntBE32(len);
            layer->sendRaw(buf, len);
            layer->sendFlush();
        }

        // GssApi::Server
        bool Server::checkServiceCredential(std::string_view service) const
        {
            Gss::ErrorCodes err;
            auto cred = Gss::acquireServiceCredential(service, & err);

            if(cred)
            {
                return true;
            }

            error(__FUNCTION__, err.func, err.code1, err.code2);
            return false;
        }

        bool Server::handshakeLayer(std::string_view service)
        {
            Gss::ErrorCodes err;
            auto cred = Gss::acquireServiceCredential(service, & err);

            if(! cred)
            {
                error(__FUNCTION__, err.func, err.code1, err.code2);
                return true;
            }

            return Gss::ServiceContext::acceptClient(std::move(cred));
        }

        void Server::error(const char* func, const char* subfunc, OM_uint32 code1, OM_uint32 code2) const
        {
            auto err = Gss::error2str(code1, code2);
            Application::error("%s: %s failed, error: \"%s\", codes: [ 0x%08" PRIx32 ", 0x%08" PRIx32 "]", func, subfunc, err.c_str(), code1, code2);
        }

        // GssApi::Client
        bool Client::checkUserCredential(std::string_view username) const
        {
            Gss::ErrorCodes err;
            auto cred = acquireUserCredential(username, & err);

            if(cred)
            {
                return true;
            }

            error(__FUNCTION__, err.func, err.code1, err.code2);
            return false;
        }

        bool Client::handshakeLayer(std::string_view service, bool mutual, std::string_view username)
        {
            if(! username.empty())
            {
                Gss::ErrorCodes err;
                auto cred = acquireUserCredential(username, & err);

                if(cred)
                {
                    return connectService(service, mutual, std::move(cred));
                }

                error(__FUNCTION__, err.func, err.code1, err.code2);
            }

            return connectService(service, mutual);
        }

        void Client::error(const char* func, const char* subfunc, OM_uint32 code1, OM_uint32 code2) const
        {
            auto err = Gss::error2str(code1, code2);
            Application::error("%s: %s failed, error: \"%s\", codes: [ 0x%08" PRIx32 ", 0x%08" PRIx32 "]", func, subfunc, err.c_str(), code1, code2);
        }
    } // GssApi

#endif
} // LTSM
