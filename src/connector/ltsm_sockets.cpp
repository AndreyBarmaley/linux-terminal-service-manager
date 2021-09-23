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
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

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

/*
// not used
#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
 #define swapLE16(x) (x)
 #define swapLE32(x) (x)
 #define swapLE64(x) (x)
 #define swapBE16(x) bswap16(x)
 #define swapBE32(x) bswap32(x)
 #define swapBE64(x) bswap64(x)
#else
 #define swapLE16(x) bswap16(x)
 #define swapLE32(x) bswap32(x)
 #define swapLE64(x) bswap64(x)
 #define swapBE16(x) (x)
 #define swapBE32(x) (x)
 #define swapBE64(x) (x)
#endif
*/

namespace LTSM
{
    /* NetworkStream */
    bool NetworkStream::hasInput(int fd)
    {
        struct pollfd fds = {0};
        fds.fd = fd;
        fds.events = POLLIN;
        return 0 < poll(& fds, 1, 0);
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
        recvRaw(& byte, 1);
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
            recvInt8();
    }

    NetworkStream & NetworkStream::sendZero(size_t length)
    {
        while(length--)
            sendInt8(0);

        return *this;
    }

    NetworkStream & NetworkStream::sendData(const std::vector<uint8_t> & v)
    {
        sendRaw(v.data(), v.size());
	return *this;
    }

    NetworkStream & NetworkStream::sendString(const std::string & str)
    {
        sendRaw(str.data(), str.size());
	return *this;
    }

    std::vector<uint8_t> NetworkStream::recvData(size_t length) const
    {
        std::vector<uint8_t> res(length, 0);
        recvRaw(res.data(), res.size());
        return res;
    }

    std::string NetworkStream::recvString(size_t length) const
    {
        std::string res;
        res.reserve(length);

        while(res.size() < length)
            res.append(1, recvInt8());

        return res;
    }

    /* BaseSocket */
    BaseSocket::~BaseSocket()
    {
        if(0 < sock)
            close(sock);
    }

    void BaseSocket::setupTLS(gnutls_session_t sess) const
    {
        gnutls_transport_set_int(sess, sock);
    }

    bool BaseSocket::hasInput(void) const
    {
        return NetworkStream::hasInput(sock);
    }

    void BaseSocket::recvRaw(void* ptr, size_t len) const
    {
	while(true)
	{
	    auto real = read(sock, ptr, len);

	    if(len == real)
		break;
	    else
	    if(0 > real)
	    {
		if(EAGAIN == errno || EINTR == errno)
	    	    continue;

        	Application::error("%s: read error: %s", __FUNCTION__, strerror(errno));
	    }
	    else
        	Application::error("%s: read byte: %d, expected: %d", __FUNCTION__, real, len);

    	    throw SocketFailed(errno);
        }
    }

    void BaseSocket::sendRaw(const void* buf, size_t len)
    {
	while(true)
	{
	    auto real = write(sock, buf, len);
	    if(len == real)
		break;
	    else
	    if(0 > real)
	    {
		if(EAGAIN == errno || EINTR == errno)
	    	    continue;

        	Application::error("%s: write error: %s", __FUNCTION__, strerror(errno));
	    }
	    else
        	Application::error("%s: write byte: %d, expected: %d", __FUNCTION__, real, len);

    	    throw SocketFailed(errno);
        }
    }

    uint8_t BaseSocket::peekInt8(void) const
    {
        uint8_t res = 0;
        if(1 != recv(sock, & res, 1, MSG_PEEK))
        {
            Application::error("%s: recv error: %s", __FUNCTION__, strerror(errno));
            throw SocketFailed(errno);
        }

        return res;
    }

    /* InetStream */
    InetStream::InetStream() : fdin(nullptr), fdout(nullptr)
    {
        int fnin = dup(fileno(stdin));
        int fnout = dup(fileno(stdout));
        fdin  = fdopen(fnin, "rb");
        fdout = fdopen(fnout, "wb");
        // reset buffering
        std::setvbuf(fdin, nullptr, _IONBF, 0);
        std::clearerr(fdin);
        // set buffering, optimal for tcp mtu size
        std::setvbuf(fdout, fdbuf.data(), _IOFBF, fdbuf.size());
        std::clearerr(fdout);
    }

    InetStream::~InetStream()
    {
        std::fclose(fdin);
        std::fclose(fdout);
    }

    void InetStream::setupTLS(gnutls_session_t sess) const
    {
        gnutls_transport_set_int2(sess, fileno(fdin), fileno(fdout));
    }

    bool InetStream::hasInput(void) const
    {
        return std::feof(fdin) || std::ferror(fdin) ? false : NetworkStream::hasInput(fileno(fdin));
    }

    void InetStream::recvRaw(void* ptr, size_t len) const
    {
        if(std::ferror(fdin))
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw SocketFailed(errno);
        }

	auto real = std::fread(ptr, 1, len, fdin);
	if(len != real)
    	{
	    Application::error("%s: read byte: %d, expected: %d", __FUNCTION__, real, len);
            throw SocketFailed(errno);
        }
    }

    void InetStream::sendRaw(const void* buf, size_t len)
    {
        if(std::ferror(fdout))
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw SocketFailed(errno);
        }

        if(std::feof(fdout))
        {
            Application::warning("%s: ended", __FUNCTION__);
            throw SocketFailed(errno);
        }

        auto real = std::fwrite(buf, 1, len, fdout);
        if(len != real)
        {
            Application::error("%s: write byte: %d, expected: %d", __FUNCTION__, real, len);
            throw SocketFailed(errno);
        }
    }

    void InetStream::sendFlush(void)
    {
        std::fflush(fdout);
    }

    bool InetStream::checkError(void) const
    {
        return std::ferror(fdin) || std::ferror(fdout) ||
	    std::feof(fdin) || std::feof(fdout);
    }

    uint8_t InetStream::peekInt8(void) const
    {
        if(std::ferror(fdin))
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw SocketFailed(errno);
        }

        int res = std::fgetc(fdin);
        if(std::feof(fdin))
        {
            Application::warning("%s: ended", __FUNCTION__);
            throw SocketFailed(errno);
        }

        std::ungetc(res, fdin);
        return res;
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
        if(loopThread.joinable()) loopThread.join();
            
        std::filesystem::remove(socketPath);
        if(0 < bridgeSock) close(bridgeSock);
        if(0 < clientSock) close(clientSock);

        bridgeSock = -1;
        clientSock = -1;
    }

    int ProxySocket::proxyClientSocket(void) const
    {
        return clientSock;
    }

    int ProxySocket::proxyBridgeSocket(void) const
    {
        return bridgeSock;
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

        loopThread = std::thread([this]{
            while(this->loopTransmission)
            {
		try
        	{
            	    if(! this->enterEventLoopAsync())
        		this->loopTransmission = false;
        	}
        	catch(const SocketFailed & ex)
        	{
		    if(ex.code)
			Application::error("socket exception: code: %d, error: %s", ex.code, strerror(ex.code));
		    else
			Application::error("socket exception: code: %d", ex.code);
        	    this->loopTransmission = false;
		}

                std::this_thread::sleep_for(1ms);
            }
            Application::notice("%s: client %d, bridge: %d", "proxy stopped", this->clientSock, this->bridgeSock);
        });
    }

    bool ProxySocket::enterEventLoopAsync(void)
    {
        buf.clear();

    	// inetFd -> bridgeSock
    	while(hasInput())
    	{
    	    uint8_t ch = recvInt8();
    	    buf.push_back(ch);
	}

        if(buf.size())
        {
            if(buf.size() != send(bridgeSock, buf.data(), buf.size(), 0))
            {
                Application::error("%s: send error: %s", __FUNCTION__, strerror(errno));
                return false;
            }

#ifdef LTSM_DEBUG
            if(! checkError())
            {
                std::string str = Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2);
                Application::debug("from remote: [%s]", str.c_str());
            }
#endif
        }

        if(checkError())
            return false;

    	// bridgeSock -> inetFd
        while(NetworkStream::hasInput(bridgeSock))
        {
            // tcp frame
            buf.resize(1492);

            auto len = recv(bridgeSock, buf.data(), buf.size(), MSG_DONTWAIT);
            if(len < 0)
            {
                Application::error("%s: recv error: %s", __FUNCTION__, strerror(errno));
                return false;
            }

            if(0 < len)
            {
                sendRaw(buf.data(), len);
                sendFlush();
            
#ifdef LTSM_DEBUG
                if(! checkError())
                {
                    std::string str = Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2);
                    Application::debug("from local: [%s]", str.c_str());
                }
#endif
            }
        }
            
        return ! checkError();
    }       

    int ProxySocket::connectUnixSocket(const char* path)
    {   
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if(0 > sock)
        {
            Application::error("%s: socket error: %s", __FUNCTION__, strerror(errno));
            return -1;
        }

        struct sockaddr_un sockaddr;
        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;
        std::strcpy(sockaddr.sun_path, path);

        if(0 != connect(sock, (struct sockaddr*) &sockaddr,  sizeof(struct sockaddr_un)))
            Application::error("%s: connect error: %s, socket path: %s", __FUNCTION__, strerror(errno), path);
        else
            Application::debug("%s: fd: %d", __FUNCTION__, sock);

        return sock;
    }

    int ProxySocket::listenUnixSocket(const char* path)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if(0 > fd)
        {
            Application::error("%s: socket error: %s", __FUNCTION__, strerror(errno));
            return -1;
        }

        struct sockaddr_un sockaddr;
        memset(& sockaddr, 0, sizeof(struct sockaddr_un));
        sockaddr.sun_family = AF_UNIX;
        std::strcpy(sockaddr.sun_path, path);
        
        std::filesystem::remove(path);
        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un)))
        {
            Application::error("%s: bind error: %s, socket path: %s", __FUNCTION__, strerror(errno), path);
            return -1;
        }
            
        if(0 != listen(fd, 5))
        {   
            Application::error("%s: listen error: %s", __FUNCTION__, strerror(errno));
            return -1;
        }
        Application::info("listen unix sock: %s", path);
        
        int sock = accept(fd, nullptr, nullptr);
        if(0 > sock)
            Application::error("%s: accept error: %s", __FUNCTION__, strerror(errno));
        else
            Application::debug("accept unix sock: %s", path);
        
        close(fd);
        return sock;
    }

    bool ProxySocket::proxyInitUnixSockets(const std::string & path)
    {   
        socketPath = path;
        std::future<int> job = std::async(std::launch::async, ProxySocket::listenUnixSocket, socketPath.c_str());
        
        Application::debug("wait server socket: %s", socketPath.c_str());
        
        // wait create socket 3000 ms, 10 ms pause
        if(! Tools::waitCallable<std::chrono::milliseconds>(3000, 10, [=](){ return ! std::filesystem::is_socket(socketPath.c_str()); }))
            return false;
        
        bridgeSock = -1;
        // socket fd: client part
        clientSock = connectUnixSocket(socketPath.c_str());
        if(0 < clientSock)
        {
            while(job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);
            // socket fd: server part
            bridgeSock = job.get();

            if(0 < bridgeSock)
		fcntl(bridgeSock, F_SETFL, fcntl(bridgeSock, F_GETFL, 0) | O_NONBLOCK);

            return 0 < bridgeSock;
        }   

	Application::error("%s: failed", __FUNCTION__);
        return false;
    }
 
    namespace TLS
    {
        void gnutls_log(int level, const char* str)
        {
            Application::info("gnutls debug: %s", str);
        }

        /* TLS::BaseContext */
        BaseContext::BaseContext(int debug) : session(nullptr), dhparams(nullptr)
        {
            Application::info("gnutls version usage: %s", GNUTLS_VERSION);
            int ret = gnutls_global_init();

            if(ret < 0)
                Application::error("gnutls_global_init error: %s", gnutls_strerror(ret));

            gnutls_global_set_log_level(debug);
            gnutls_global_set_log_function(gnutls_log);
        }

        BaseContext::~BaseContext()
        {
            if(dhparams) gnutls_dh_params_deinit(dhparams);

            if(session) gnutls_deinit(session);

            gnutls_global_deinit();
        }

        bool BaseContext::initSession(const std::string & priority, int mode)
        {
            int ret = gnutls_init(& session, mode);

            if(gnutls_error_is_fatal(ret))
            {
                Application::error("gnutls_init error: %s", gnutls_strerror(ret));
                return false;
            }

            if(priority.empty())
                ret = gnutls_set_default_priority(session);
            else
            {
                ret = gnutls_priority_set_direct(session, priority.c_str(), nullptr);

                if(ret != GNUTLS_E_SUCCESS)
                {
                    const char* compat = "NORMAL:+ANON-ECDH:+ANON-DH";
                    Application::error("gnutls_priority_set_direct error: %s, priority: %s", gnutls_strerror(ret), priority.c_str());
                    Application::info("reuse compat priority: %s", compat);
                    ret = gnutls_priority_set_direct(session, compat, nullptr);
                }
            }

            if(gnutls_error_is_fatal(ret))
            {
                Application::error("gnutls_set_default_priority error: %s", gnutls_strerror(ret));
                return false;
            }

            ret = gnutls_dh_params_init(&dhparams);

            if(gnutls_error_is_fatal(ret))
            {
                Application::error("gnutls_dh_params_init error: %s", gnutls_strerror(ret));
                return false;
            }

            ret = gnutls_dh_params_generate2(dhparams, 1024);

            if(gnutls_error_is_fatal(ret))
            {
                Application::error("gnutls_dh_params_generate2 error: %s", gnutls_strerror(ret));
                return false;
            }

            return true;
        }

        /* TLS::AnonCredentials */
        AnonCredentials::~AnonCredentials()
        {
            if(cred)
                gnutls_anon_free_server_credentials(cred);
        }

        bool AnonCredentials::initSession(const std::string & priority, int mode)
        {
            Application::info("gnutls init session: %s", "AnonTLS");

            if(BaseContext::initSession(priority, mode))
            {
                int ret = gnutls_anon_allocate_server_credentials(& cred);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_anon_allocate_server_credentials error: %s", gnutls_strerror(ret));
                    return false;
                }

                gnutls_anon_set_server_dh_params(cred, dhparams);
                ret = gnutls_credentials_set(session, GNUTLS_CRD_ANON, cred);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_credentials_set error: %s", gnutls_strerror(ret));
                    return false;
                }

                return true;
            }

            return false;
        }

        /* TLS::X509Credentials */
        X509Credentials::~X509Credentials()
        {
            if(cred)
                gnutls_certificate_free_credentials(cred);
        }

        bool X509Credentials::initSession(const std::string & priority, int mode)
        {
            if(caFile.empty() || ! std::filesystem::exists(caFile))
            {
                Application::error("CA file not found: %s", caFile.c_str());
                return false;
            }

            if(certFile.empty() || ! std::filesystem::exists(certFile))
            {
                Application::error("cert file not found: %s", certFile.c_str());
                return false;
            }

            if(keyFile.empty() || ! std::filesystem::exists(keyFile))
            {
                Application::error("key file not found: %s", keyFile.c_str());
                return false;
            }

            Application::info("gnutls init session: %s", "X509");

            if(BaseContext::initSession(priority, mode))
            {
                int ret = gnutls_certificate_allocate_credentials(& cred);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_certificate_allocate_credentials error: %s", gnutls_strerror(ret));
                    return false;
                }

                ret = gnutls_certificate_set_x509_trust_file(cred, caFile.c_str(), GNUTLS_X509_FMT_PEM);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_certificate_set_x509_trust_file error: %s, ca: %s", gnutls_strerror(ret), caFile.c_str());
                    return false;
                }

                if(! crlFile.empty() && std::filesystem::exists(crlFile))
                {
                    ret = gnutls_certificate_set_x509_crl_file(cred, crlFile.c_str(), GNUTLS_X509_FMT_PEM);

                    if(gnutls_error_is_fatal(ret))
                    {
                        Application::error("gnutls_certificate_set_x509_crl_file error: %s, crl: %s", gnutls_strerror(ret), crlFile.c_str());
                        return false;
                    }
                }

                ret = gnutls_certificate_set_x509_key_file(cred, certFile.c_str(), keyFile.c_str(), GNUTLS_X509_FMT_PEM);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_certificate_set_x509_key_file error: %s, cert: %s, key: %s", gnutls_strerror(ret), certFile.c_str(), keyFile.c_str());
                    return false;
                }

                /*
                if(! ocspStatusFile.empty() && std::filesystem::is_regular_file(ocspStatusFile))
                {
                    ret = gnutls_certificate_set_ocsp_status_request_file(cred, ocspStatusFile.c_str(), 0);
                    if(gnutls_error_is_fatal(ret))
                    {
                        Application::error("gnutls_certificate_set_ocsp_status_request_file error: %s, ocsp status file: %s", gnutls_strerror(ret), ocspStatusFile.c_str());
                        return false;
                    }
                }
                */
                gnutls_certificate_set_dh_params(cred, dhparams);
                ret = gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, cred);

                if(gnutls_error_is_fatal(ret))
                {
                    Application::error("gnutls_credentials_set error: %s", gnutls_strerror(ret));
                    return false;
                }

                gnutls_certificate_server_set_request(session, GNUTLS_CERT_IGNORE);
                return true;
            }

            return false;
        }

        /* TLS::Stream */
        Stream::Stream(const NetworkStream* bs) : layer(bs), handshake(false)
        {
            if(! bs)
                throw std::invalid_argument("tls stream failed");
        }

        Stream::~Stream()
        {
            if(tls && handshake)
                gnutls_bye(tls->session, GNUTLS_SHUT_WR);
        }

        bool Stream::initAnonHandshake(const std::string & priority, int debug)
        {
            int ret = 0;
            tls.reset(new AnonCredentials(debug));

            if(! tls->initSession(priority, GNUTLS_SERVER))
            {
                tls.reset();
                return false;
            }

            layer->setupTLS(tls->session);

            do
            {
                ret = gnutls_handshake(tls->session);
            }
            while(ret < 0 && gnutls_error_is_fatal(ret) == 0);

            if(ret < 0)
            {
                Application::error("gnutls_handshake error: %s", gnutls_strerror(ret));
                return false;
            }

            handshake = true;
            return true;
        }

        bool Stream::initX509Handshake(const std::string & priority, const std::string & caFile, const std::string & certFile, const std::string & keyFile, const std::string & crlFile, int debug)
        {
            int ret = 0;
            tls.reset(new X509Credentials(caFile, certFile, keyFile, crlFile, debug));

            if(! tls->initSession(priority, GNUTLS_SERVER))
            {
                tls.reset();
                return false;
            }

            layer->setupTLS(tls->session);

            do
            {
                ret = gnutls_handshake(tls->session);
            }
            while(ret < 0 && gnutls_error_is_fatal(ret) == 0);

            if(ret < 0)
            {
                Application::error("gnutls_handshake error: %s", gnutls_strerror(ret));
                return false;
            }

            handshake = true;
            return true;
        }

	std::string Stream::sessionDescription(void) const
	{
    	    auto desc = gnutls_session_get_desc(tls->session);
    	    std::string res(desc);
    	    gnutls_free(desc);
    	    return res;
	}

        bool Stream::hasInput(void) const
        {
            // gnutls doc: 6.5.1 Asynchronous operation
            // utilize gnutls_record_check_pending, either before the poll system call
            return (tls ? 0 < gnutls_record_check_pending(tls->session) : false) || layer->hasInput();
        }

        void Stream::sendRaw(const void* ptr, size_t len)
        {
            ssize_t ret = 0;
            while((ret = gnutls_record_send(tls->session, ptr, len)) < 0)
            {
                if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
                    continue;
                break;
            }

            if(ret != len)
            {
                Application::error("gnutls_record_send ret: %ld, error: %s", ret, gnutls_strerror(ret));
                if(gnutls_error_is_fatal(ret))
            	    throw SocketFailed(ret);
            }
        }

        void Stream::recvRaw(void* ptr, size_t len) const
        {
            ssize_t ret = 0;
            while((ret = gnutls_record_recv(tls->session, ptr, len)) < 0)
            {
                if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
                    continue;
                break;
            }

            if(ret != len)
            {
                Application::error("gnutls_record_recv ret: %ld, error: %s", ret, gnutls_strerror(ret));
                if(gnutls_error_is_fatal(ret))
            	    throw SocketFailed(ret);
            }
        }

        uint8_t Stream::peekInt8(void) const
        {
            uint8_t res = 0;
#if (GNUTLS_VERSION_NUMBER < 0x030605)
            Application::error("gnutls_record_recv_early_data added for 3.6.5, your version: %d.%d.%d", GNUTLS_VERSION_MAJOR, GNUTLS_VERSION_MINOR, GNUTLS_VERSION_PATCH);
#else
            auto ret = gnutls_record_recv_early_data(tls->session, & res, 1);
            if(ret != 1)
            {
                Application::error("gnutls_record_recv_early_data error: %s", gnutls_strerror(ret));
                throw SocketFailed(ret);
            }
#endif
            return res;
        }

        void Stream::sendFlush(void)
        {
            gnutls_record_cork(tls->session);
            gnutls_record_uncork(tls->session, 0);
        }
    } // TLS

} // LTSM
