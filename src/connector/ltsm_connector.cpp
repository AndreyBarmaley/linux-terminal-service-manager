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
#include <sys/select.h>

#include <unistd.h>

#include <cstdio>
#include <thread>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_font_psf.h"
#include "ltsm_connector.h"

#include "ltsm_connector_vnc.h"
#include "ltsm_connector_rdp.h"

using namespace std::chrono_literals;

namespace LTSM
{
    /* BaseStream */
    void BaseStream::sendFlush(void)
    {
        std::fflush(fdout);
    }

    bool BaseStream::hasInput(void) const
    {
        fd_set st;
        struct timeval tv = { 0 };
        FD_ZERO(& st);
        FD_SET(STDIN_FILENO, & st);
        return 0 < select(1, & st, NULL, NULL, & tv);
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

    int BaseStream::recvIntBE16(void)
    {
        return (recvInt8() << 8) | recvInt8();
    }

    int BaseStream::recvIntBE32(void)
    {
        return (recvIntBE16() << 16) | recvIntBE16();
    }

    int BaseStream::recvIntLE16(void)
    {
        return  recvInt8() | (recvInt8() << 8);
    }

    int BaseStream::recvIntLE32(void)
    {
        return  recvIntLE16() | (recvIntLE16() << 16);
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

    BaseStream & BaseStream::sendRaw(const uint8_t* buf, size_t length)
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
        return sendRaw(reinterpret_cast<const uint8_t*>(str.c_str()), str.size());
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

    /* Connector::Service */
    Connector::Service::Service(int argc, const char** argv)
        : ApplicationJsonConfig("ltsm_connector", argc, argv), _type("vnc")
    {
        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help"))
            {
                std::cout << "usage: " << argv[0] << " --config <path> --type <RDP|VNC>" << std::endl;
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
            Application::error("dbus create connection failed");
            return EXIT_FAILURE;
        }

        FILE* in  = fdopen(dup(fileno(stdin)), "rb");
        FILE* out = fdopen(dup(fileno(stdout)), "wb");
        // reset buffering
        std::setvbuf(in, nullptr, _IONBF, 0);
        std::clearerr(in);
        char bufout[1492];
        // set buffering, optimal for tcp mtu size
        std::setvbuf(out, bufout, _IOFBF, 1492);
        std::clearerr(out);
        std::unique_ptr<BaseStream> stream;
        Application::busSetDebugLevel(_config.getString("logging:level"));

        // protocol up
        if(_type == "vnc")
            stream.reset(new Connector::VNC(in, out, conn.get(), _config));
        else if(_type == "rdp")
            stream.reset(new Connector::RDP(in, out, conn.get(), _config));

        if(!stream)
            throw std::string("unknown connector type: ").append(_type);

        int res = stream->communication();
        std::fclose(in);
        std::fclose(out);
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
        const std::chrono::milliseconds waitms(5000);
        auto tp = std::chrono::system_clock::now();
        std::string socketFormat = _config->getString("xvfb:socket");
        std::string socketPath = Tools::replace(socketFormat, "%{display}", screen);

        while(! Tools::checkUnixSocket(socketPath))
        {
            std::this_thread::sleep_for(100ms);

            if(waitms < std::chrono::system_clock::now() - tp)
            {
                Application::error("xvfb: %s", "not started");
                return false;
            }
        }

        _xcbDisplay.reset(new XCB::RootDisplay(addr));
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
	    }
    	    _xcbDisableMessages = false;
        }
    }

    void Connector::SignalProxy::onClearRenderPrimitives(const int32_t & display)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: clear render primitives, display: %d", display);
            _renderPrimitives.clear();
        }
    }

    void Connector::SignalProxy::onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill)
    {
        if(0 < _display && display == _display)
        {
            Application::info("dbus signal: add fill rect, display: %d", display);
            _renderPrimitives.emplace_back(new RenderRect(rect, color, fill));
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
            _renderPrimitives.emplace_back(new RenderText(text, { rx, ry, rw, rh }, color));
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
        Application::busSetDebugLevel(level);
    }
}

int main(int argc, const char** argv)
{
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
