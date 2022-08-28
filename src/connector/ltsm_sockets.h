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

#include <array>
#include <atomic>
#include <vector>
#include <thread>
#include <string>
#include <string_view>
#include <filesystem>
#include <memory>
#include <cstdint>

#include "gnutls/gnutls.h"
#include "ltsm_streambuf.h"

#define LTSM_SOCKETS_VERSION 20220828

namespace LTSM
{
    /// @brief: network stream interface
    class NetworkStream : protected ByteOrderInterface
    {
        size_t                  rcvTimeout;

    protected:
        static bool             hasInput(int fd, int timeoutMS = 1);
        static size_t           hasData(int fd);

        inline void             getRaw(void* ptr, size_t len) const override { recvRaw(ptr, len); };
        inline void             putRaw(const void* ptr, size_t len) override { sendRaw(ptr, len); };

    public:
        NetworkStream() : rcvTimeout(0) {}
        virtual ~NetworkStream() {}

        virtual void            setupTLS(gnutls_session_t) const {}

        inline NetworkStream &  sendIntBE16(uint16_t x) { putIntBE16(x); return *this; }
        inline NetworkStream &  sendIntBE32(uint32_t x) { putIntBE32(x); return *this; }
        inline NetworkStream &  sendIntBE64(uint64_t x) { putIntBE64(x); return *this; }

        inline NetworkStream &  sendIntLE16(uint16_t x) { putIntLE16(x); return *this; }
        inline NetworkStream &  sendIntLE32(uint32_t x) { putIntLE32(x); return *this; }
        inline NetworkStream &  sendIntLE64(uint64_t x) { putIntLE64(x); return *this; }

        NetworkStream &         sendInt8(uint8_t);
        NetworkStream &         sendInt16(uint16_t);
        NetworkStream &         sendInt32(uint32_t);
        NetworkStream &         sendInt64(uint64_t);

        NetworkStream & 	sendZero(size_t);
        NetworkStream & 	sendData(const std::vector<uint8_t> &);

        virtual void            sendFlush(void) {}
        virtual void		sendRaw(const void*, size_t) = 0;

        virtual bool            hasInput(void) const = 0;
        virtual size_t          hasData(void) const = 0;
        virtual uint8_t		peekInt8(void) const = 0;

        inline uint16_t         recvIntBE16(void) const { return getIntBE16(); }
        inline uint32_t	        recvIntBE32(void) const { return getIntBE32(); }
        inline uint64_t	        recvIntBE64(void) const { return getIntBE64(); }

        inline uint16_t	        recvIntLE16(void) const { return getIntLE16(); }
        inline uint32_t	        recvIntLE32(void) const { return getIntLE32(); }
        inline uint64_t	        recvIntLE64(void) const { return getIntLE64(); }

        uint8_t		        recvInt8(void) const;
        uint16_t	        recvInt16(void) const;
        uint32_t	        recvInt32(void) const;
        uint64_t	        recvInt64(void) const;

        void                    recvSkip(size_t) const;
        std::vector<uint8_t>    recvData(size_t) const;
        void                    recvData(void* ptr, size_t len) const;

        virtual void            recvRaw(void*, size_t) const = 0;
        virtual void            recvRaw(void* ptr, size_t len, size_t timeout /* ms */) const;

        NetworkStream &         sendString(std::string_view);
        std::string	        recvString(size_t) const;

        void                    setReadTimeout(size_t ms);
    };

    namespace FileDescriptor
    {
        void			write(int fd, const void*, ssize_t);
        void	                read(int fd, void*, ssize_t);
    };

    /// @brief: socket stream
    class SocketStream : public NetworkStream
    {
    protected:
        int                     sock;
        std::vector<uint8_t>    buf;

    public:
        SocketStream(int fd = 0);
        ~SocketStream();

        void                    setupTLS(gnutls_session_t) const override;

        void                    setSocket(int fd) { sock = fd; }

        bool                    hasInput(void) const override;
        size_t                  hasData(void) const override;
        uint8_t	                peekInt8(void) const override;

        void			sendRaw(const void*, size_t) override;
        void	                recvRaw(void*, size_t) const override;

        void                    sendFlush(void) override;
    };

    /// @brief: inetd stream
    class InetStream : public NetworkStream
    {
    protected:
        FILE*                   fdin = nullptr;
        FILE*                   fdout = nullptr;
        std::array<char, 1492>  fdbuf;

        void                    inetFdClose(void);

    public:
        InetStream();
        ~InetStream();

        void                    setupTLS(gnutls_session_t) const override;

        bool                    hasInput(void) const override;
        size_t                  hasData(void) const override;
        uint8_t		        peekInt8(void) const override;

        void                    sendFlush(void) override;
        bool                    checkError(void) const;

        void			sendRaw(const void*, size_t) override;
        void	                recvRaw(void*, size_t) const override;
    };

    /// @brief: proxy socket: stdin/stdout to local socket
    class ProxySocket : protected InetStream
    {
    protected:
        std::atomic<bool>       loopTransmission;
        std::thread             loopThread;
        int                     bridgeSock;
        int                     clientSock;
        std::filesystem::path   socketPath;

    protected:
        bool                    transmitDataIteration(void);

    public:
        ProxySocket() : loopTransmission(false), bridgeSock(-1), clientSock(-1) {}
        ~ProxySocket();
            
        int                     proxyClientSocket(void) const;
        bool                    proxyInitUnixSockets(const std::filesystem::path &);
        bool                    proxyRunning(void) const;

        void                    proxyStartEventLoop(void);
        void                    proxyStopEventLoop(void);
        void                    proxyShutdown(void);

        static int              connectUnixSocket(const std::filesystem::path &);
        static int              listenUnixSocket(const std::filesystem::path &);
    };

    /// transport layer security
    namespace TLS
    {
        std::vector<uint8_t>    randomKey(size_t);
        std::vector<uint8_t>    encryptDES(const std::vector<uint8_t> & crypt, std::string_view key);

        /// @brief: tls context
        struct BaseContext
        {
            gnutls_session_t    session = nullptr;
            gnutls_dh_params_t  dhparams = nullptr;

            BaseContext(int debug = 0);
            virtual ~BaseContext();

            std::string         sessionDescription(void) const;
            virtual bool        initSession(const std::string & priority, int mode = GNUTLS_SERVER);
        };

        /// @brief: tls stream
        class Stream : public NetworkStream
        {
        protected:
            const NetworkStream* layer;
            std::unique_ptr<BaseContext> tls;
            bool                handshake = false;
            mutable int         peek = -1;

        public:
            Stream(const NetworkStream*);
            ~Stream();

            bool                initAnonHandshake(const std::string & priority, int debug);
            bool                initX509Handshake(const std::string & priority, const std::string & caFile, const std::string & certFile,
                                                        const std::string & keyFile, const std::string & crlFile, int debug);

            bool                hasInput(void) const override;
            size_t              hasData(void) const override;
            void                sendFlush(void) override;
            uint8_t	        peekInt8(void) const override;

            void		sendRaw(const void*, size_t) override;
            void                recvRaw(void*, size_t) const override;
            void                recvRaw(void* ptr, size_t len, size_t timeout /* ms */) const override;

	    std::string		sessionDescription(void) const;
        };

        struct AnonCredentials : BaseContext
        {
            gnutls_anon_server_credentials_t cred;

            AnonCredentials(int debug = 0) : BaseContext(debug), cred(nullptr) {}
            ~AnonCredentials();

            bool                initSession(const std::string & priority, int mode = GNUTLS_SERVER) override;
        };

        struct X509Credentials : BaseContext
        {
            gnutls_certificate_credentials_t cred;
            const std::string   caFile, certFile, keyFile, crlFile;

            X509Credentials(const std::string & ca, const std::string & cert, const std::string & key, const std::string & crl, int debug = 0)
                : BaseContext(debug), cred(nullptr), caFile(ca), certFile(cert), keyFile(key), crlFile(crl) {}
            ~X509Credentials();

            bool                initSession(const std::string & priority, int mode = GNUTLS_SERVER) override;
        };
    } // TLS

} // LTSM

#endif // _LTSM_SOCKETS_
