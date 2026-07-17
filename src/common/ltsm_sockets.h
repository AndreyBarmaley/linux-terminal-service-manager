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

#ifndef _LTSM_SOCKETS_
#define _LTSM_SOCKETS_

#include <list>
#include <chrono>
#include <array>
#include <atomic>
#include <vector>
#include <thread>
#include <string>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <filesystem>

#include <zlib.h>

#include "ltsm_streambuf.h"

#ifdef LTSM_WITH_ZLIB
#include "ltsm_zlib.h"
#endif

#define LTSM_SOCKETS_VERSION 20260520

namespace LTSM {
    struct network_error : public std::runtime_error {
        explicit network_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    /// @brief: network stream interface
    class NetworkStream : protected ByteOrderInterface {
        std::chrono::steady_clock::time_point tp;

        bool showStatistic = true;
        mutable size_t bytesIn = 0;
        mutable size_t bytesOut = 0;

      protected:
        inline void getRaw(void* ptr, size_t len) const override {
            recvRaw(ptr, len);
        };

        inline void putRaw(const void* ptr, size_t len) override {
            sendRaw(ptr, len);
        };

      public:
        NetworkStream();
        virtual ~NetworkStream();

        void useStatistic(bool f) {
            showStatistic = f;
        }

        void incrBytesIn(size_t val) const {
            bytesIn += val;
        }

        void incrBytesOut(size_t val) const {
            bytesOut += val;
        }

        static bool hasInput(int fd, int timeoutMS = 1);
        static size_t hasData(int fd);

        inline NetworkStream & sendIntBE16(uint16_t x) {
            putIntBE16(x);
            return *this;
        }

        inline NetworkStream & sendIntBE32(uint32_t x) {
            putIntBE32(x);
            return *this;
        }

        inline NetworkStream & sendIntBE64(uint64_t x) {
            putIntBE64(x);
            return *this;
        }

        inline NetworkStream & sendIntLE16(uint16_t x) {
            putIntLE16(x);
            return *this;
        }

        inline NetworkStream & sendIntLE32(uint32_t x) {
            putIntLE32(x);
            return *this;
        }

        inline NetworkStream & sendIntLE64(uint64_t x) {
            putIntLE64(x);
            return *this;
        }

        NetworkStream & sendInt8(uint8_t);
        NetworkStream & sendInt16(uint16_t);
        NetworkStream & sendInt32(uint32_t);
        NetworkStream & sendInt64(uint64_t);

        NetworkStream & sendZero(size_t);
        NetworkStream & sendData(const std::vector<uint8_t> &);

        virtual void sendFlush(void) { /* default empty */ }

        virtual void sendRaw(const void*, size_t) = 0;

        virtual bool hasInput(void) const = 0;
        virtual size_t hasData(void) const = 0;

        inline uint16_t recvIntBE16(void) const {
            return getIntBE16();
        }

        inline uint32_t recvIntBE32(void) const {
            return getIntBE32();
        }

        inline uint64_t recvIntBE64(void) const {
            return getIntBE64();
        }

        inline uint16_t recvIntLE16(void) const {
            return getIntLE16();
        }

        inline uint32_t recvIntLE32(void) const {
            return getIntLE32();
        }

        inline uint64_t recvIntLE64(void) const {
            return getIntLE64();
        }

        uint8_t recvInt8(void) const;
        uint16_t recvInt16(void) const;
        uint32_t recvInt32(void) const;
        uint64_t recvInt64(void) const;

        void recvSkip(size_t) const;
        std::vector<uint8_t> recvData(size_t) const;
        void recvData(void* ptr, size_t len) const;

        virtual void recvRaw(void*, size_t) const = 0;

        NetworkStream & sendString(std::string_view);
        std::string recvString(size_t) const;

        static void sendTo(int fd, const void*, ssize_t);
        static void recvFrom(int fd, void*, ssize_t);
    };

    /// @brief: socket stream
    class SocketStream : public NetworkStream {
      protected:
        int sock = -1;

      public:
        explicit SocketStream(int fd, bool statistic = true);
        ~SocketStream();

        SocketStream(const SocketStream &) = delete;
        SocketStream & operator=(const SocketStream &) = delete;

        bool isValid(void) const {
            return 0 <= sock;
        }

        bool hasInput(void) const override;
        size_t hasData(void) const override;

        void sendRaw(const void*, size_t) override;
        void recvRaw(void*, size_t) const override;

        int fd(void) const {
            return sock;
        }
        void reset(void);
    };

    /// @brief: inetd stream
    class InetStream : public SocketStream {
      public:
        InetStream();
    };

    /// @brief: proxy socket: stdin/stdout to local socket
    class ProxySocket : protected InetStream {
      protected:
        std::atomic<bool> loopTransmission{false};
        std::thread loopThread;
        int bridgeSock = -1;
        int clientSock = -1;
        std::filesystem::path socketPath;

      protected:
        bool transmitDataIteration(void);

      public:
        ProxySocket() = default;
        ~ProxySocket();

        int proxyClientSocket(void) const;
#ifdef __UNIX__
        bool proxyInitUnixSockets(const std::filesystem::path &);
#endif
        bool proxyRunning(void) const;

        void proxyStartEventLoop(void);
        void proxyStopEventLoop(void);
        void proxyShutdown(void);
    };

    namespace TCPSocket {
        int connect(const std::string & ipaddr, uint16_t port);
        std::string resolvHostname(const std::string & hostname);
#ifdef __UNIX__
        std::string resolvAddress(const std::string & ipaddr);
        std::list<std::string> resolvHostname2(const std::string & hostname);
        int listen(uint16_t port, int conn = 5);
        int listen(const std::string & ipaddr, uint16_t port, int conn = 5);
        int accept(int fd);
#endif
    }

#ifdef __UNIX__
    namespace UnixSocket {
        int connect(const std::filesystem::path &);
        int listen(const std::filesystem::path &, int conn = 5);
        int accept(int fd);
    }
#endif
} // LTSM

#endif // _LTSM_SOCKETS_
