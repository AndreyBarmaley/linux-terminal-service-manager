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

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_font_psf.h"
#include "ltsm_connector.h"

#include "ltsm_connector_vnc.h"
#include "ltsm_connector_rdp.h"
#include "ltsm_connector_spice.h"

using namespace std::chrono_literals;

namespace LTSM
{
    void gnutls_log(int level, const char* str)
    {
	Application::info("gnutls debug: %s", str);
    }

    /* TLS */
    TLS::TLS(int debug) : session(nullptr), dhparams(nullptr)
    {
        int ret = gnutls_global_init();
	if(ret < 0)
	    Application::error("gnutls_global_init error: %s", gnutls_strerror(ret));

	gnutls_global_set_log_level(debug);
	gnutls_global_set_log_function(gnutls_log);
    }

    bool TLS::initSession(const std::string & priority, int mode)
    {
        int ret = gnutls_init(& session, mode);
	if(gnutls_error_is_fatal(ret))
	{
	    Application::error("gnutls_init error: %s", gnutls_strerror(ret));
	    return false;
	}

        if(priority.empty())
        {
	    ret = gnutls_set_default_priority(session);
        }
        else
        {
            ret = gnutls_priority_set_direct(session, priority.c_str(), nullptr);
	    if(ret != GNUTLS_E_SUCCESS)
	    {
		const char* compat = "NORMAL:+ANON-ECDH:+ANON-DH";
	        Application::error("gnutls_priority_set_direct error: %s, priority: %s", gnutls_strerror(ret), priority.c_str());
	        Application::info("reuse compat priority: %s", gnutls_strerror(ret), compat);
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

    bool AnonTLS::initSession(const std::string & priority, int mode)
    {
	if(TLS::initSession(priority, mode))
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

    bool x509TLS::initSession(const std::string & priority, int mode)
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

	if(TLS::initSession(priority, mode))
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

    TLS::~TLS()
    {
	if(dhparams) gnutls_dh_params_deinit(dhparams);
        if(session) gnutls_deinit(session);
        gnutls_global_deinit();
    }

    AnonTLS::~AnonTLS()
    {
        if(cred)
	    gnutls_anon_free_server_credentials(cred);
    }

    x509TLS::~x509TLS()
    {
        if(cred)
	    gnutls_certificate_free_credentials(cred);
    }

    int TLS::recvInt8(void)
    {
	uint8_t ch = 0;
	int ret = 0;
	const int length = 1;

	while((ret = gnutls_record_recv(session, & ch, length)) < 0)
	{
    	    if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
		continue;
            break;
        }

	if(ret == length)
	    return ch;

        if(gnutls_error_is_fatal(ret) == 0)
    	    Application::error("gnutls_record_recv ret: %d, error: %s", ret, gnutls_strerror(ret));

	return 0;
    }

    bool TLS::sendInt8(uint8_t val)
    {
	int ret = 0;
	const int length = 1;

	while((ret = gnutls_record_send(session, & val, length)) < 0)
	{
    	    if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
		continue;
            break;
        }

	if(ret == length)
	    return true;

        if(gnutls_error_is_fatal(ret) == 0)
    	    Application::error("gnutls_record_send ret: %d, error: %s", ret, gnutls_strerror(ret));

	return false;
    }

    bool TLS::sendRaw(const void* buf, size_t length)
    {
	int ret = 0;
	size_t maxsz = gnutls_record_get_max_size(session);

	while((ret = gnutls_record_send(session, buf, std::min(length, maxsz))) < 0)
	{
    	    if(ret == GNUTLS_E_AGAIN || ret == GNUTLS_E_INTERRUPTED)
		continue;
            break;
        }

	if(ret == length)
	    return true;

	if(0 < ret && ret < length)
	    return sendRaw(reinterpret_cast<const uint8_t*>(buf) + maxsz, length - maxsz);

        if(gnutls_error_is_fatal(ret) == 0)
    	    Application::error("gnutls_record_send ret: %d, error: %s", ret, gnutls_strerror(ret));

	return false;
    }

    std::string TLS::sessionDescription(void) const
    {
	auto desc = gnutls_session_get_desc(session);
	std::string res(desc);
	gnutls_free(desc);
	return res;
    }


    /* TLS_Stream */
    TLS_Stream::~TLS_Stream()
    {
	if(tls && handshake)
	    gnutls_bye(tls->session, GNUTLS_SHUT_WR);
    }

    bool TLS_Stream::tlsInitAnonHandshake(const std::string & priority, int debug)
    {
	int ret = 0;
	tls.reset(new AnonTLS(debug));

	if(! tls->initSession(priority, GNUTLS_SERVER))
	{
	    tls.reset();
	    return false;
	}

	gnutls_transport_set_int2(tls->session, fileno(fdin), fileno(fdout));

        do {
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

    bool TLS_Stream::tlsInitX509Handshake(const std::string & priority, const std::string & caFile, const std::string & certFile, const std::string & keyFile, const std::string & crlFile, int debug)
    {
	int ret = 0;
	tls.reset(new x509TLS(caFile, certFile, keyFile, crlFile, debug));

	if(! tls->initSession(priority, GNUTLS_SERVER))
	{
	    tls.reset();
	    return false;
	}

	gnutls_transport_set_int2(tls->session, fileno(fdin), fileno(fdout));

        do {
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

    bool TLS_Stream::hasInput(void) const
    {
        // gnutls doc: 6.5.1 Asynchronous operation
        // utilize gnutls_record_check_pending, either before the poll system call
        return (tls ? 0 < gnutls_record_check_pending(tls->session) : false) ||
            BaseStream::hasInput();
    }

    void TLS_Stream::sendFlush(void) const
    {
	if(tls)
	    gnutls_record_uncork(tls->session, 0); 
	else
	    BaseStream::sendFlush();
    }

    int TLS_Stream::recvInt8(void)
    {
	return tls ? tls->recvInt8() : BaseStream::recvInt8();
    }

    TLS_Stream & TLS_Stream::sendInt8(uint8_t val)
    {
	if(tls)
	    tls->sendInt8(val);
	else
	    BaseStream::sendInt8(val);
	return *this;
    }

    TLS_Stream & TLS_Stream::sendRaw(const void* buf, size_t length)
    {
	if(tls)
	    tls->sendRaw(buf, length);
	else
	    BaseStream::sendRaw(buf, length);
	return *this;
    }
 
    /* BaseStream */
    BaseStream::BaseStream() : fdin(nullptr), fdout(nullptr), ioerr(false)
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

    BaseStream::~BaseStream()
    {
        std::fclose(fdin);
        std::fclose(fdout);
    }

    void BaseStream::sendFlush(void) const
    {
        std::fflush(fdout);
    }

    bool BaseStream::hasInput(void) const
    {
	struct pollfd fds = {0};
	fds.fd = fileno(fdin);
	fds.events = POLLIN;
	return 0 < poll(& fds, 1, 0);
    }

    BaseStream & BaseStream::sendInt8(uint8_t val)
    {
	if(! ioerr)
        {
    	    if(0 != std::ferror(fdout))
    	    {
    	        Application::error("output stream error: %s", strerror(errno));
    	        ioerr = true;
	        return *this;
    	    }

    	    std::fputc(val, fdout);
        }
        return *this;
    }

    BaseStream & BaseStream::sendInt16(uint16_t val)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return sendIntLE16(val);
#else
        return sendIntBE16(val);
#endif
    }

    BaseStream & BaseStream::sendInt32(uint32_t val)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return sendIntLE32(val);
#else
        return sendIntBE32(val);
#endif
    }

    BaseStream & BaseStream::sendInt64(uint64_t val)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return sendIntLE64(val);
#else
        return sendIntBE64(val);
#endif
    }

    BaseStream & BaseStream::sendIntBE16(uint16_t val)
    {
        sendInt8(0x00FF & (val >> 8));
        sendInt8(0x00FF & val);
        return *this;
    }

    BaseStream & BaseStream::sendIntBE32(uint32_t val)
    {
        sendIntBE16(0x0000FFFF & (val >> 16));
        sendIntBE16(0x0000FFFF & val);
        return *this;
    }

    BaseStream & BaseStream::sendIntBE64(uint64_t val)
    {
        sendIntBE32(0xFFFFFFFF & (val >> 32));
        sendIntBE32(0xFFFFFFFF & val);
        return *this;
    }

    BaseStream & BaseStream::sendIntLE16(uint16_t val)
    {
        sendInt8(0x00FF & val);
        sendInt8(0x00FF & (val >> 8));
        return *this;
    }

    BaseStream & BaseStream::sendIntLE32(uint32_t val)
    {
        sendIntLE16(0x0000FFFF & val);
        sendIntLE16(0x0000FFFF & (val >> 16));
        return *this;
    }

    BaseStream & BaseStream::sendIntLE64(uint64_t val)
    {
        sendIntLE32(0xFFFFFFFF & val);
        sendIntLE32(0xFFFFFFFF & (val >> 32));
        return *this;
    }

    int BaseStream::recvInt8(void)
    {
        int res = 0;

	if(! ioerr)
	{
    	    if(0 != std::ferror(fdin))
    	    {
    	        Application::error("input stream error: %s", strerror(errno));
    	        ioerr = true;
	        return 0;
    	    }

    	    res = std::fgetc(fdin);
        }
        return res;
    }

    int BaseStream::recvInt16(void)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return recvIntLE16();
#else
        return recvIntBE16();
#endif
    }

    int BaseStream::recvInt32(void)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return recvIntLE32();
#else
        return recvIntBE32();
#endif
    }

    int BaseStream::recvInt64(void)
    {
#ifdef __ORDER_LITTLE_ENDIAN__
        return recvIntLE64();
#else
        return recvIntBE64();
#endif
    }

    int BaseStream::recvIntBE16(void)
    {
        return (recvInt8() << 8) | recvInt8();
    }

    int BaseStream::recvIntBE32(void)
    {
        return (recvIntBE16() << 16) | recvIntBE16();
    }

    int64_t BaseStream::recvIntBE64(void)
    {
        return (static_cast<int64_t>(recvIntBE32()) << 32) | recvIntBE32();
    }

    int BaseStream::recvIntLE16(void)
    {
        return  recvInt8() | (recvInt8() << 8);
    }

    int BaseStream::recvIntLE32(void)
    {
        return  recvIntLE16() | (recvIntLE16() << 16);
    }

    int64_t BaseStream::recvIntLE64(void)
    {
        return  recvIntLE32() | (static_cast<int64_t>(recvIntLE32()) << 32);
    }

    void BaseStream::recvSkip(size_t length)
    {
	if(! ioerr)
        {
            if(0 != std::ferror(fdin))
            {
                Application::error("input stream error: %s", strerror(errno));
                ioerr = true;
            }

            while(! ioerr && length--)
                recvInt8();
        }
    }

    BaseStream & BaseStream::sendRaw(const void* buf, size_t length)
    {
	if(! ioerr)
        {
            if(0 != std::ferror(fdout))
            {
                Application::error("output stream error: %s", strerror(errno));
                ioerr = true;
	        return *this;
            }

            size_t sz = std::fwrite(buf, 1, length, fdout);
            if(sz != length)
            {
                Application::error("output stream error: %s", strerror(errno));
                ioerr = true;
            }
        }

        return *this;
    }

    BaseStream & BaseStream::sendString(const std::string & str)
    {
        return sendRaw(str.c_str(), str.size());
    }

    std::string BaseStream::recvString(size_t length)
    {
        std::string res;

	if(! ioerr)
        {
            if(0 != std::ferror(fdin))
            {
                Application::error("input stream error: %s", strerror(errno));
                ioerr = true;
	        return res;
            }

            res.reserve(length);
            while(res.size() < length)
                res.append(1, recvInt8());
        }

        return res;
    }

    /* ZlibOutStream */
    void zlibStream::pushInt8(uint8_t val)
    {
	outbuf.push_back(val);
    }

    void zlibStream::pushRaw(const uint8_t* buf, size_t length)
    {
	outbuf.insert(outbuf.end(), buf, buf + length);
    }

    std::vector<uint8_t> zlibStream::syncFlush(bool finish)
    {
    	next_in = outbuf.data();
    	avail_in = outbuf.size();

        std::vector<uint8_t> zip(deflateBound(this, outbuf.size()));
        next_out = zip.data();
        avail_out = zip.size();

        int prev = total_out;
        int ret = deflate(this, finish ? Z_FINISH : Z_SYNC_FLUSH);
        if(ret < Z_OK)
            Application::error("zlib: deflate error: %d", ret);

        size_t zipsz = total_out - prev;
	zip.resize(zipsz);

	outbuf.clear();
        next_out = nullptr;
        avail_out = 0;

	return zip;
    }

    void ZlibOutStream::zlibDeflateStart(size_t len)
    {
        if(! zlibStreamPtr)
        {
            zlibStreamPtr.reset(new zlibStream());
            int ret = deflateInit2(zlibStreamPtr.get(), Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
            if(ret < Z_OK)
                Application::error("zlib: deflateInit error: %d", ret);
	}

	zlibStreamPtr->outbuf.reserve(len);
	deflateStarted = true;
    }

    std::vector<uint8_t> ZlibOutStream::zlibDeflateStop(void)
    {
	deflateStarted = false;
	return zlibStreamPtr->syncFlush();
    }

    ZlibOutStream & ZlibOutStream::sendInt8(uint8_t val)
    {
	if(zlibStreamPtr && deflateStarted)
	    zlibStreamPtr->pushInt8(val);
	else
	    TLS_Stream::sendInt8(val);

	return *this;
    }

    ZlibOutStream & ZlibOutStream::sendRaw(const void* buf, size_t length)
    {
	if(zlibStreamPtr && deflateStarted)
	    zlibStreamPtr->pushRaw(reinterpret_cast<const uint8_t*>(buf), length);
	else
	    TLS_Stream::sendRaw(buf, length);

	return *this;
    }

    /* Connector::Service */
    Connector::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_connector", argc, argv), _type("vnc")
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help"))
            {
                std::cout << "usage: " << argv[0] << " --config <path> --type <RDP|VNC|SPICE>" << std::endl;
                throw 0;
            }
            else if(0 == std::strcmp(argv[it], "--type") && it + 1 < argc)
            {
                _type = Tools::lower(argv[it + 1]);
                it = it + 1;
            }
        }
    }

    int Connector::Service::start(void)
    {
        auto conn = sdbus::createSystemBusConnection();
        if(! conn)
        {
            Application::error("%s", "dbus create connection failed");
            return EXIT_FAILURE;
        }

        std::unique_ptr<BaseStream> stream;
        Application::setDebugLevel(_config.getString("connector:debug"));
        Application::info("connector version: %d", LTSM::service_version);

        // protocol up
        if(_type == "vnc")
            stream.reset(new Connector::VNC(conn.get(), _config));
        else if(_type == "rdp")
            stream.reset(new Connector::RDP(conn.get(), _config));
        else if(_type == "spice")
            stream.reset(new Connector::SPICE(conn.get(), _config));

	int res = stream ?
		stream->communication() : EXIT_FAILURE;

        return res;
    }

    /* Connector::SignalProxy */
    Connector::SignalProxy::SignalProxy(sdbus::IConnection* conn, const JsonObject & jo, const char* type)
        : ProxyInterfaces(*conn, LTSM::dbus_service_name, LTSM::dbus_object_path), _conn(conn), _config(& jo), _display(0),
          _conntype(type), _xcbDisableMessages(true)
    {
        _remoteaddr = Tools::getenv("REMOTE_ADDR", "local");
        _encodingThreads = _config->getInteger("encoding:threads", 2);

        if(_encodingThreads < 1)
        {
            _encodingThreads = 1;
        }
        else
        if(std::thread::hardware_concurrency() < _encodingThreads)
        {
            _encodingThreads = std::thread::hardware_concurrency();
            Application::error("encoding threads incorrect, fixed to hardware concurrency: %d", _encodingThreads);
        }
    }

    bool Connector::SignalProxy::xcbConnect(int screen)
    {
        std::string xauthFile = busCreateAuthFile(screen);
        Application::debug("uid: %d, euid: %d, gid: %d, egid: %d", getuid(), geteuid(), getgid(), getegid());
        Application::debug("xauthfile request: %s", xauthFile.c_str());
        // Xvfb: wait display starting
        setenv("XAUTHORITY", xauthFile.c_str(), 1);
        const std::string addr = Tools::StringFormat(":%1").arg(screen);

        std::string socketFormat = _config->getString("xvfb:socket");
        std::string socketPath = Tools::replace(socketFormat, "%{display}", screen);
        Tools::FrequencyTime ftp;

        while(! Tools::checkUnixSocket(socketPath))
        {
            if(ftp.finishedMilliSeconds(5000))
            {
                Application::error("xvfb: %s", "not started");
                return false;
            }
            std::this_thread::sleep_for(100ms);
        }

        _xcbDisplay.reset(new XCB::RootDisplayExt(addr));
        Application::info("xcb display info, width: %d, height: %d, depth: %d", _xcbDisplay->width(), _xcbDisplay->height(), _xcbDisplay->depth());

        int color = _config->getInteger("display:solid", 0);
        if(0 != color) _xcbDisplay->fillBackground(color);

        const int & winsz_w = _xcbDisplay->width();
        const int & winsz_h = _xcbDisplay->height();
        const int bpp = _xcbDisplay->bitsPerPixel() >> 3;
        const int pagesz = 4096;
        const size_t shmsz = ((winsz_w * winsz_h * bpp / pagesz) + 1) * pagesz;
        _shmInfo = _xcbDisplay->createSHM(shmsz, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP /* only for 'user:conn', 'group:shm' */);

        if(! _shmInfo.isValid())
        {
            Application::error("xcb shm failed, error code: %d", _shmInfo.error()->error_code);
            return false;
        }

        _damageInfo = _xcbDisplay->createDamageNotify(0, 0, winsz_w, winsz_h, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);

        if(! _damageInfo.isValid())
        {
            Application::error("xcb damage failed, error code: %d", _damageInfo.error()->error_code);
            return false;
        }

        _display = screen;
        return true;
    }

    void Connector::SignalProxy::onLoginSuccess(const int32_t & display, const std::string & userName)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: login success, display: %d, username: %s", display, userName.c_str());
            // disable message loop
            _xcbDisableMessages = true;
            int oldDisplay = _display;
            int newDisplay = busStartUserSession(oldDisplay, userName, _remoteaddr, _conntype);

            if(newDisplay < 0)
                throw std::string("user session request failure");

	    if(newDisplay != oldDisplay)
	    {
        	// wait xcb old operations ended
    		std::this_thread::sleep_for(100ms);

        	if(! xcbConnect(newDisplay))
            	    throw std::string("xcb connect failed");

        	busConnectorSwitched(oldDisplay, newDisplay);
		_display = newDisplay;
	    }
    	    _xcbDisableMessages = false;
        }
    }

    void Connector::SignalProxy::onClearRenderPrimitives(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: clear render primitives, display: %d", display);
            for(auto & ptr : _renderPrimitives)
	    {
        	switch(ptr->type)
        	{
            	    case RenderType::RenderRect:
                	if(auto prim = static_cast<RenderRect*>(ptr.get()))
                	{
			    onAddDamage(std::get<0>(prim->region),std::get<1>(prim->region),std::get<2>(prim->region),std::get<3>(prim->region));
                	}
                	break;

            	    case RenderType::RenderText:
                	if(auto prim = static_cast<RenderText*>(ptr.get()))
                	{
			    onAddDamage(std::get<0>(prim->region),std::get<1>(prim->region),std::get<2>(prim->region),std::get<3>(prim->region));
                	}
                	break;

            	    default:
                	break;
        	}
	    }
            _renderPrimitives.clear();
        }
    }

    void Connector::SignalProxy::onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: add fill rect, display: %d", display);
            auto ptr = new RenderRect(rect, color, fill);
            _renderPrimitives.emplace_back(ptr);
	    onAddDamage(std::get<0>(rect),std::get<1>(rect),std::get<2>(rect),std::get<3>(rect));
        }
    }

    void Connector::SignalProxy::onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: add render text, display: %d", display);
            const int16_t rx = std::get<0>(pos);
            const int16_t ry = std::get<1>(pos);
            const uint16_t rw = _systemfont.width * text.size();
            const uint16_t rh = _systemfont.height;
            auto ptr = new RenderText(text, { rx, ry, rw, rh }, color);
            _renderPrimitives.emplace_back(ptr);
	    onAddDamage(rx,ry,rw,rh);
        }
    }

    void Connector::SignalProxy::onPingConnector(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
	    std::thread([=](){ this->busConnectorAlive(display); }).detach();
	}
    }

    void Connector::SignalProxy::onDebugLevel(const std::string & level)
    {
        Application::info("dbus signal: debug level: %s", level.c_str());
        Application::setDebugLevel(level);
    }
}

int main(int argc, const char** argv)
{
    LTSM::Application::setDebugLevel(LTSM::DebugLevel::SyslogInfo);
    int res = 0;

    try
    {
        LTSM::Connector::Service app(argc, argv);
        res = app.start();
    }
    catch(const sdbus::Error & err)
    {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
        LTSM::Application::info("%s", "terminate...");
    }
    catch(const std::string & err)
    {
        LTSM::Application::error("%s", err.c_str());
        LTSM::Application::info("%s", "terminate...");
    }
    catch(int val)
    {
        res = val;
    }

    return res;
}
