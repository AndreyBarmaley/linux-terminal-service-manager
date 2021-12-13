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
#include <memory>
#include <cstdint>

#include "gnutls/gnutls.h"
#include "ltsm_streambuf.h"

namespace LTSM
{
    /// @brief: network stream interface
    class NetworkStream : protected ByteOrderInterface
    {
    protected:
        static bool             hasInput(int);

        uint8_t			getInt8(void) const override { return recvInt8(); }
        void    		putInt8(uint8_t v) override { sendInt8(v); }

    public:
        NetworkStream() {}
        virtual ~NetworkStream() {}

        virtual void            setupTLS(gnutls_session_t) const {}

        NetworkStream &         sendIntBE16(uint16_t v) { putIntBE16(v); return *this; }
        NetworkStream &         sendIntBE32(uint32_t v) { putIntBE32(v); return *this; }
        NetworkStream &         sendIntBE64(uint64_t v) { putIntBE64(v); return *this; }

        NetworkStream &         sendIntLE16(uint16_t v) { putIntLE16(v); return *this; }
        NetworkStream &         sendIntLE32(uint32_t v) { putIntLE32(v); return *this; }
        NetworkStream &         sendIntLE64(uint64_t v) { putIntLE64(v); return *this; }

        NetworkStream &         sendInt8(uint8_t);
        NetworkStream &         sendInt16(uint16_t);
        NetworkStream &         sendInt32(uint32_t);
        NetworkStream &         sendInt64(uint64_t);

        NetworkStream & 	sendZero(size_t);
        NetworkStream & 	sendData(const std::vector<uint8_t> &);

        virtual void            sendFlush(void) {}
        virtual void		sendRaw(const void*, size_t) = 0;

        virtual bool            hasInput(void) const = 0;
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
        virtual void            recvRaw(void*, size_t) const = 0;

        NetworkStream &         sendString(const std::string &);
        std::string	        recvString(size_t) const;
    };

    ///@brief socket exception
    struct SocketFailed
    {
	std::string             err;
	SocketFailed(const std::string & val) : err(val) {}
    };

    /// @brief: base socket
    class BaseSocket : public NetworkStream
    {
    protected:
        int                     sock;

    public:
        BaseSocket(int fd = 0) : sock(fd) {}
        ~BaseSocket();

        void                    setupTLS(gnutls_session_t) const override;

        void                    setSocket(int fd) { sock = fd; }

        bool                    hasInput(void) const override;
        uint8_t	                peekInt8(void) const override;

        void			sendRaw(const void*, size_t) override;
        void	                recvRaw(void*, size_t) const override;
    };

    /// @brief: tcp stream
    class TcpStream : public BaseSocket
    {
    protected:

    public:
        TcpStream(int port){}
    };

    /// @brief: inetd stream
    class InetStream : public NetworkStream
    {
    protected:
        FILE*                   fdin;
        FILE*                   fdout;
        std::array<char, 1492>  fdbuf;

        void                    inetFdClose(void);

    public:
        InetStream();
        ~InetStream();

        void                    setupTLS(gnutls_session_t) const override;

        bool                    hasInput(void) const override;
        uint8_t		        peekInt8(void) const override;

        void                    sendFlush(void) override;
        bool                    checkError(void) const;

        void			sendRaw(const void*, size_t) override;
        void	                recvRaw(void*, size_t) const override;
    };

    /// @brief: proxy socket: stdin/stdout to local socket
    class ProxySocket : private InetStream
    {
        std::atomic<bool>       loopTransmission;
        std::thread             loopThread;
        int                     bridgeSock;
        int                     clientSock;
        std::string             socketPath;
        std::vector<uint8_t>    buf;

    protected:
        bool                    enterEventLoopAsync(void);

    public:
        ProxySocket() : loopTransmission(false), bridgeSock(-1), clientSock(-1) {}
        ~ProxySocket();
            
        int                     proxyClientSocket(void) const;
        int                     proxyBridgeSocket(void) const;
        bool                    proxyInitUnixSockets(const std::string &);
        bool                    proxyRunning(void) const;

        void                    proxyStartEventLoop(void);
        void                    proxyStopEventLoop(void);
        void                    proxyShutdown(void);

        static int              connectUnixSocket(const char* path);
        static int              listenUnixSocket(const char* path);
    };

    /// transport layer security
    namespace TLS
    {
        std::vector<uint8_t>    randomKey(size_t);
        std::vector<uint8_t>    encryptDES(const std::vector<uint8_t> & crypt, const std::string & key);

        /// @brief: tls context
        struct BaseContext
        {
            gnutls_session_t    session;
            gnutls_dh_params_t  dhparams;

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
            bool                handshake;
            std::unique_ptr<BaseContext> tls;

        public:
            Stream(const NetworkStream*);
            ~Stream();

            bool                initAnonHandshake(const std::string & priority, int debug);
            bool                initX509Handshake(const std::string & priority, const std::string & caFile, const std::string & certFile,
                                                        const std::string & keyFile, const std::string & crlFile, int debug);

            bool                hasInput(void) const override;
            void                sendFlush(void) override;
            uint8_t	        peekInt8(void) const override;

            void		sendRaw(const void*, size_t) override;
            void                recvRaw(void*, size_t) const override;

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
