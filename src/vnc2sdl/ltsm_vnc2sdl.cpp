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
    const auto krb5def = "TERMSRV@remotehost.name";

#ifdef LTSM_RUTOKEN
    RutokenWrapper::RutokenWrapper(const std::string & lib,
                                   ChannelClient & owner) : sender(& owner)
    {
        rutoken::pkicore::initialize(std::filesystem::path(lib).parent_path());
        shutdown = false;
        thscan = std::thread([this]
        {
            std::forward_list<std::string> attached;

            while(! this->shutdown)
            {
                std::forward_list<std::string> devices;

                try
                {
                    for(auto & dev : rutoken::pkicore::Pkcs11Device::enumerate())
                    {
                        devices.emplace_front(dev.getSerialNumber());
                    }

                    for(auto & serial : devices)
                        if(std::none_of(attached.begin(), attached.end(), [&](auto & str)
                    {
                        return str == serial;
                    }))
                    this->attachedDevice(serial);

                    for(auto & serial : attached)
                        if(std::none_of(devices.begin(), devices.end(), [&](auto & str)
                    {
                        return str == serial;
                    }))
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
        {
            thscan.join();
        }

        rutoken::pkicore::deinitialize();
    }

    void RutokenWrapper::attachedDevice(const std::string & serial)
    {
        LTSM::Application::info("%s: serial: %s", __FUNCTION__, serial.c_str());
        auto devices = rutoken::pkicore::Pkcs11Device::enumerate();
        auto it = std::find_if(devices.begin(), devices.end(), [ = ](auto & dev)
        {
            return dev.getSerialNumber() == serial;
        });

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
                {
                    jas.push(cert.toPem());
                }

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

    std::vector<uint8_t> RutokenWrapper::decryptPkcs7(const std::string & serial,
            const std::string & pin, int cert, const std::vector<uint8_t> & pkcs7)
    {
        LTSM::Application::info("%s: serial: %s", __FUNCTION__, serial.c_str());
        auto devices = rutoken::pkicore::Pkcs11Device::enumerate();
        auto itd = std::find_if(devices.begin(), devices.end(), [ = ](auto & dev)
        {
            return dev.getSerialNumber() == serial;
        });

        if(itd == devices.end())
        {
            LTSM::Application::error("%s: device not found, serial: %s", __FUNCTION__,
                                     serial.c_str());
            throw token_error("device not found");
        }

        auto & dev = *itd;

        if(! dev.isLoggedIn())
        {
            dev.login(pin);
        }

        if(! dev.isLoggedIn())
        {
            LTSM::Application::error("%s: incorrect pin code, serial: %s, code: %s",
                                     __FUNCTION__, serial.c_str(), pin.c_str());
            throw token_error("incorrect pin code");
        }

        auto certs = dev.enumerateCerts();
        auto itc = std::find_if(certs.begin(), certs.end(), [ = ](auto & crt)
        {
            return Tools::crc32b(crt.toPem()) == cert;
        });

        if(itc == certs.end())
        {
            LTSM::Application::error("%s: certificate not found, serial: %s", __FUNCTION__,
                                     serial.c_str());
            throw token_error("certificate not found");
        }

        auto & pkcs11Cert = *itc;
        auto envelopedData = rutoken::pkicore::cms::EnvelopedData::parse(pkcs7);
        auto params = rutoken::pkicore::cms::EnvelopedData::DecryptParams(pkcs11Cert);
        auto message = envelopedData.decrypt(params);
        return rutoken::pkicore::cms::Data::cast(std::move(message)).data();
    }
#endif

    void printHelp(const char* prog, const std::list<int> & encodings)
    {
        std::cout << std::endl <<
                  prog << " version: " << LTSM_VNC2SDL_VERSION << std::endl;
        std::cout << std::endl <<
                  "usage: " << prog <<
                  ": --host <localhost> [--port 5900] [--password <pass>] [--version] [--debug] [--syslog] "
                  <<
                  "[--noaccel] [--fullscreen] [--geometry <WIDTHxHEIGHT>]" <<
                  "[--notls] " <<
#ifdef LTSM_WITH_GSSAPI
                  "[--kerberos <" << krb5def << ">] " <<
#endif
#ifdef LTSM_DECODING_FFMPEG
                  "[--h264]" <<
                  "[--av1]" <<
                  "[--vp8]" <<
#endif
                  "[--encoding <string>] " <<
                  "[--tls-priority <string>] [--tls-ca-file <path>] [--tls-cert-file <path>] [--tls-key-file <path>] "
                  <<
                  "[--share-folder <folder>] [--printer [" << printdef << "]] [--sane [" << sanedef << "]] " <<
                  "[--pcscd [" << pcscdef << "]] [--token-lib [" << librtdef << "]" <<
                  "[--noxkb] [--nocaps] [--loop] [--seamless <path>] " << std::endl;
        std::cout << std::endl << "arguments:" << std::endl <<
                  "    --debug (debug mode)" << std::endl <<
                  "    --syslog (to syslog)" << std::endl <<
                  "    --host <localhost> " << std::endl <<
                  "    --port <port> " << std::endl <<
                  "    --username <user> " << std::endl <<
                  "    --password <pass> " << std::endl <<
                  "    --noaccel (disable SDL2 acceleration)" << std::endl <<
                  "    --fullscreen (switch to fullscreen mode, Ctrl+F10 toggle)" << std::endl <<
                  "    --geometry <WIDTHxHEIGHT> (set window geometry)" << std::endl <<
                  "    --notls (disable tls1.2, the server may reject the connection)" <<
                  std::endl <<
#ifdef LTSM_WITH_GSSAPI
                  "    --kerberos <" << krb5def <<
                  "> (kerberos auth, may be use --username for token name)" << std::endl <<
#endif
#ifdef LTSM_DECODING_FFMPEG
                  "    --h264 (the same as --encoding ffmpeg_h264)" << std::endl <<
                  "    --av1 (the same as --encoding ffmpeg_av1)" << std::endl <<
                  "    --vp8 (the same as --encoding ffmpeg_vp8)" << std::endl <<
#endif
                  "    --encoding <string> (set preffered encoding)" << std::endl <<
                  "    --tls-priority <string> " << std::endl <<
                  "    --tls-ca-file <path> " << std::endl <<
                  "    --tls-cert-file <path> " << std::endl <<
                  "    --tls-key-file <path> " << std::endl <<
                  "    --share-folder <folder> (redirect folder)" << std::endl <<
                  "    --seamless <path> (seamless remote program)" << std::endl <<
                  "    --noxkb (disable send xkb)" << std::endl <<
                  "    --nocaps (disable send capslock)" << std::endl <<
                  "    --loop (always reconnecting)" << std::endl <<
                  "    --audio [" <<
#ifdef LTSM_WITH_OPUS
                  "opus, pcm" <<
#else
                  "opus" <<
#endif
                  " " << "]" << "(audio support)" << std::endl <<
                  "    --printer [" << printdef << "] (redirect printer)" << std::endl <<
                  "    --sane [" << sanedef << "] (redirect scanner)" << std::endl <<
                  "    --pcscd [" << pcscdef << "] (redirect pkcs11)" << std::endl <<
                  "    --token-lib [" << librtdef << "] (token autenfication with LDAP)" <<
                  std::endl <<
                  std::endl;
        std::cout << std::endl << "supported encodings: " << std::endl <<
                  "    ";

        for(auto enc : encodings)
        {
            std::cout << Tools::lower(RFB::encodingName(enc)) << " ";
        }

        std::cout << std::endl << std::endl;
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_client")
    {
        Application::setDebug(DebugTarget::Console, DebugLevel::Info);
        rfbsec.authVenCrypt = true;
        rfbsec.tlsDebug = 2;

        if(2 > argc)
        {
            printHelp(argv[0], supportedEncodings());
            throw 0;
        }

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h"))
            {
                printHelp(argv[0], supportedEncodings());
                throw 0;
            }
        }

        for(int it = 1; it < argc; ++it)
        {
            if(0 == std::strcmp(argv[it], "--nocaps"))
            {
                capslock = false;
            }
            else if(0 == std::strcmp(argv[it], "--noaccel"))
            {
                accelerated = false;
            }
            else if(0 == std::strcmp(argv[it], "--notls"))
            {
                rfbsec.authVenCrypt = false;
            }
            else if(0 == std::strcmp(argv[it], "--noxkb"))
            {
                usexkb = false;
            }
            else if(0 == std::strcmp(argv[it], "--loop"))
            {
                alwaysRunning = true;
            }
            else if(0 == std::strcmp(argv[it], "--fullscreen"))
            {
                fullscreen = true;
            }

#ifdef LTSM_DECODING_FFMPEG
            else if(0 == std::strcmp(argv[it], "--h264"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_H264)));
            }
            else if(0 == std::strcmp(argv[it], "--av1"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_AV1)));
            }
            else if(0 == std::strcmp(argv[it], "--vp8"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_VP8)));
            }

#endif
            else if(0 == std::strcmp(argv[it], "--encoding"))
            {
                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    prefferedEncoding.assign(Tools::lower(argv[it + 1]));
                    it = it + 1;
                }

                auto encodings = supportedEncodings();

                if(std::none_of(encodings.begin(), encodings.end(),
                        [&](auto & str) { return Tools::lower(RFB::encodingName(str)) == prefferedEncoding; }))
                {
                    Application::warning("%s: incorrect encoding: %s", __FUNCTION__,
                                         prefferedEncoding.c_str());
                    prefferedEncoding.clear();
                }
            }

#ifdef LTSM_WITH_GSSAPI
            else if(0 == std::strcmp(argv[it], "--kerberos"))
            {
                rfbsec.authKrb5 = true;
                rfbsec.krb5Service = "TERMSRV";

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    rfbsec.krb5Service = argv[it + 1];
                    it = it + 1;
                }
            }

#endif
            else if(0 == std::strcmp(argv[it], "--audio"))
            {
                audioEnable = true;

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    audioEncoding = argv[it + 1];
                    it = it + 1;
                }
            }
            else if(0 == std::strcmp(argv[it], "--printer"))
            {
                printerUrl.assign(printdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                    {
                        Application::warning("%s: parse %s failed, unknown url: %s", __FUNCTION__,
                                             "printer", argv[it + 1]);
                    }
                    else
                    {
                        printerUrl.assign(argv[it + 1]);
                    }

                    it = it + 1;
                }
            }
            else if(0 == std::strcmp(argv[it], "--sane"))
            {
                saneUrl.assign(sanedef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                    {
                        Application::warning("%s: parse %s failed, unknown url: %s", __FUNCTION__,
                                             "sane", argv[it + 1]);
                    }
                    else
                    {
                        saneUrl.assign(argv[it + 1]);
                    }

                    it = it + 1;
                }
            }
            else if(0 == std::strcmp(argv[it], "--pcscd"))
            {
                pcscdUrl.assign(pcscdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    auto url = Channel::parseUrl(argv[it + 1]);

                    if(url.first == Channel::ConnectorType::Unknown)
                    {
                        Application::warning("%s: parse %s failed, unknown url: %s", __FUNCTION__,
                                             "pcscd", argv[it + 1]);
                    }
                    else
                    {
                        pcscdUrl.assign(argv[it + 1]);
                    }

                    it = it + 1;
                }
            }
            else if(0 == std::strcmp(argv[it], "--token-lib"))
            {
                tokenLib.assign(librtdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    tokenLib.assign(argv[it + 1]);
                    it = it + 1;
                }

                if(! std::filesystem::exists(tokenLib))
                {
                    Application::warning("%s: parse %s failed, not exist: %s", __FUNCTION__,
                                         "token-lib", tokenLib.c_str());
                    tokenLib.clear();
                }
            }
            else if(0 == std::strcmp(argv[it], "--debug"))
            {
                Application::setDebugLevel(DebugLevel::Debug);
            }
            else if(0 == std::strcmp(argv[it], "--syslog"))
            {
                Application::setDebugTarget(DebugTarget::Syslog);
            }
            else if(0 == std::strcmp(argv[it], "--host") && it + 1 < argc)
            {
                host.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--seamless") && it + 1 < argc)
            {
                seamless.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--share-folder") && it + 1 < argc)
            {
                auto dir = argv[it + 1];
                if(std::filesystem::is_directory(dir))
                    shareFolders.emplace_front(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--tls-priority") && it + 1 < argc)
            {
                rfbsec.tlsPriority.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                rfbsec.passwdFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--username") && it + 1 < argc)
            {
                username.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--port") && it + 1 < argc)
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
            else if(0 == std::strcmp(argv[it], "--geometry") && it + 1 < argc)
            {
                size_t idx;

                try
                {
                    auto width = std::stoi(argv[it + 1], & idx, 0);
                    auto height = std::stoi(argv[it + 1] + idx + 1, nullptr, 0);
                    setGeometry = XCB::Size(width, height);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid geometry" << std::endl;
                }

                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--tls-ca-file") && it + 1 < argc)
            {
                rfbsec.caFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--tls-cert-file") && it + 1 < argc)
            {
                rfbsec.certFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--tls-key-file") && it + 1 < argc)
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
        {
            tokenLib.clear();
        }

        if(fullscreen)
        {
            SDL_DisplayMode mode;

            if(0 == SDL_GetDisplayMode(0, 0, & mode))
            {
                setGeometry = XCB::Size(mode.w, mode.h);

                if(setGeometry.width < setGeometry.height)
                {
                    std::swap(setGeometry.width, setGeometry.height);
                }
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
        {
            return -1;
        }

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
            {
                if(auto env = std::getenv("USER"))
                    rfbsec.krb5Name.assign(env);
                else
                if(auto env = std::getenv("USERNAME"))
                    rfbsec.krb5Name.assign(env);
            }
            else
                rfbsec.krb5Name.assign(username);
        }

        if(rfbsec.authKrb5)
        {
            // fixed service format: SERVICE@hostname
            if(std::string::npos == rfbsec.krb5Service.find("@"))
            {
                rfbsec.krb5Service.append("@").append(host);
            }

            Application::info("%s: kerberos remote service: %s", __FUNCTION__,
                              rfbsec.krb5Service.c_str());
            Application::info("%s: kerberos local name: %s", __FUNCTION__,
                              rfbsec.krb5Name.c_str());
        }

        // connected
        if(! rfbHandshake(rfbsec))
        {
            return -1;
        }

        // rfb thread: process rfb messages
        auto thrfb = std::thread([ = ]()
        {
            this->rfbMessagesLoop();
        });
        // xcb thread: wait xkb event
        auto thxcb = std::thread([this]()
        {
            while(this->rfbMessagesRunning())
            {
                if(! xcbEventProcessing())
                {
                    break;
                }

                std::this_thread::sleep_for(200ms);
            }
        });

        if(! tokenLib.empty() && std::filesystem::exists(tokenLib))
        {
#ifdef LTSM_RUTOKEN

            if(std::filesystem::path(tokenLib).filename() == std::filesystem::path(
                                librtdef).filename())
            {
                Application::info("%s: check %s success", __FUNCTION__, "rutoken",
                                  tokenLib.c_str());
                token = std::make_unique<RutokenWrapper>(tokenLib,
                        static_cast<ChannelClient &>(*this));
            }

#endif
        }

        // main thread: sdl processing
        auto clipboardDelay = std::chrono::steady_clock::now();
        std::thread thclip;

        if(isContinueUpdatesSupport())
        {
            sendContinuousUpdates(true, { XCB::Point(0, 0), windowSize });
        }

        while(true)
        {
            if(! rfbMessagesRunning())
            {
                break;
            }

            if(xcbError())
            {
                break;
            }

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
                if(thclip.joinable())
                {
                    thclip.join();
                }
                else
                {
                    thclip = std::thread([this]() mutable
                    {
                        if(auto ptr = SDL_GetClipboardText())
                        {
                            const std::scoped_lock guard{ this->clipboardLock };
                            auto clipboardBufSdl = RawPtr<uint8_t>(reinterpret_cast<uint8_t*>(ptr),
                                                                   SDL_strlen(ptr));

                            if(clipboardBufRemote != clipboardBufSdl &&
                                clipboardBufLocal != clipboardBufSdl)
                            {
                                clipboardBufLocal = clipboardBufSdl;
                                this->sendCutTextEvent(ptr, SDL_strlen(ptr));
                            }

                            SDL_free(ptr);
                        }
                    });
                }

                clipboardDelay = std::chrono::steady_clock::now();
            }

            if(needUpdate && sfback)
            {
                const std::scoped_lock guard{ renderLock };
                SDL_Texture* tx = SDL_CreateTextureFromSurface(window->render(), sfback.get());

                if(! tx)
                {
                    Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                       "SDL_CreateTextureFromSurface", SDL_GetError());
                    throw sdl_error(NS_FuncName);
                }

                window->renderReset();

                if(0 != SDL_RenderCopy(window->render(), tx, nullptr, nullptr))
                {
                    Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_RenderCopy",
                                       SDL_GetError());
                    throw sdl_error(NS_FuncName);
                }

                SDL_RenderPresent(window->render());
                SDL_DestroyTexture(tx);
                needUpdate = false;
            }

            if(! sdlEventProcessing())
            {
                std::this_thread::sleep_for(5ms);
                continue;
            }
        }

        rfbMessagesShutdown();

        if(thclip.joinable())
        {
            thclip.join();
        }

        if(thrfb.joinable())
        {
            thrfb.join();
        }

        if(thxcb.joinable())
        {
            thxcb.join();
        }

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

    enum LocalEvent { Resize = 776, ResizeCont = 777 };

    bool Vnc2SDL::sdlEventProcessing(void)
    {
        const std::scoped_lock guard{ renderLock };

        if(! SDL_PollEvent(& sdlEvent))
        {
            return false;
        }

        const SDL::GenericEvent ev(& sdlEvent);

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
                else if(ev.wheel()->y < 0)
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
                {
                    window->renderPresent();
                }

                if(SDL_WINDOWEVENT_FOCUS_GAINED == ev.window()->event)
                {
                    focusLost = false;
                }
                else if(SDL_WINDOWEVENT_FOCUS_LOST == ev.window()->event)
                {
                    focusLost = true;
                }

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

                    if(xksym == 0)
                    {
                        xksym = ev.key()->keysym.sym;
                    }

                    if(usexkb)
                    {
                        auto keycodeGroup = keysymToKeycodeGroup(xksym);
                        int group = xkbGroup();

                        if(group != keycodeGroup.second)
                        {
                            xksym = keycodeGroupToKeysym(keycodeGroup.first, group);
                        }
                    }

                    sendKeyEvent(ev.type() == SDL_KEYDOWN, xksym);
                }

                break;

            case SDL_DROPFILE:
            {
                std::unique_ptr<char, void(*)(void*)> drop{ ev.drop()->file, SDL_free };
                dropFiles.emplace_front(drop.get());
                dropStart = std::chrono::steady_clock::now();
            }
            break;

            case SDL_USEREVENT:
            {
                // resize event
                if(ev.user()->code == LocalEvent::Resize ||
                    ev.user()->code == LocalEvent::ResizeCont)
                {
                    auto width = (size_t) ev.user()->data1;
                    auto height = (size_t) ev.user()->data2;
                    bool contUpdateResume = ev.user()->code == LocalEvent::ResizeCont;
                    cursors.clear();

                    if(fullscreen)
                    {
                        window.reset(new SDL::Window("LTSM_client", width, height,
                                                     0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP, accelerated));
                    }
                    else
                    {
                        window->resize(width, height);
                    }

                    auto pair = window->geometry();
                    windowSize = XCB::Size(pair.first, pair.second);
                    displayResizeEvent(windowSize);
                    // full update
                    sendFrameBufferUpdate(false);

                    if(contUpdateResume)
                    {
                        sendContinuousUpdates(true, {0, 0, windowSize.width, windowSize.height});
                    }
                }
            }
            break;

            case SDL_QUIT:
                exitEvent();
                return true;
        }

        return false;
    }

    bool Vnc2SDL::pushEventWindowResize(const XCB::Size & nsz)
    {
        if(windowSize == nsz)
        {
            return true;
        }

        bool contUpdateResume = false;

        if(isContinueUpdatesProcessed())
        {
            sendContinuousUpdates(false, XCB::Region{0, 0, windowSize.width, windowSize.height});
            contUpdateResume = true;
        }

        // create event for resize (resized in main thread)
        SDL_UserEvent event;
        event.type = SDL_USEREVENT;
        event.code = contUpdateResume ? LocalEvent::ResizeCont :
                          LocalEvent::Resize;
        event.data1 = (void*)(ptrdiff_t) nsz.width;
        event.data2 = (void*)(ptrdiff_t) nsz.height;

        if(0 > SDL_PushEvent((SDL_Event*) & event))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_PushEvent",
                               SDL_GetError());
            return false;
        }

        return true;
    }

    void Vnc2SDL::decodingExtDesktopSizeEvent(int status, int err,
            const XCB::Size & nsz, const std::vector<RFB::ScreenInfo> & screens)
    {
        needUpdate = false;

        // 1. server request: status: 0x00, error: 0x00
        if(status == 0 && err == 0)
        {
            // negotiate part
            if(! serverExtDesktopSizeSupported)
            {
                serverExtDesktopSizeSupported = true;

                if(! setGeometry.isEmpty() && setGeometry != windowSize)
                {
                    sendSetDesktopSize(setGeometry);
                }
            }
            // server runtime
            else
            {
                if(fullscreen && setGeometry != nsz)
                {
                    Application::warning("%s: fullscreen mode: [%" PRIu16 ", %" PRIu16
                                         "], server request resize desktop: [%" PRIu16 ", %" PRIu16 "]",
                                         __FUNCTION__, setGeometry.width, setGeometry.height, nsz.width, nsz.height);
                }

                pushEventWindowResize(nsz);
            }
        }
        else

            // 3. server reply
            if(status == 1)
            {
                if(0 == err)
                {
                    pushEventWindowResize(nsz);
                }
                else
                {
                    Application::error("%s: status: %d, error code: %d", __FUNCTION__, status, err);

                    if(nsz.isEmpty())
                    {
                        throw sdl_error(NS_FuncName);
                    }

                    pushEventWindowResize(nsz);
                    setGeometry.reset();
                }
            }
    }

    void Vnc2SDL::fbUpdateEvent(void)
    {
        if(! needUpdate)
        {
            needUpdate = true;
        }
    }

    void Vnc2SDL::pixelFormatEvent(const PixelFormat & pf, const XCB::Size & wsz)
    {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                          wsz.width, wsz.height);
        const std::scoped_lock guard{ renderLock };

        if(! window)
        {
            window.reset(new SDL::Window("VNC2SDL", wsz.width, wsz.height, 0, 0, 0,
                                         accelerated));
        }

        auto pair = window->geometry();
        windowSize = XCB::Size(pair.first, pair.second);
        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(SDL_TRUE != SDL_PixelFormatEnumToMasks(window->pixelFormat(), &bpp, &rmask,
                &gmask, &bmask, &amask))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__,
                               "SDL_PixelFormatEnumToMasks", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        clientPf = PixelFormat(bpp, rmask, gmask, bmask, amask);
        displayResizeEvent(windowSize);
    }

    void Vnc2SDL::setPixel(const XCB::Point & dst, uint32_t pixel)
    {
        fillPixel({dst, XCB::Size{1, 1}}, pixel);
    }

    void Vnc2SDL::fillPixel(const XCB::Region & dst, uint32_t pixel)
    {
        const std::scoped_lock guard{ renderLock };

        if(! sfback || sfback->w != windowSize.width || sfback->h != windowSize.height)
        {
            sfback.reset(SDL_CreateRGBSurface(0, windowSize.width, windowSize.height,
                                              clientPf.bitsPerPixel,
                                              clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask()));

            if(! sfback)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateSurface", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }

        SDL_Rect dstrt{ .x = dst.x, .y = dst.y, .w = dst.width, .h = dst.height };
        auto col = clientPf.color(pixel);
        Uint32 color = SDL_MapRGB(sfback->format, col.r, col.g, col.b);

        if(0 > SDL_FillRect(sfback.get(), &dstrt, color))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_FillRect",
                               SDL_GetError());
            throw sdl_error(NS_FuncName);
        }
    }

    void Vnc2SDL::updateRawPixels(const void* data, const XCB::Size & wsz,
                                  uint16_t pitch, const PixelFormat & pf)
    {
        const std::scoped_lock guard{ renderLock };

        if(windowSize == wsz)
        {
            std::unique_ptr<SDL_Surface, SurfaceDeleter> sfframe;

            if(! sfback || sfback->w != windowSize.width || sfback->h != windowSize.height)
            {
                sfback.reset(SDL_CreateRGBSurface(0, windowSize.width, windowSize.height,
                                                  clientPf.bitsPerPixel,
                                                  clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask()));

                if(! sfback)
                {
                    Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                       "SDL_CreateSurface", SDL_GetError());
                    throw sdl_error(NS_FuncName);
                }
            }

            sfframe.reset(SDL_CreateRGBSurfaceFrom((void*) data, wsz.width, wsz.height,
                                                   pf.bitsPerPixel, pitch,
                                                   pf.rmask(), pf.gmask(), pf.bmask(), pf.amask()));

            if(! sfframe)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateSurfaceFrom", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }

            if(0 > SDL_BlitSurface(sfframe.get(), nullptr, sfback.get(), nullptr))
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_BlitSurface",
                                   SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }
        else
        {
            Application::warning("%s: incorrect geometry, win size: [%" PRIu16 ", %" PRIu16
                                 "], frame size: [%" PRIu16 ", %" PRIu16 "]",
                                 __FUNCTION__, windowSize.width, windowSize.height, wsz.width, wsz.height);
        }
    }

    const PixelFormat & Vnc2SDL::clientFormat(void) const
    {
        return clientPf;
    }

    XCB::Size Vnc2SDL::clientSize(void) const
    {
        return windowSize;
    }

    std::string Vnc2SDL::clientEncoding(void) const
    {
        return prefferedEncoding;
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

        if(0 != SDL_SetClipboardText(reinterpret_cast<const char*>
                                     (clipboardBufRemote.data())))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__,
                               "SDL_SetClipboardText", SDL_GetError());
        }

        if(zeroInserted)
        {
            clipboardBufRemote.pop_back();
        }
    }

    void Vnc2SDL::richCursorEvent(const XCB::Region & reg,
                                  std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask)
    {
        uint32_t key = Tools::crc32b(pixels.data(), pixels.size());
        auto it = cursors.find(key);

        if(cursors.end() == it)
        {
            auto sdlFormat = SDL_MasksToPixelFormatEnum(clientPf.bitsPerPixel,
                             clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask());

            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            Application::debug("%s: create cursor, crc32b: %" PRIu32 ", size: [%" PRIu16
                               ", %" PRIu16 "], sdl format: %s",
                               __FUNCTION__, key, reg.width, reg.height, SDL_GetPixelFormatName(sdlFormat));
            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width,
                      reg.height, clientPf.bitsPerPixel, reg.width * clientPf.bytePerPixel(),
                      sdlFormat);

            if(! sf)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
                return;
            }

            auto pair = cursors.emplace(key, ColorCursor{ .pixels = std::move(pixels) });
            it = pair.first;
            (*it).second.surface.reset(sf);
            auto curs = SDL_CreateColorCursor(sf, reg.x, reg.y);

            if(! curs)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateColorCursor", SDL_GetError());
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
            sendSystemClientVariables(clientOptions(), clientEnvironments(), names,
                                      (0 <= group && group < names.size() ? names[group] : ""));
            sendOptions = true;
        }
    }

    void Vnc2SDL::xkbStateChangeEvent(int group)
    {
        if(usexkb)
        {
            sendSystemKeyboardChange(xkbNames(), group);
        }
    }

    json_plain Vnc2SDL::clientEnvironments(void) const
    {
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
            { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" }
        };
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
        {
            jo.push("XSESSION", seamless);
        }

        return jo.flush();
    }

    json_plain Vnc2SDL::clientOptions(void) const
    {
        // other
        JsonObjectStream jo;
        jo.push("hostname", "localhost");
        jo.push("ipaddr", "127.0.0.1");
        jo.push("platform", SDL_GetPlatform());
        jo.push("ltsm:client", LTSM_VNC2SDL_VERSION);

        if(username.empty())
        {
            if(auto env = std::getenv("USER"))
                jo.push("username", env);
            else
            if(auto env = std::getenv("USERNAME"))
                jo.push("username", env);
        }
        else
        {
            jo.push("username", username);
        }

        if(! rfbsec.passwdFile.empty())
        {
            jo.push("password", rfbsec.passwdFile);
        }

        if(! rfbsec.certFile.empty())
        {
            jo.push("certificate", Tools::fileToString(rfbsec.certFile));
        }

        if(! printerUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "printer",
                              printerUrl.c_str());
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

        if(! shareFolders.empty())
        {

            jo.push("fuse", Tools::join(shareFolders.begin(), shareFolders.end(), "|"));
        }

        if(audioEnable)
        {
            std::forward_list<std::string> allowEncoding = { "auto", "pcm" };
#ifdef LTSM_WITH_OPUS
            allowEncoding.emplace_front("opus");
#endif
            if(std::any_of(allowEncoding.begin(), allowEncoding.end(), [&](auto & enc){ return enc == audioEncoding; }))
                jo.push("audio", audioEncoding);
            else
                Application::warning("%s: unsupported audio: %s", __FUNCTION__, audioEncoding.c_str());
        }

        return jo.flush();
    }

    void Vnc2SDL::systemTokenAuth(const JsonObject & jo)
    {
        auto action = jo.getString("action");
        auto serial = jo.getString("serial");
        Application::info("%s: action: %s, serial: %s", __FUNCTION__, action.c_str(),
                          serial.c_str());

        if(! token)
        {
            Application::error("%s: token api not loaded", __FUNCTION__);
            return;
        }

        if(action == "check")
        {
            std::thread([this, ptr = token.get(), serial, pin = jo.getString("pin"),
                               cert = jo.getInteger("cert"), content = jo.getString("data")]
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
            {
                token.reset();
            }
        }
        else
        {
            auto error = jo.getString("error");
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "login",
                               error.c_str());
        }
    }

    void Vnc2SDL::bellEvent(void)
    {
        bell(75);
    }

    bool Vnc2SDL::createChannelAllow(const Channel::ConnectorType & type, const std::string & content, const Channel::ConnectorMode & mode) const
    {
        if(type == Channel::ConnectorType::Fuse)
        {
            if(! std::any_of(shareFolders.begin(), shareFolders.end(), [&](auto & val){ return val == content; }))
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "share", content.c_str());
                return false;
            }
        }

        return true;
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
            {
                programRestarting = false;
            }

            res = app.start();
        }
        catch(const std::invalid_argument & err)
        {
            std::cerr << "unknown params: " << err.what() << std::endl << std::endl;
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
