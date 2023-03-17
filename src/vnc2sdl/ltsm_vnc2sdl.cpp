/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 **************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>

#include <list>
#include <chrono>
#include <thread>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "librfb_client.h"
#include "ltsm_vnc2sdl.h"

using namespace std::chrono_literals;

namespace LTSM
{
    const auto sanedef = "sock://127.0.0.1:6566";
    const auto pcscdef = "unix:///var/run/pcscd/pcscd.comm";
    const auto librtdef = "/usr/lib64/librtpkcs11ecp.so";
    const auto printdef = "cmd:///usr/bin/lpr";
    const auto pulsedef = "XDG_RUNTIME_DIR/pulse/native";
    const auto krb5def = "TERMSRV@remotehost.name";

#ifdef LTSM_RUTOKEN
    RutokenWrapper::RutokenWrapper(const std::string & lib, ChannelClient & owner) : sender(& owner)
    {
        rutoken::pkicore::initialize(std::filesystem::path(lib).parent_path());
        shutdown = false;

        thscan = std::thread([this]
        {
            std::list<std::string> attached;

            while(! this->shutdown)
            {
                std::list<std::string> devices;

                try
                {
                    for(auto & dev : rutoken::pkicore::Pkcs11Device::enumerate())
                        devices.emplace_back(dev.getSerialNumber());

                    for(auto & serial : devices)
                        if(std::none_of(attached.begin(), attached.end(), [&](auto & str){ return str == serial; }))
                            this->attachedDevice(serial);

                    for(auto & serial : attached)
                        if(std::none_of(devices.begin(), devices.end(), [&](auto & str){ return str == serial; }))
                            this->detachedDevice(serial);

                    attached.swap(devices);
                }
                catch(const rutoken::pkicore::error::InvalidTokenException &)
                {
                }

                std::this_thread::sleep_for(2000ms);
            }

            shutdown = true;
        });
    }

    RutokenWrapper::~RutokenWrapper()
    {
        shutdown = true;

        if(thscan.joinable())
            thscan.join();

        rutoken::pkicore::deinitialize();
    }

    void RutokenWrapper::attachedDevice(const std::string & serial)
    {
        LTSM::Application::info("%s: serial: %s", __FUNCTION__, serial.c_str());

        auto devices = rutoken::pkicore::Pkcs11Device::enumerate();
        auto it = std::find_if(devices.begin(), devices.end(), [=](auto & dev){ return dev.getSerialNumber() == serial; });

        if(it != devices.end())
        {
            auto & dev = *it;
            auto certs = dev.enumerateCerts();

            if(! certs.empty())
            {
                JsonObjectStream jos;
                jos.push("cmd", SystemCommand::TokenAuth);
                jos.push("action", "attach");
                jos.push("serial", serial);
                jos.push("description", dev.getLabel());

                JsonArrayStream jas;
                for(auto & cert : certs)
                    jas.push(cert.toPem());

                jos.push("certs", jas.flush());
                sender->sendLtsmEvent(Channel::System, jos.flush());
            }
        }
    }

    void RutokenWrapper::detachedDevice(const std::string & serial)
    {
        LTSM::Application::info("%s: serial: %s", __FUNCTION__, serial.c_str());

        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::TokenAuth);
        jos.push("action", "detach");
        jos.push("serial", serial);
        sender->sendLtsmEvent(Channel::System, jos.flush());
    }

    std::vector<uint8_t> RutokenWrapper::decryptPkcs7(const std::string & serial, const std::string & pin, int cert, const std::vector<uint8_t> & pkcs7)
    {
        LTSM::Application::info("%s: serial: %s", __FUNCTION__, serial.c_str());

        auto devices = rutoken::pkicore::Pkcs11Device::enumerate();
        auto itd = std::find_if(devices.begin(), devices.end(), [=](auto & dev){ return dev.getSerialNumber() == serial; });

        if(itd == devices.end())
        {
            LTSM::Application::error("%s: device not found, serial: %s", __FUNCTION__, serial.c_str());
            throw token_error("device not found");
        }

        auto & dev = *itd;

        if(! dev.isLoggedIn())
            dev.login(pin);

        if(! dev.isLoggedIn())
        {
            LTSM::Application::error("%s: incorrect pin code, serial: %s, code: %s", __FUNCTION__, serial.c_str(), pin.c_str());
            throw token_error("incorrect pin code");
        }

        auto certs = dev.enumerateCerts();
        auto itc = std::find_if(certs.begin(), certs.end(), [=](auto & crt){ return Tools::crc32b(crt.toPem()) == cert; });

        if(itc == certs.end())
        {
            LTSM::Application::error("%s: certificate not found, serial: %s", __FUNCTION__, serial.c_str());
            throw token_error("certificate not found");
        }

        auto & pkcs11Cert = *itc;

        auto envelopedData = rutoken::pkicore::cms::EnvelopedData::parse(pkcs7);
        auto params = rutoken::pkicore::cms::EnvelopedData::DecryptParams(pkcs11Cert);
        auto message = envelopedData.decrypt(params);

        return rutoken::pkicore::cms::Data::cast(std::move(message)).data();
    }
#endif

    void printHelp(const char* prog)
    {
        std::cout << "usage: " << prog << ": --host <localhost> [--port 5900] [--password <pass>] [--version] [--debug] [--syslog] " <<
            "[--noaccel] [--fullscreen] [--geometry <WIDTHxHEIGHT>]" <<
            "[--notls] " << 
#ifdef LTSM_WITH_GSSAPI
	    "[--kerberos <" << krb5def << ">] " << 
#endif
	    "[--tls-priority <string>] [--tls-ca-file <path>] [--tls-cert-file <path>] [--tls-key-file <path>] " <<
            "[--share-folder <folder>] [--pulse [" << pulsedef << "]] [--printer [" << printdef << "]] [--sane [" << sanedef << "]] " <<
            "[--pcscd [" << pcscdef << "]] [--token-lib [" << librtdef << "]" <<
            "[--noxkb] [--nocaps] [--loop] [--seamless <path>] " << std::endl;

        std::cout << std::endl << "arguments:" << std::endl <<
            "    --version (show program version)" << std::endl <<
            "    --debug (debug mode)" << std::endl <<
            "    --syslog (to syslog)" << std::endl <<
            "    --host <localhost> " << std::endl <<
            "    --port <port> " << std::endl <<
            "    --username <user> " << std::endl <<
            "    --password <pass> " << std::endl <<
            "    --noaccel (disable SDL2 acceleration)" << std::endl <<
            "    --fullscreen (switch to fullscreen mode, Ctrl+F10 toggle)" << std::endl <<
            "    --geometry <WIDTHxHEIGHT> (set window geometry)" << std::endl <<
            "    --notls (disable tls1.2, the server may reject the connection)" << std::endl <<
#ifdef LTSM_WITH_GSSAPI
            "    --kerberos <" << krb5def << "> (kerberos auth, may be use --username for token name)" << std::endl <<
#endif
            "    --tls-priority <string> " << std::endl <<
            "    --tls-ca-file <path> " << std::endl <<
            "    --tls-cert-file <path> " << std::endl <<
            "    --tls-key-file <path> " << std::endl <<
            "    --share-folder <folder> (redirect folder)" << std::endl <<
            "    --seamless <path> (seamless remote program)" << std::endl <<
            "    --noxkb (disable send xkb)" << std::endl <<
            "    --nocaps (disable send capslock)" << std::endl <<
            "    --loop (always reconnecting)" << std::endl <<
            "    --pulse [" << pulsedef << "] (redirect pulseaudio)" << std::endl <<
            "    --printer [" << printdef << "] (redirect printer)" << std::endl <<
            "    --sane [" << sanedef << "] (redirect scanner)" << std::endl <<
            "    --pcscd [" << pcscdef << "] (redirect pkcs11)" << std::endl <<
            "    --token-lib [" << librtdef << "] (token autenfication with LDAP)" << std::endl <<
            std::endl;
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_vnc2sdl")
    {
        Application::setDebug(DebugTarget::Console, DebugLevel::Info);
        username = Tools::getLocalUsername();

        rfbsec.authVenCrypt = true;
        rfbsec.tlsDebug = 2;

        if(2 > argc)
        {
            printHelp(argv[0]);
            throw 0;
        }

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                printHelp(argv[0]);
                throw 0;
            }
        }

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--version"))
                std::cout << "version: " << LTSM_VNC2SDL_VERSION << std::endl;
            else
            if(0 == std::strcmp(argv[it], "--nocaps"))
                capslock = false;
            else
            if(0 == std::strcmp(argv[it], "--noaccel"))
                accelerated = false;
            else
            if(0 == std::strcmp(argv[it], "--notls"))
                rfbsec.authVenCrypt = false;
            else
            if(0 == std::strcmp(argv[it], "--noxkb"))
                usexkb = false;
            else
            if(0 == std::strcmp(argv[it], "--loop"))
                alwaysRunning = true;
            else
            if(0 == std::strcmp(argv[it], "--fullscreen"))
                fullscreen = true;
            else
#ifdef LTSM_WITH_GSSAPI
            if(0 == std::strcmp(argv[it], "--kerberos"))
            {
                rfbsec.authKrb5 = true;
                rfbsec.krb5Service = "TERMSRV";

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    rfbsec.krb5Service = argv[it + 1];
                    it = it + 1;
                }
	    }
            else
#endif
            if(0 == std::strcmp(argv[it], "--printer"))
            {
                printerUrl.assign(printdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "printer", argv[it + 1]);
                    else
                        printerUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--sane"))
            {
                saneUrl.assign(sanedef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "sane", argv[it + 1]);
                    else
                        saneUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--pulse"))
            {
                if(auto runtime = getenv("XDG_RUNTIME_DIR"))
                {
                    auto path = std::filesystem::path(runtime) / "pulse" / "native";
                    pulseUrl = std::string("unix://").append(path.native());
                }

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "pulse", argv[it + 1]);
                    else
                        pulseUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--pcscd"))
            {
                pcscdUrl.assign(pcscdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                        Application::error("%s: parse %s failed, unknown url: %s", __FUNCTION__, "pcscd", argv[it + 1]);
                    else
                        pcscdUrl.assign(argv[it + 1]);

                    it = it + 1;
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--token-lib"))
            {
                tokenLib.assign(librtdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    tokenLib.assign(argv[it + 1]);
                    it = it + 1;
                }

                if(! std::filesystem::exists(tokenLib))
                {
                    Application::error("%s: parse %s failed, not exist: %s", __FUNCTION__, "token-lib", tokenLib.c_str());
                    tokenLib.clear();
                }
            }
            else
            if(0 == std::strcmp(argv[it], "--debug"))
                Application::setDebugLevel(DebugLevel::Debug);
            else
            if(0 == std::strcmp(argv[it], "--syslog"))
                Application::setDebugTarget(DebugTarget::Syslog);
            else
            if(0 == std::strcmp(argv[it], "--host") && it + 1 < argc)
            {
                host.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--seamless") && it + 1 < argc)
            {
                seamless.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--share-folder") && it + 1 < argc)
            {
                share.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--tls-priority") && it + 1 < argc)
            {
                rfbsec.tlsPriority.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                rfbsec.passwdFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--username") && it + 1 < argc)
            {
                username.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
            {
                try
                {
                    port = std::stoi(argv[it + 1]);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "incorrect port number" << std::endl;
                    port = 5900;
                }
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--geometry") && it + 1 < argc)
            {
                size_t idx;

                try
                {
                    setWidth = std::stoi(argv[it + 1], & idx, 0);
                    setHeight = std::stoi(argv[it + 1] + idx + 1, nullptr, 0);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid geometry" << std::endl;
                    setWidth = setHeight = 0;
                }

                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--tls-ca-file") && it + 1 < argc)
            {
                rfbsec.caFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--tls-cert-file") && it + 1 < argc)
            {
                rfbsec.certFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            if(0 == std::strcmp(argv[it], "--tls-key-file") && it + 1 < argc)
            {
                rfbsec.keyFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else
            {
                throw std::invalid_argument(argv[it]);
            }
        }

        if(tokenLib.size() && rfbsec.passwdFile.size() && username.size())
            tokenLib.clear();

        if(fullscreen)
        {
            SDL_DisplayMode mode;
            if(0 == SDL_GetDisplayMode(0, 0, & mode))
            {
                setWidth = mode.w;
                setHeight = mode.h;

                if(setWidth < setHeight)
                    std::swap(setWidth, setHeight);
            }
        }
    }

    bool Vnc2SDL::isAlwaysRunning(void) const
    {
        return alwaysRunning;
    }

    int Vnc2SDL::start(void)
    {
        auto ipaddr = TCPSocket::resolvHostname(host);
        int sockfd = TCPSocket::connect(ipaddr, port);

        if(0 > sockfd)
            return -1;

        RFB::ClientDecoder::setSocketStreamMode(sockfd);

        rfbsec.authVnc = ! rfbsec.passwdFile.empty();
        rfbsec.tlsAnonMode = rfbsec.keyFile.empty();

        if(rfbsec.authKrb5 && rfbsec.krb5Service.empty())
	{
            Application::warning("%s: kerberos remote service empty", __FUNCTION__);
	    rfbsec.authKrb5 = false;
	}

        if(rfbsec.authKrb5 && rfbsec.krb5Name.empty())
	{
	    if(username.empty())
		rfbsec.krb5Name.assign(std::getenv("USERNAME"));
	    else
		rfbsec.krb5Name.assign(username);
	}

        if(rfbsec.authKrb5)
	{
	    // fixed service format: SERVICE@hostname
    	    if(std::string::npos == rfbsec.krb5Service.find("@"))
		rfbsec.krb5Service.append("@").append(host);

            Application::info("%s: kerberos remote service: %s", __FUNCTION__, rfbsec.krb5Service.c_str());
            Application::info("%s: kerberos local name: %s", __FUNCTION__, rfbsec.krb5Name.c_str());
	}

        // connected
        if(! rfbHandshake(rfbsec))
            return -1;

        // rfb thread: process rfb messages
        auto thrfb = std::thread([=]()
        {
            this->rfbMessagesLoop();
        });

        // xcb thread: wait xkb event
        auto thxcb = std::thread([this]()
        {
            while(this->rfbMessagesRunning())
            {
                if(! xcbEventProcessing())
                    break;

                std::this_thread::sleep_for(200ms);
            }
        });

        if(! tokenLib.empty() && std::filesystem::exists(tokenLib))
        {
#ifdef LTSM_RUTOKEN
            if(std::filesystem::path(tokenLib).filename() == std::filesystem::path(librtdef).filename())
            {
                Application::info("%s: check %s success", __FUNCTION__, "rutoken", tokenLib.c_str());
                token = std::make_unique<RutokenWrapper>(tokenLib, static_cast<ChannelClient &>(*this));
            }
#endif
        }

        // main thread: sdl processing
        SDL_Event ev;

        auto clipboardDelay = std::chrono::steady_clock::now();

        if(isContinueUpdatesSupport())
            sendContinuousUpdates(true, { XCB::Point(0,0), clientSize() });

        while(true)
        {
            if(! rfbMessagesRunning())
                break;

            if(xcbError())
                break;

            if(! dropFiles.empty() &&
                std::chrono::steady_clock::now() - dropStart > 700ms)
            {
                ChannelClient::sendSystemTransferFiles(dropFiles);
                dropFiles.clear();
            }

            // send clipboard
            if(std::chrono::steady_clock::now() - clipboardDelay > 300ms &&
                ! focusLost && SDL_HasClipboardText())
            {
                std::thread([this]() mutable
                {
                    if(auto ptr = SDL_GetClipboardText())
                    {
                        const std::scoped_lock guard{ this->clipboardLock };
                        auto clipboardBufSdl = RawPtr<uint8_t>(reinterpret_cast<uint8_t*>(ptr), SDL_strlen(ptr));

                        if(clipboardBufRemote != clipboardBufSdl && clipboardBufLocal != clipboardBufSdl)
                        {
                            clipboardBufLocal = clipboardBufSdl;
                            this->sendCutTextEvent(ptr, SDL_strlen(ptr));
                        }

                        SDL_free(ptr);
                    }
                }).detach();

                clipboardDelay = std::chrono::steady_clock::now();
            }

            if(! SDL_PollEvent(& ev))
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if(! sdlEventProcessing(& ev))
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }
        }

        rfbMessagesShutdown();

        if(thrfb.joinable())
            thrfb.join();

        if(thxcb.joinable())
            thxcb.join();

        return 0;
    }

    void Vnc2SDL::sendMouseState(void)
    {
        int posx, posy;
        uint8_t buttons = SDL_GetMouseState(& posx, & posy);
        //auto [coordX, coordY] = SDL::Window::scaleCoord(ev.button()->x, ev.button()->y);
        sendPointerEvent(buttons, posx, posy);
    }

    void Vnc2SDL::exitEvent(void)
    {
        RFB::ClientDecoder::rfbMessagesShutdown();
    }

    bool Vnc2SDL::sdlEventProcessing(const SDL::GenericEvent & ev)
    {
        const std::scoped_lock guard{ renderLock };

        switch(ev.type())
        {
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                sendMouseState();
                break;

            case SDL_MOUSEWHEEL:
                // scroll up
                if(ev.wheel()->y > 0)
                {
                    // press
                    int posx, posy;
                    uint8_t buttons = SDL_GetMouseState(& posx, & posy);
                    sendPointerEvent(0x08 | buttons, posx, posy);
                    // release
                    sendMouseState();
                }
                else
                if(ev.wheel()->y < 0)
                {
                    // press
                    int posx, posy;
                    uint8_t buttons = SDL_GetMouseState(& posx, & posy);
                    sendPointerEvent(0x10 | buttons, posx, posy);
                    // release
                    sendMouseState();
                }
                break;

            case SDL_WINDOWEVENT:
                if(SDL_WINDOWEVENT_EXPOSED == ev.window()->event)
                    window->renderPresent();
                if(SDL_WINDOWEVENT_FOCUS_GAINED == ev.window()->event)
                    focusLost = false;
                else
                if(SDL_WINDOWEVENT_FOCUS_LOST == ev.window()->event)
                    focusLost = true;
                break;

            case SDL_KEYDOWN:
                // ctrl + F10 -> fast close
                if(ev.key()->keysym.sym == SDLK_F10 &&
                    (KMOD_CTRL & SDL_GetModState()))
                {
                    exitEvent();
                    return true;
                }
                // ctrl + F11 -> fullscreen toggle
                if(ev.key()->keysym.sym == SDLK_F11 &&
                    (KMOD_CTRL & SDL_GetModState()))
                {
		    if(fullscreen)
		    {
			SDL_SetWindowFullscreen(window->get(), 0);
			fullscreen = false;
		    }
		    else
		    {
			SDL_SetWindowFullscreen(window->get(), SDL_WINDOW_FULLSCREEN);
			fullscreen = true;
                    }
		    return true;
		}
                // key press delay 200 ms
                if(std::chrono::steady_clock::now() - keyPress < 200ms)
                {
                    keyPress = std::chrono::steady_clock::now();
                    break;
                }
                // continue
            case SDL_KEYUP:
                if(ev.key()->keysym.sym == 0x40000000 && ! capslock)
                {
                    auto mod = SDL_GetModState();
                    SDL_SetModState(static_cast<SDL_Keymod>(mod & ~KMOD_CAPS));
                    Application::notice("%s: CAPS reset", __FUNCTION__);
                    return true;
                }
                else
                {
                    int xksym = SDL::Window::convertScanCodeToKeySym(ev.key()->keysym.scancode);
                    if(xksym == 0) xksym = ev.key()->keysym.sym;

                    if(usexkb)
                    {
                        auto keycodeGroup = keysymToKeycodeGroup(xksym);
                        int group = xkbGroup();

                        if(group != keycodeGroup.second)
                            xksym = keycodeGroupToKeysym(keycodeGroup.first, group);
                    }

                    sendKeyEvent(ev.type() == SDL_KEYDOWN, xksym);
                }
                break;

            case SDL_DROPFILE:
                {
                    std::unique_ptr<char, void(*)(void*)> drop{ ev.drop()->file, SDL_free };
                    dropFiles.emplace_back(drop.get());
                    dropStart = std::chrono::steady_clock::now();
                }
                break;

            case SDL_USEREVENT:
                {
                    // resize event
                    if(ev.user()->code == 777 || ev.user()->code == 775)
                    {
                        XCB::Size sz;
                        sz.width = (size_t) ev.user()->data1;
                        sz.height = (size_t) ev.user()->data2;
                        bool contUpdateResume = ev.user()->code == 777;

                        fb.reset(new FrameBuffer(sz, format));

                        if(fullscreen)
                            window.reset(new SDL::Window("VNC2SDL", sz.width, sz.height,
                                    0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP, accelerated));
                        else
                            window->resize(sz.width, sz.height);

                        if(contUpdateResume)
                            sendContinuousUpdates(true, {0, 0, sz.width, sz.height});
                        else
                            sendFrameBufferUpdate(false);
                    }
                }
                break;

            case SDL_QUIT:
                exitEvent();
                return true;
        }

        return false;
    }

    void Vnc2SDL::decodingExtDesktopSizeEvent(uint16_t status, uint16_t err, const XCB::Size & sz, const std::vector<RFB::ScreenInfo> & screens)
    {
        // 1. server info: status: 0x00, error: 0x00
        if(status == 0 && err == 0)
        {
            bool resize = false;

            auto [ winWidth, winHeight ] = window->geometry();
            
            if(setWidth && setHeight)
            {
                // 2. send client size
                sendSetDesktopSize(setWidth, setHeight);
            }
            else
            if(winWidth != sz.width || winHeight != sz.height)
            {
                // 2. send client size
                sendSetDesktopSize(sz.width, sz.height);
            } 
        }
        else
        // 3. server reply
        if(status == 1)
        {
            if(0 == err)
            {
                if(setWidth && setHeight &&
                    setWidth == sz.width && setHeight == sz.height)
                    setWidth = setHeight = 0;

                bool contUpdateResume = false;

                if(isContinueUpdatesProcessed() && fb)
                {
                    sendContinuousUpdates(false, XCB::Region(0, 0, fb->width(), fb->height()));
                    contUpdateResume = true;
                }

                // create event for resize (resized in main thread)
                SDL_Event event;
                event.type = SDL_USEREVENT;
                event.user.code = contUpdateResume ? 777 : 775;
                event.user.data1 = (void*)(ptrdiff_t) sz.width;
                event.user.data2 = (void*)(ptrdiff_t) sz.height;
    
                if(0 > SDL_PushEvent(& event))
                    Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_PushEvent", SDL_GetError());
            }
            else
            {
                Application::info("%s: status: %d, error code: %d", __FUNCTION__, status, err);
            }
        }
    }

    void Vnc2SDL::fbUpdateEvent(void)
    {
        const std::scoped_lock guard{ renderLock };

        SDL_Rect area{ .x = dirty.x, .y = dirty.y, .w = dirty.width, .h = dirty.height };
        if(0 != SDL_UpdateTexture(window->display(), & area, fb->pitchData(dirty.y) + dirty.x * fb->bytePerPixel(), fb->pitchSize()))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_UpdateTexture", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        window->renderPresent();
        dirty.reset();
    }

    void Vnc2SDL::pixelFormatEvent(const PixelFormat & pf, uint16_t width, uint16_t height) 
    {
        Application::info("%s: width: %d, height: %d", __FUNCTION__, width, height);

        const std::scoped_lock guard{ renderLock };

        if(! window)
            window.reset(new SDL::Window("VNC2SDL", width, height, 0, 0, 0, accelerated));

        if(setWidth && setHeight)
        {
            auto [ winWidth, winHeight ] = window->geometry();
            if(setWidth == winWidth && setHeight == winHeight)
                setWidth = setHeight = 0;
        }

        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(SDL_TRUE != SDL_PixelFormatEnumToMasks(window->pixelFormat(), &bpp, &rmask, &gmask, &bmask, &amask))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_PixelFormatEnumToMasks", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        bool contUpdateResume = false;

        if(isContinueUpdatesProcessed() && fb)
        {
            sendContinuousUpdates(false, XCB::Region(0, 0, fb->width(), fb->height()));
            contUpdateResume = true;
        }

        format = PixelFormat(bpp, rmask, gmask, bmask, amask);
        fb.reset(new FrameBuffer(XCB::Size(width, height), format));

        if(contUpdateResume)
            sendContinuousUpdates(true, {0, 0, width, height});
    }

    void Vnc2SDL::setPixel(const XCB::Point & dst, uint32_t pixel)
    {
        const std::scoped_lock guard{ renderLock };

        fb->setPixel(dst, pixel);
        dirty.join(dst.x, dst.y, 1, 1);
    }

    void Vnc2SDL::fillPixel(const XCB::Region & dst, uint32_t pixel)
    {
        const std::scoped_lock guard{ renderLock };

        fb->fillPixel(dst, pixel);
        dirty.join(dst);
    }

    const PixelFormat & Vnc2SDL::clientPixelFormat(void) const
    {
        return format;
    }

    XCB::Size Vnc2SDL::clientSize(void) const
    {
        auto [ width, height ] = window->geometry();
        return XCB::Size(width, height);
    }

    void Vnc2SDL::cutTextEvent(std::vector<uint8_t> && buf)
    {
        bool zeroInserted = false;

        if(buf.back() != 0)
        {
            buf.push_back(0);
            zeroInserted = true;
        }

        const std::scoped_lock guard{ clipboardLock };
        clipboardBufRemote.swap(buf);

        if(0 != SDL_SetClipboardText(reinterpret_cast<const char*>(clipboardBufRemote.data())))
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_SetClipboardText", SDL_GetError());

        if(zeroInserted)
            clipboardBufRemote.pop_back();
    }

    void Vnc2SDL::richCursorEvent(const XCB::Region & reg, std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask)
    {
        uint32_t key = Tools::crc32b(pixels.data(), pixels.size());
        auto it = cursors.find(key);
        if(cursors.end() == it)
        {
            auto sdlFormat = SDL_MasksToPixelFormatEnum(format.bitsPerPixel, format.rmask(), format.gmask(), format.bmask(), format.amask());
            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            Application::debug("%s: create cursor, crc32b: %d, width: %d, height: %d, sdl format: %s", __FUNCTION__, key, reg.width, reg.height, SDL_GetPixelFormatName(sdlFormat));

            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width, reg.height, format.bitsPerPixel, reg.width * format.bytePerPixel(), sdlFormat);
            if(! sf)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
                return;
            }

            auto pair = cursors.emplace(key, ColorCursor{ .pixels = std::move(pixels), .surface{ sf, SDL_FreeSurface } } );
            it = pair.first;    

            auto curs = SDL_CreateColorCursor(sf, reg.x, reg.y);
            if(! curs)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_CreateColorCursor", SDL_GetError());
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void Vnc2SDL::ltsmHandshakeEvent(int flags)
    {
        if(! sendOptions)
        {
            auto names = xkbNames();
            auto group = xkbGroup();

            sendSystemClientVariables(clientOptions(), clientEnvironments(), names, (0 <= group && group < names.size() ? names[group] : ""));
            sendOptions = true;
        }
    }

    void Vnc2SDL::xkbStateChangeEvent(int group)
    {
        if(usexkb)
            sendSystemKeyboardChange(xkbNames(), group);
    }

    json_plain Vnc2SDL::clientEnvironments(void) const
    {
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
                                        { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" } };
        JsonObjectStream jo;

        for(auto & lc : lcall)
        {
            auto ptr = std::setlocale(lc.first, "");
            jo.push(lc.second, ptr ? ptr : "C");
        }

        // lang
        auto lang = std::getenv("LANG");
        jo.push("LANG", lang ? lang : "C");

        // timezone
        jo.push("TZ", Tools::getTimeZone());

        // seamless
        if(! seamless.empty())
            jo.push("XSESSION", seamless);

        return jo.flush();
    }

    json_plain Vnc2SDL::clientOptions(void) const
    {
        // other
        JsonObjectStream jo;
        jo.push("hostname", "localhost");
        jo.push("ipaddr", "127.0.0.1");
        jo.push("username", username);
        jo.push("platform", SDL_GetPlatform());

        if(! rfbsec.passwdFile.empty())
            jo.push("password", rfbsec.passwdFile);

        if(! rfbsec.certFile.empty())
            jo.push("certificate", Tools::fileToString(rfbsec.certFile));

        if(! printerUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "printer", printerUrl.c_str());
            jo.push("printer", printerUrl);
        }

        if(! saneUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "sane", saneUrl.c_str());
            jo.push("sane", saneUrl);
        }

        if(! pcscdUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "pcscd", pcscdUrl.c_str());
            jo.push("pcscd", pcscdUrl);
        }

        if(! pulseUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "pulse", pulseUrl.c_str());
            jo.push("pulseaudio", pulseUrl);
        }

        if(! share.empty() && std::filesystem::is_directory(share))
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "fuse", share.c_str());
            jo.push("fuse", share);
        }

        return jo.flush();
    }

    void fuseOpenAction(std::string share, std::string fs, int cookie, int flags, ChannelClient* owner)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "open");
        jos.push("path", fs);
        jos.push("cookie", cookie);

        if(share.size())
        {
            auto path = std::filesystem::path(share + fs);
            int fd = open(path.c_str(), flags);

            if(fd < 0)
            {
                jos.push("error", true);
                jos.push("errno", errno);
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open", strerror(errno), errno);
            }
            else
            {
                close(fd);
            }
        }
        else
        {
            jos.push("error", true);
            jos.push("errno", ENOENT);
        }

        owner->sendLtsmEvent(Channel::System, jos.flush());
    }

    void fuseReadDirAction(std::string share, std::string fs, int cookie, ChannelClient* owner)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "readdir");
        jos.push("path", fs);
        jos.push("cookie", cookie);

        if(share.size())
        {
            std::list<std::string> names;
            auto path = std::filesystem::path(share + fs);

            // return list names
            if(std::filesystem::is_directory(path))
            {
                for(auto const & dirEntry : std::filesystem::directory_iterator{path})
                    names.emplace_back(dirEntry.path().filename());
            }
            else
            {
                jos.push("error", true);
                jos.push("errno", ENOENT);
            }

            jos.push("names", JsonArrayStream(names.begin(), names.end()).flush());
        }
        else
        {
            jos.push("error", true);
            jos.push("errno", ENOENT);
        }

        owner->sendLtsmEvent(Channel::System, jos.flush());
    }

    void fuseGetAttrAction(std::string share, std::string fs, int cookie, ChannelClient* owner)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "getattr");
        jos.push("path", fs);
        jos.push("cookie", cookie);

        if(share.size())
        {
            auto path = std::filesystem::path(share + fs);
            if(std::filesystem::exists(path))
            {
                // return stat struct
                struct stat st = {0};

                if(0 == stat(path.c_str(), & st))
                {
                    JsonObjectStream jst;

                    //jst.push("st_dev", static_cast<int>(st.st_dev));
                    //jst.push("st_ino", static_cast<int>(st.st_ino));
                    jst.push("st_mode", static_cast<int>(st.st_mode));
                    jst.push("st_nlink", static_cast<int>(st.st_nlink));
                    //jst.push("st_uid", static_cast<int>(st.st_uid));
                    //jst.push("st_gid", static_cast<int>(st.st_gid));
                    //jst.push("st_rdev", static_cast<int>(st.st_rdev));
                    jst.push("st_size", static_cast<int>(st.st_size));
                    //jst.push("st_blksize", static_cast<int>(st.st_blksize));
                    jst.push("st_blocks", static_cast<int>(st.st_blocks));
                    jst.push("st_atime", static_cast<int>(st.st_atime));
                    jst.push("st_mtime", static_cast<int>(st.st_mtime));
                    jst.push("st_ctime", static_cast<int>(st.st_ctime));

                    jos.push("stat", jst.flush());

                    Application::debug("%s: fs: %s, st_ino: %d, st_mode: %d, st_nlink: %d, st_size: %d, st_blocks: %d, st_atime: %d, st_mtime: %d, st_ctime: %d", __FUNCTION__, fs.c_str(), st.st_ino, st.st_mode, st.st_nlink, st.st_size, st.st_blocks, st.st_atime, st.st_mtime, st.st_ctime);
                }
                else
                {
                    jos.push("error", true);
                    jos.push("errno", errno);
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "stat", strerror(errno), errno);
                }
            }
            else
            {
                jos.push("error", true);
                jos.push("errno", ENOENT);
            }
        }
        else
        {
            jos.push("error", true);
            jos.push("errno", ENOENT);
        }

        owner->sendLtsmEvent(Channel::System, jos.flush());
    }

    void fuseReadAction(std::string share, std::string fs, int cookie, size_t size, int offset, ChannelClient* owner)
    {
        JsonObjectStream jos;
        jos.push("cmd", SystemCommand::FuseProxy);
        jos.push("fuse", "read");
        jos.push("path", fs);
        jos.push("cookie", cookie);

        if(share.size())
        {
            JsonObjectStream jst;
            auto path = std::filesystem::path(share + fs);

            int fd = open(path.c_str(), 0);
            Application::debug("%s: fs: %s, size: %d, offset: %d", __FUNCTION__, fs.c_str(), size, offset);

            if(fd < 0)
            {
                jos.push("error", true);
                jos.push("errno", errno);
                Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open", strerror(errno), errno);
            }
            else
            {
                if(0 > lseek(fd, offset, SEEK_SET))
                {
                    jos.push("error", true);
                    jos.push("errno", errno);
                    Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "lseek", strerror(errno), errno);
                }
                else
                {
                    BinaryBuf buf(size, 0);

                    int ret = read(fd, buf.data(), buf.size());

                    if(0 > ret)
                    {
                        jos.push("error", true);
                        jos.push("errno", errno);
                        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "lseek", strerror(errno), errno);
                    }
                    else
                    // end stream
                    if(ret == 0)
                    {
                        jos.push("eof", true);
                    }
                    else
                    {
                        buf.resize(ret);
                        jos.push("eof", false);
                        jos.push("data", Tools::convertBinary2JsonString(RawPtr(buf.data(), buf.size())));
                    }
                }

                close(fd);
            }
        }
        else
        {
            jos.push("error", true);
            jos.push("errno", EBADF);
        }

        owner->sendLtsmEvent(Channel::System, jos.flush());
    }

    void Vnc2SDL::systemFuseProxy(const JsonObject & jo)
    {
        auto fuse = jo.getString("fuse");
        if(fuse.empty())
        {
            Application::error("%s: command empty", __FUNCTION__);
            throw sdl_error(NS_FuncName);
        }

        auto path = jo.getString("path");

        if(path.empty())
        {
            Application::error("%s: path empty", __FUNCTION__);
            throw sdl_error(NS_FuncName);
        }

        auto cookie = jo.getInteger("cookie", 0);
        Application::info("%s: command: %s, path: `%s', cookie: %d", __FUNCTION__, fuse.c_str(), path.c_str(), cookie);

        if(fuse == "open")
        {
            std::thread(fuseOpenAction, share, path, cookie, jo.getInteger("flags"), static_cast<ChannelClient*>(this)).detach();
        }
        else
        if(fuse == "readdir")
        {
            std::thread(fuseReadDirAction, share, path, cookie, static_cast<ChannelClient*>(this)).detach();
        }
        else
        if(fuse == "getattr")
        {
            std::thread(fuseGetAttrAction, share, path, cookie, static_cast<ChannelClient*>(this)).detach();
        }
        else
        if(fuse == "read")
        {
            size_t size = jo.getInteger("size", 0);
            int offset = jo.getInteger("offset", 0);

            std::thread(fuseReadAction, share, path, cookie, size, offset, static_cast<ChannelClient*>(this)).detach();
        }
        else
        {
            Application::error("%s: unknown command: %s", __FUNCTION__, fuse.c_str());
            throw sdl_error(NS_FuncName);
        }
    }

    void Vnc2SDL::systemTokenAuth(const JsonObject & jo)
    {
        auto action = jo.getString("action");
        auto serial = jo.getString("serial");

        Application::info("%s: action: %s, serial: %s", __FUNCTION__, action.c_str(), serial.c_str());

        if(! token)
        {
            Application::error("%s: token api not loaded", __FUNCTION__);
            return;
        }

        if(action == "check")
        {
            std::thread([this, ptr = token.get(), serial, pin = jo.getString("pin"), cert = jo.getInteger("cert"), content = jo.getString("data")]
            {
                auto data = Tools::convertJsonString2Binary(content);

                try
                {
                    data = ptr->decryptPkcs7(serial, pin, cert, data);
                }
                catch(const std::exception & err)
                {
                    std::string str = err.what();
                    data.assign(str.begin(), str.end());
                }

                JsonObjectStream jos;
                jos.push("cmd", SystemCommand::TokenAuth);
                jos.push("action", "reply");
                jos.push("serial", serial);
                jos.push("cert", cert);
                jos.push("decrypt", std::string(data.begin(), data.end()));

                static_cast<ChannelClient*>(this)->sendLtsmEvent(Channel::System, jos.flush());
            }).detach();
        }
        else
        {
            Application::error("%s: unknown action: %s", __FUNCTION__, action.c_str());
            throw sdl_error(NS_FuncName);
        }
    }

    void Vnc2SDL::systemLoginSuccess(const JsonObject & jo)
    {
        if(jo.getBoolean("action", false))
        {
            if(token)
                token.reset();
        }
        else
        {
            auto error = jo.getString("error");
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "login", error.c_str());
        }
    }
}

int main(int argc, const char** argv)
{
    if(0 > SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "sdl init video failed" << std::endl;
        return -1;
    }

    bool programRestarting = true;
    int res = 0;

    while(programRestarting)
    {
        try
        {
            LTSM::Vnc2SDL app(argc, argv);
            if(! app.isAlwaysRunning())
                programRestarting = false;
            res = app.start();
        }
        catch(const std::invalid_argument & err)
        {
            std::cerr << "unknown params: " << err.what() << std::endl << std::endl;
            LTSM::printHelp(argv[0]);
            return -1;
        }
        catch(int val)
        {
            return val;
        }
        catch(const std::exception & err)
        {
            LTSM::Application::error("%s: exception: %s", __FUNCTION__, err.what());
            LTSM::Application::info("program: %s", "terminate...");
        }
    }

    SDL_Quit();
    return res;
}
