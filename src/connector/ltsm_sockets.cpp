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

#include "gnutls/gnutls.h"
#include "gnutls/crypto.h"

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
            return false;

        struct pollfd fds = {0};
        fds.fd = fd;
        fds.events = POLLIN;
        int res = poll(& fds, 1, timeoutMS);

        if(0 > res)
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw std::runtime_error("NetworkStream::hasInput error");
        }

        return 0 < res;
    }

    size_t NetworkStream::hasData(int fd)
    {
        if(0 > fd)
            return 0;

        int count;
        if(0 > ioctl(fd, FIONREAD, & count))
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw std::runtime_error("NetworkStream::hasData error");
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

    NetworkStream & NetworkStream::sendString(std::string_view str)
    {
        sendRaw(str.data(), str.size());
	return *this;
    }

    void NetworkStream::setReadTimeout(size_t ms)
    {
        rcvTimeout = ms;
    }

    std::vector<uint8_t> NetworkStream::recvData(size_t length) const
    {
        std::vector<uint8_t> res(length, 0);
        recvData(res.data(), res.size());
        return res;
    }

    void NetworkStream::recvData(void* ptr, size_t len) const
    {
        if(rcvTimeout)
            recvRaw(ptr, len, rcvTimeout);
        else
            recvRaw(ptr, len);
    }

    void NetworkStream::recvRaw(void* ptr, size_t len, size_t timeout) const
    {
        auto startp = std::chrono::steady_clock::now();
        auto timeoutms = std::chrono::milliseconds(timeout);

        while(len)
        {
            if(size_t real = hasData())
            {
                real = std::min(real, len);
                recvRaw(ptr, real);

                ptr = static_cast<uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            if(timeoutms < std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startp))
            {
                Application::error("%s: timeout error", __FUNCTION__);
                throw std::runtime_error("NetworkStream::recvRaw timeout");
            }
            else
            {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    std::string NetworkStream::recvString(size_t length) const
    {
        std::string res(length, 0);
        recvData(& res[0], length);
        return res;
    }

    /* FileDescriptor */
    void FileDescriptor::read(int fd, void* ptr, ssize_t len)
    {
	while(true)
	{
	    ssize_t real = ::read(fd, ptr, len);

	    if(len == real)
                break;

	    if(0 < real && real < len)
            {
                ptr = static_cast<uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
	    if(0 == real)
                throw std::runtime_error("FileDescriptor::read data end");

            // error
            if(EAGAIN == errno || EINTR == errno)
	    	continue;

            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
    	    throw std::runtime_error("FileDescriptor::read error");
        }
    }

    void FileDescriptor::write(int fd, const void* ptr, ssize_t len)
    {
	while(true)
	{
	    ssize_t real = ::write(fd, ptr, len);

	    if(len == real)
                break;

            if(0 < real && real < len)
	    {
                ptr = static_cast<const uint8_t*>(ptr) + real;
                len -= real;
                continue;
            }

            // eof
	    if(0 == real)
                throw std::runtime_error("FileDescriptor::write data end");

            // error
	    if(EAGAIN == errno || EINTR == errno)
	    	continue;

            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
    	    throw std::runtime_error("FileDescriptor::write error");
        }
    }

    /* SocketStream */
    SocketStream::SocketStream(int fd) : sock(fd)
    {
        buf.reserve(2048);
    }

    SocketStream::~SocketStream()
    {
        if(0 < sock)
            close(sock);
    }

    void SocketStream::setupTLS(gnutls_session_t sess) const
    {
        gnutls_transport_set_int(sess, sock);
    }

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
        FileDescriptor::read(sock, ptr, len);
    }

    void SocketStream::sendRaw(const void* ptr, size_t len)
    {
        if(ptr && len)
        {
            auto it = static_cast<const uint8_t*>(ptr);
            buf.insert(buf.end(), it, it + len);
        }
    }

    void SocketStream::sendFlush(void)
    {
        if(buf.size())
        {
            FileDescriptor::write(sock, buf.data(), buf.size());
            buf.clear();
        }
    }

    uint8_t SocketStream::peekInt8(void) const
    {
        uint8_t res = 0;
        if(1 != recv(sock, & res, 1, MSG_PEEK))
        {
            Application::error("%s: recv error: %s", __FUNCTION__, strerror(errno));
            throw std::runtime_error("SocketStream::peekInt8");
        }

        return res;
    }

    /* InetStream */
    InetStream::InetStream()
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
        inetFdClose();
    }

    void InetStream::inetFdClose(void)
    {
        if(fdin)
        {
            std::fclose(fdin);
            fdin = nullptr;
        }
        if(fdout)
        {
            std::fclose(fdout);
            fdout = nullptr;
        }
    }

    void InetStream::setupTLS(gnutls_session_t sess) const
    {
        gnutls_transport_set_int2(sess, fileno(fdin), fileno(fdout));
    }

    bool InetStream::hasInput(void) const
    {
        return fdin == nullptr || std::feof(fdin) || std::ferror(fdin) ?
                false : NetworkStream::hasInput(fileno(fdin));
    }

    size_t InetStream::hasData(void) const
    {
        return NetworkStream::hasData(fileno(fdin));
    }

    void InetStream::recvRaw(void* ptr, size_t len) const
    {
        while(true)
        {
	    size_t real = std::fread(ptr, 1, len, fdin);
	    if(len == real)
                break;

            if(std::feof(fdin))
                throw std::runtime_error("InetStream::recvRaw end stream");

            if(std::ferror(fdin))
            {
                Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
                throw std::runtime_error("InetStream::recvRaw error");
            }
        
            ptr = static_cast<uint8_t*>(ptr) + real;
            len -= real;
        }
    }

    void InetStream::sendRaw(const void* ptr, size_t len)
    {
        while(true)
        {
            size_t real = std::fwrite(ptr, 1, len, fdout);
            if(len == real)
                break;

            if(std::feof(fdout))
                throw std::runtime_error("InetStream::sendRaw end stream");

            if(std::ferror(fdout))
            {
                Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
                throw std::runtime_error("InetStream::sendRaw error");
            }

            ptr = static_cast<const uint8_t*>(ptr) + real;
            len -= real;
        }
    }

    void InetStream::sendFlush(void)
    {
        std::fflush(fdout);
    }

    bool InetStream::checkError(void) const
    {
        return fdin == nullptr || fdout == nullptr || std::ferror(fdin) || std::ferror(fdout) ||
	    std::feof(fdin) || std::feof(fdout);
    }

    uint8_t InetStream::peekInt8(void) const
    {
        int res = std::fgetc(fdin);

        if(std::feof(fdin))
            throw std::runtime_error("InetStream::peekInt8 end stream");

        if(std::ferror(fdin))
        {
            Application::error("%s: error: %s", __FUNCTION__, strerror(errno));
            throw std::runtime_error("InetStream::peekInt8 error");
        }

        std::ungetc(res, fdin);
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

        if(loopThread.joinable()) loopThread.join();
        std::filesystem::remove(socketPath);
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

        loopThread = std::thread([this]{
            while(this->loopTransmission)
            {
		try
        	{
            	    if(! this->transmitDataIteration())
        		this->loopTransmission = false;
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
        if(fdin == nullptr || std::feof(fdin) || std::ferror(fdin))
            return false;

        size_t dataSz = 0;

    	// inetFd -> bridgeSock
    	if(NetworkStream::hasInput(fileno(fdin)))
    	{
            dataSz = NetworkStream::hasData(fileno(fdin));
            if(dataSz == 0)
            {
                Application::warning("%s: stream ended", __FUNCTION__);
                return false;
            }

            auto buf = recvData(dataSz);
            FileDescriptor::write(bridgeSock, buf.data(), buf.size());

#ifdef LTSM_DEBUG
            if(! checkError())
            {
                std::string str = Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2);
                Application::debug("from remote: [%s]", str.c_str());
            }
#endif
        }

        if(fdout == nullptr || std::feof(fdout) || std::ferror(fdout))
            return false;

    	// bridgeSock -> inetFd
        if(NetworkStream::hasInput(bridgeSock))
        {
            dataSz = NetworkStream::hasData(bridgeSock);
            if(dataSz == 0)
            {
                Application::warning("%s: stream ended", __FUNCTION__);
                return false;
            }

            std::vector<uint8_t> buf(dataSz);
            FileDescriptor::read(bridgeSock, buf.data(), buf.size());

            sendRaw(buf.data(), buf.size());
            sendFlush();

#ifdef LTSM_DEBUG
            if(! checkError())
            {
                std::string str = Tools::buffer2hexstring<uint8_t>(buf.data(), len, 2);
                Application::debug("from local: [%s]", str.c_str());
            }
#endif
        }

        // no action
        if(dataSz == 0)
            std::this_thread::sleep_for(1ms);

        return true;
    }

    int ProxySocket::connectUnixSocket(std::string_view path)
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
        sockaddr.sun_path[sizeof(sockaddr.sun_path) - 1] = 0;
        std::strncpy(sockaddr.sun_path, path.data(), std::min(path.size(), sizeof(sockaddr.sun_path) - 2));

        if(0 != connect(sock, (struct sockaddr*) &sockaddr,  sizeof(struct sockaddr_un)))
            Application::error("%s: connect error: %s, socket path: %s", __FUNCTION__, strerror(errno), path.data());
        else
            Application::debug("%s: fd: %d", __FUNCTION__, sock);

        return sock;
    }

    int ProxySocket::listenUnixSocket(std::string_view path)
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
        sockaddr.sun_path[sizeof(sockaddr.sun_path) - 1] = 0;
        std::strncpy(sockaddr.sun_path, path.data(), std::min(path.size(), sizeof(sockaddr.sun_path) - 2));

        std::filesystem::remove(path);
        if(0 != bind(fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un)))
        {
            Application::error("%s: bind error: %s, socket path: %s", __FUNCTION__, strerror(errno), path.data());
            close(fd);
            return -1;
        }
            
        if(0 != listen(fd, 5))
        {   
            Application::error("%s: listen error: %s", __FUNCTION__, strerror(errno));
            close(fd);
            return -1;
        }

        Application::info("%s: listen unix sock: %s", __FUNCTION__, path.data());
        return fd;
    }

    int acceptClientUnixSocket(int fd)
    {
        int sock = accept(fd, nullptr, nullptr);
        if(0 > sock)
            Application::error("%s: accept error: %s", __FUNCTION__, strerror(errno));
        else
            Application::debug("%s: conected client, fd: %d", __FUNCTION__, sock);

        return sock;
    }

    bool ProxySocket::proxyInitUnixSockets(std::string_view path)
    {   
        int srvfd = ProxySocket::listenUnixSocket(path);
        if(0 > srvfd)
            return false;

        if(! std::filesystem::is_socket(path))
        {
            Application::error("%s: socket failed, path: %s", __FUNCTION__, path.data());
            return false;
        }

        socketPath = path;
        std::future<int> job = std::async(std::launch::async, acceptClientUnixSocket, srvfd);

        bridgeSock = -1;
        // socket fd: client part
        clientSock = connectUnixSocket(socketPath);

        if(0 < clientSock)
        {
            while(job.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready);

            // socket fd: server part
            bridgeSock = job.get();
        }   
        else
	{
            Application::error("%s: failed", __FUNCTION__);
        }

        close(srvfd);

        if(0 < bridgeSock)
	{
            fcntl(bridgeSock, F_SETFL, fcntl(bridgeSock, F_GETFL, 0) | O_NONBLOCK);
            return true;
        }

        return false;
    }
 
    namespace TLS
    {
        void gnutls_log(int level, const char* str)
        {
            Application::info("gnutls debug: %s", str);
        }

        /* TLS::BaseContext */
        BaseContext::BaseContext(int debug)
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
        Stream::Stream(const NetworkStream* bs) : layer(bs)
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

        size_t Stream::hasData(void) const
        {
            return gnutls_record_check_pending(tls->session);
        }

        void Stream::recvRaw(void* ptr, size_t len, size_t timeoutMS) const
        {
            if(0 >= gnutls_record_check_pending(tls->session))
            {
                void* recv_ptr; void* send_ptr;
                gnutls_transport_get_ptr2(tls->session, & recv_ptr, & send_ptr);

                struct pollfd fds = {0};
                fds.fd = (long) recv_ptr;
                fds.events = POLLIN;

                int ret = poll(& fds, 1, timeoutMS);

                if(0 > ret)
                {
                    Application::error("%s: poll error: %s", __FUNCTION__, strerror(errno));
                    throw std::runtime_error("TLS::Stream::recvRaw error");
                }
                else
                if(0 == ret)
                {
                    Application::error("%s: timeout error", __FUNCTION__);
                    throw std::runtime_error("TLS::Stream::recvRaw timeout");
                }
            }

            recvRaw(ptr, len);
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
            while((ret = gnutls_record_recv(tls->session, ptr, len)) < 0)
            {
                if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
                    continue;
                break;
            }

            if(ret != static_cast<ssize_t>(len))
            {
                Application::error("gnutls_record_recv ret: %ld, error: %s", ret, gnutls_strerror(ret));
                if(gnutls_error_is_fatal(ret))
            	    throw std::runtime_error("TLS::Stream::recvRaw");
            }
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

            if(ret != static_cast<ssize_t>(len))
            {
                Application::error("gnutls_record_send ret: %ld, error: %s", ret, gnutls_strerror(ret));
                if(gnutls_error_is_fatal(ret))
            	    throw std::runtime_error("TLS::Stream::sendRaw");
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
            gnutls_record_uncork(tls->session, 0);
            // cached send data
            gnutls_record_cork(tls->session);
        }

        std::vector<uint8_t> encryptDES(const std::vector<uint8_t> & data, std::string_view str)
        {
            gnutls_cipher_hd_t ctx;

            std::vector<uint8_t> res(data);
            std::array<uint8_t, 8> _key = { 0 };
            std::array<uint8_t, 8> _iv = { 0 };

            std::copy_n(str.begin(), std::min(str.size(), _key.size()), _key.begin());
            gnutls_datum_t key = { _key.data(), _key.size() };
            gnutls_datum_t iv = { _iv.data(), _iv.size() };

            // Reverse the order of bits in the byte
            for(auto & val : _key)
                if(val) val = ((val * 0x0202020202ULL & 0x010884422010ULL) % 1023) & 0xfe;

            size_t offset = 0;
            while(offset < res.size())
            {
                if(int ret = gnutls_cipher_init(& ctx, GNUTLS_CIPHER_DES_CBC, & key, & iv))
                {
                    Application::error("gnutls_cipher_init error: %s", gnutls_strerror(ret));
                    throw std::runtime_error("gnutls_cipher_init");
                }

                if(int ret = gnutls_cipher_encrypt(ctx, res.data() + offset, std::min(_key.size(), res.size() - offset)))
                {
                    Application::error("gnutls_cipher_encrypt error: %s", gnutls_strerror(ret));
                    throw std::runtime_error("gnutls_cipher_encrypt2");
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
                Application::error("gnutls_rnd error: %s", gnutls_strerror(ret));
                throw std::runtime_error("gnutls_rnd");
            }
            return res;
        }

    } // TLS

} // LTSM
