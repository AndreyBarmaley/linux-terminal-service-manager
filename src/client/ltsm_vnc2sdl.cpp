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

#ifdef __WIN32__
#include <winsock2.h>
#endif

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
    const auto librtdef = "/usr/lib64/librtpkcs11ecp.so";
    const auto printdef = "cmd:///usr/bin/lpr";
    const auto krb5def = "TERMSRV@remotehost.name";
    const auto windowTitle = "LTSM_client";

    void printHelp(const char* prog, const std::list<int> & encodings)
    {
        std::cout << std::endl <<
                  prog << " version: " << LTSM_VNC2SDL_VERSION << std::endl;
        std::cout << std::endl <<
                  "usage: " << prog <<
                  ": --host <localhost> [--port 5900] [--password <pass>] [password-file <file>] " <<
                  "[--version] [--debug [<types>]] [--trace] [--syslog [<tofile>]] " <<
                  "[--noltsm] [--noaccel] [--fullscreen] [--geometry <WIDTHxHEIGHT>] [--fixed] " <<
#ifdef LTSM_WITH_GSSAPI
                  "[--kerberos <" << krb5def << ">] " <<
#endif
#ifdef LTSM_DECODING
 #ifdef LTSM_DECODING_QOI
                  "[--qoi] " <<
 #endif
 #ifdef LTSM_DECODING_LZ4
                  "[--lz4] " <<
 #endif
 #ifdef LTSM_DECODING_TJPG
                  "[--tjpg] " <<
 #endif
#endif
#ifdef LTSM_DECODING_FFMPEG
 #ifdef LTSM_DECODING_H264
                  "[--h264] " <<
 #endif
 #ifdef LTSM_DECODING_AV1
                  "[--av1] " <<
 #endif
 #ifdef LTSM_DECODING_VP8
                  "[--vp8] " <<
 #endif
#endif
                  "[--encoding <string>] " <<
#ifdef LTSM_WITH_GNUTLS
                  "[--notls] [--tls-priority <string>] [--tls-ca-file <path>] [--tls-cert-file <path>] [--tls-key-file <path>] " <<
#endif
#ifdef LTSM_WITH_FUSE
                  "[--share-folder <folder>] " <<
#endif
                  "[--printer [" << printdef << "]] " << "[--sane [" << sanedef << "]] " <<
#ifdef LTSM_PKCS11_AUTH
                  "[--pcsc11-auth [" << librtdef << "]] " <<
#endif
#ifdef LTSM_WITH_PCSC
                  "[--pcsc] " <<
#endif
                  "[--noxkb] [--nocaps] [--loop] [--seamless <path>] " << std::endl;
        std::cout << std::endl << "arguments:" << std::endl <<
                  "    --debug <types> (allow types: [all],xcb,rfb,clip,sock,tls,chnl,conn,enc,x11srv,x11cli,audio,fuse,pcsc,pkcs11,sdl,app,ldap,gss,mgr)" << std::endl <<
                  "    --trace (big more debug)" << std::endl <<
                  "    --syslog (to syslog or <file>)" << std::endl <<
                  "    --host <localhost> " << std::endl <<
                  "    --port <port> " << std::endl <<
                  "    --username <user> " << std::endl <<
                  "    --password <pass> " << std::endl <<
                  "    --password-file <file> (password from file or STDIN)" << std::endl <<
                  "    --noaccel (disable SDL2 acceleration)" << std::endl <<
                  "    --fullscreen (switch to fullscreen mode, Ctrl+F10 toggle)" << std::endl <<
                  "    --nodamage (skip X11 damage events)" << std::endl <<
                  "    --framerate <fps>" << std::endl <<
                  "    --geometry <WIDTHxHEIGHT> (set window geometry)" << std::endl <<
                  "    --fixed (not resizable window)" << std::endl <<
                  "    --extclip (extclip support)" << std::endl <<
                  "    --noltsm (disable LTSM features, viewer only)" << std::endl <<
#ifdef LTSM_WITH_GNUTLS
                  "    --notls (disable tls1.2, the server may reject the connection)" << std::endl <<
#endif
#ifdef LTSM_WITH_GSSAPI
                  "    --kerberos <" << krb5def <<
                  "> (kerberos auth, may be use --username for token name)" << std::endl <<
#endif
#ifdef LTSM_DECODING
 #ifdef LTSM_DECODING_QOI
                  "    --qoi (the same as --encoding ltsm_qoi)" << std::endl <<
 #endif
 #ifdef LTSM_DECODING_LZ4
                   "    --lz4 (the same as --encoding ltsm_lz4)" << std::endl <<
 #endif
 #ifdef LTSM_DECODING_TJPG
                  "    --tjpg (the same as --encoding ltsm_tjpg)" << std::endl <<
 #endif
#endif
#ifdef LTSM_DECODING_FFMPEG
 #ifdef LTSM_DECODING_H264
                  "    --h264 (the same as --encoding ffmpeg_h264)" << std::endl <<
 #endif
 #ifdef LTSM_DECODING_AV1
                  "    --av1 (the same as --encoding ffmpeg_av1)" << std::endl <<
 #endif
 #ifdef LTSM_DECODING_VP8
                  "    --vp8 (the same as --encoding ffmpeg_vp8)" << std::endl <<
 #endif
#endif
                  "    --encoding <string> (set preffered encoding)" << std::endl <<
#ifdef LTSM_WITH_GNUTLS
                  "    --tls-priority <string> " << std::endl <<
                  "    --tls-ca-file <path> " << std::endl <<
                  "    --tls-cert-file <path> " << std::endl <<
                  "    --tls-key-file <path> " << std::endl <<
#endif
#ifdef LTSM_WITH_FUSE
                  "    --share-folder <folder> (redirect folder)" << std::endl <<
#endif
                  "    --seamless <path> (seamless remote program)" << std::endl <<
                  "    --noxkb (disable send xkb)" << std::endl <<
                  "    --nocaps (disable send capslock)" << std::endl <<
                  "    --loop (always reconnecting)" << std::endl <<
                  "    --audio [ " <<
#ifdef LTSM_WITH_OPUS
                  "opus, pcm" <<
#else
                  "pcm" <<
#endif
                  " ] (audio support)" << std::endl <<
                  "    --printer [" << printdef << "] (redirect printer)" << std::endl <<
                  "    --sane [" << sanedef << "] (redirect scanner)" << std::endl <<
#ifdef LTSM_WITH_PCSC
                  "    --pcsc (redirect smartcard)" << std::endl <<
#endif
#ifdef LTSM_PKCS11_AUTH
                  "    --pkcs11-auth [" << librtdef << "] (pkcs11 autenfication, and the user's certificate is in the LDAP database)" <<
#endif
                  std::endl;
        std::cout << std::endl << "supported encodings: " << std::endl <<
                  "    ";

        for(const auto & enc: encodings)
        {
            if(RFB::isVideoEncoding(enc))
                std::cout << Tools::lower(RFB::encodingName(enc)) << " ";
        }

        std::cout << std::endl;
        std::cout << std::endl << "encoding options: " << std::endl;

        for(const auto & enc: encodings)
        {
            if(auto opts = RFB::encodingOpts(enc); ! opts.empty())
            {
                std::cout << "    " << opts << std::endl;
            }
        }
        std::cout << std::endl;
    }

    std::list<int> Vnc2SDL::clientSupportedEncodings(void) const
    {
        std::list<int> encodings = { 
            // first preffered
#ifdef LTSM_DECODING
 #ifdef LTSM_DECODING_QOI
            RFB::ENCODING_LTSM_QOI,
 #endif
 #ifdef LTSM_DECODING_LZ4
            RFB::ENCODING_LTSM_LZ4,
 #endif
 #ifdef LTSM_DECODING_TJPG
            RFB::ENCODING_LTSM_TJPG,
 #endif
#endif
#ifdef LTSM_DECODING_FFMPEG
 #ifdef LTSM_DECODING_H264
            RFB::ENCODING_FFMPEG_H264,
 #endif
 #ifdef LTSM_DECODING_AV1
            RFB::ENCODING_FFMPEG_AV1,
 #endif
 #ifdef LTSM_DECODING_VP8
            RFB::ENCODING_FFMPEG_VP8,
 #endif
#endif
        };

        encodings.push_back(RFB::ENCODING_LTSM_CURSOR);

        if(extClipboardLocalCaps())
        {
            encodings.push_back( RFB::ENCODING_EXT_CLIPBOARD );
        }

        return encodings;
    }

    Vnc2SDL::Vnc2SDL(int argc, const char** argv)
        : Application("ltsm_client")
    {
        Application::setDebug(DebugTarget::Console, DebugLevel::Info);
#ifdef LTSM_WITH_GNUTLS
        rfbsec.authVenCrypt = true;
        rfbsec.tlsDebug = 2;
#else
        rfbsec.authVenCrypt = false;
#endif

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
                capslockEnable = false;
            }
            else if(0 == std::strcmp(argv[it], "--noltsm"))
            {
                ltsmSupport = false;
            }
            else if(0 == std::strcmp(argv[it], "--noaccel"))
            {
                windowAccel = false;
            }
#ifdef LTSM_WITH_GNUTLS
            else if(0 == std::strcmp(argv[it], "--notls"))
            {
                rfbsec.authVenCrypt = false;
            }
#endif
            else if(0 == std::strcmp(argv[it], "--noxkb"))
            {
                useXkb = false;
            }
            else if(0 == std::strcmp(argv[it], "--loop"))
            {
                alwaysRunning = true;
            }
            else if(0 == std::strcmp(argv[it], "--fullscreen"))
            {
                windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            }
            else if(0 == std::strcmp(argv[it], "--fixed"))
            {
                windowFlags &= ~SDL_WINDOW_RESIZABLE;
            }
            else if(0 == std::strcmp(argv[it], "--nodamage"))
            {
                xcbNoDamage = true;
            }
#ifdef LTSM_WITH_PCSC
            else if(0 == std::strcmp(argv[it], "--pcsc"))
            {
                pcscEnable = true;
            }
#endif
            else if(0 == std::strcmp(argv[it], "--extclip"))
            {
                setExtClipboardLocalCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);
            }

#ifdef LTSM_DECODING
            else if(0 == std::strcmp(argv[it], "--qoi"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_LTSM_QOI)));
            }
            else if(0 == std::strcmp(argv[it], "--lz4"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_LTSM_LZ4)));
            }
            else if(0 == std::strcmp(argv[it], "--tjpg"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_LTSM_TJPG)));
            }
            else if(0 == std::strncmp(argv[it], "--tjpg,", 7))
            {
                auto opts = Tools::split(argv[it], ',');

                opts.pop_front();
                encodingOptions.assign(opts.begin(), opts.end());

                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_LTSM_TJPG)));
            }
#endif
#ifdef LTSM_DECODING_FFMPEG
 #ifdef LTSM_DECODING_H264
            else if(0 == std::strcmp(argv[it], "--h264"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_H264)));
            }
 #endif
 #ifdef LTSM_DECODING_AV1
            else if(0 == std::strcmp(argv[it], "--av1"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_AV1)));
            }
 #endif
 #ifdef LTSM_DECODING_VP8
            else if(0 == std::strcmp(argv[it], "--vp8"))
            {
                prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                        RFB::ENCODING_FFMPEG_VP8)));
            }
 #endif
#endif
            else if(0 == std::strcmp(argv[it], "--encoding"))
            {
                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2) /* not --param */)
                {
                    auto opts = Tools::split(argv[it + 1], ',');

                    prefferedEncoding.assign(Tools::lower(opts.front()));

                    opts.pop_front();
                    encodingOptions.assign(opts.begin(), opts.end());

                    it = it + 1;
                }

                auto encodings = supportedEncodings();

                if(std::none_of(encodings.begin(), encodings.end(), [&](auto & str) { return Tools::lower(RFB::encodingName(str)) == prefferedEncoding; }))
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
#ifdef LTSM_PKCS11_AUTH
            else if(0 == std::strcmp(argv[it], "--pkcs11-auth"))
            {
                pkcs11Auth.assign(librtdef);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2))
                {
                    pkcs11Auth.assign(argv[it + 1]);
                    it = it + 1;
                }

                if(! std::filesystem::exists(pkcs11Auth))
                {
                    Application::warning("%s: parse %s failed, not exist: %s", __FUNCTION__,
                                         "pkcs11-auth", pkcs11Auth.c_str());
                    pkcs11Auth.clear();
                }
            }
#endif
            else if(0 == std::strcmp(argv[it], "--trace"))
            {
                Application::setDebugLevel(DebugLevel::Trace);
            }
            else if(0 == std::strcmp(argv[it], "--debug"))
            {
                if(! Application::isDebugLevel(DebugLevel::Trace))
                    Application::setDebugLevel(DebugLevel::Debug);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2) /* not --param */)
                {
                    Application::setDebugTypes(Tools::debugTypes(Tools::split(argv[it + 1], ',')));
                    it = it + 1;
                }

            }
            else if(0 == std::strcmp(argv[it], "--syslog"))
            {
                Application::setDebugTarget(DebugTarget::Syslog);

                if(it + 1 < argc && std::strncmp(argv[it + 1], "--", 2) /* not --param */)
                {
                    Application::setDebugTargetFile(argv[it + 1]);
                    it = it + 1;
                }
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
                {
                    shareFolders.emplace_front(argv[it + 1]);
                }

                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--password") && it + 1 < argc)
            {
                rfbsec.passwdFile.assign(argv[it + 1]);
                it = it + 1;
            }
            else if(0 == std::strcmp(argv[it], "--password-file") && it + 1 < argc)
            {
                passfile.assign(argv[it + 1]);
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
            else if(0 == std::strcmp(argv[it], "--framerate") && it + 1 < argc)
            {
                try
                {
                    frameRate = std::stoi(argv[it + 1]);

                    if(frameRate < 5)
                    {
                        frameRate = 5;
                        std::cerr << "set frame rate: " << frameRate << std::endl;
                    }
                    else if(frameRate > 25)
                    {
                        frameRate = 25;
                        std::cerr << "set frame rate: " << frameRate << std::endl;
                    }
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "incorrect frame rate" << std::endl;
                    frameRate = 16;
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
                    primarySize = XCB::Size(width, height);
                }
                catch(const std::invalid_argument &)
                {
                    std::cerr << "invalid geometry" << std::endl;
                }

                it = it + 1;
            }
#ifdef LTSM_WITH_GNUTLS
            else if(0 == std::strcmp(argv[it], "--tls-priority") && it + 1 < argc)
            {
                rfbsec.tlsPriority.assign(argv[it + 1]);
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
#endif
            else
            {
                throw std::invalid_argument(argv[it]);
            }
        }

        if(pkcs11Auth.size() && rfbsec.passwdFile.size() && username.size())
        {
            pkcs11Auth.clear();
        }

        // fullscreen
        if(windowFullScreen())
        {
            SDL_DisplayMode mode;

            if(0 == SDL_GetDisplayMode(0, 0, & mode))
            {
                primarySize = XCB::Size(mode.w, mode.h);

                if(primarySize.width < primarySize.height)
                {
                    std::swap(primarySize.width, primarySize.height);
                }
            }
        }
    }

    bool Vnc2SDL::windowFullScreen(void) const
    {
        return windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    bool Vnc2SDL::windowResizable(void) const
    {
        return windowFlags & SDL_WINDOW_RESIZABLE;
    }
    
    bool Vnc2SDL::isAlwaysRunning(void) const
    {
        return alwaysRunning;
    }

    const char* Vnc2SDL::pkcs11Library(void) const
    {
#ifdef LTSM_PKCS11_AUTH
        return pkcs11Auth.c_str();
#else
        return nullptr;
#endif
    }

    int Vnc2SDL::start(void)
    {
        auto ipaddr = TCPSocket::resolvHostname(host);
        int sockfd = TCPSocket::connect(ipaddr, port);

        if(0 > sockfd)
        {
            return -1;
        }

        if(rfbsec.passwdFile.empty())
        {
            if(auto env = std::getenv("LTSM_PASSWORD"))
            {
                rfbsec.passwdFile.assign(env);
            }

            if(passfile == "-" || Tools::lower(passfile) == "stdin")
            {
                std::getline(std::cin, rfbsec.passwdFile);
            }
            else if(std::filesystem::is_regular_file(passfile))
            {
                std::ifstream ifs(passfile);

                if(ifs)
                {
                    std::getline(ifs, rfbsec.passwdFile);
                }
            }
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
                {
                    rfbsec.krb5Name.assign(env);
                }
                else if(auto env = std::getenv("USERNAME"))
                {
                    rfbsec.krb5Name.assign(env);
                }
            }
            else
            {
                rfbsec.krb5Name.assign(username);
            }
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
        auto thrfb = std::thread([this]()
        {
            this->rfbMessagesLoop();
        });

        // xcb thread: wait xkb event
        auto thxcb = std::thread([this]()
        {
            while(this->rfbMessagesRunning())
            {
#ifdef __UNIX__
                if(auto err = XCB::RootDisplay::hasError())
                {
                    Application::warning("%s: x11 error: %d", __FUNCTION__, err);
                    this->rfbMessagesShutdown();
                    break;
                }

                if(auto ev = XCB::RootDisplay::pollEvent())
                {
                    if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)))
                    {
                        uint16_t opcode = 0;

                        if(extXkb->isEventError(ev, & opcode))
                        {
                            Application::warning("%s: %s error: 0x%04" PRIx16, __FUNCTION__, "xkb", opcode);
                        }
                    }
                }
#endif

                std::this_thread::sleep_for(50ms);
            }
        });

        if(isContinueUpdatesSupport())
        {
            sendContinuousUpdates(true, { XCB::Point(0, 0), windowSize });
        }

        // main thread: sdl processing
//        auto clipboardDelay = std::chrono::steady_clock::now();
//        std::thread thclip;

        while(true)
        {
            if(! rfbMessagesRunning())
            {
                break;
            }

            if(! dropFiles.empty() &&
                    std::chrono::steady_clock::now() - dropStart > 700ms)
            {
                ChannelClient::sendSystemTransferFiles(dropFiles);
                dropFiles.clear();
            }

/*
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
                                this->sendCutTextEvent(clipboardBufSdl.data(), clipboardBufSdl.size(), false);
                            }

                            SDL_free(ptr);
                        }
                    });
                }

                clipboardDelay = std::chrono::steady_clock::now();
            }
*/
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

/*
        if(thclip.joinable())
        {
            thclip.join();
        }
*/
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
                else if(SDL_WINDOWEVENT_FOCUS_GAINED == ev.window()->event)
                {
                    focusLost = false;
                }
                else if(SDL_WINDOWEVENT_FOCUS_LOST == ev.window()->event)
                {
                    focusLost = true;
                }
                else if(SDL_WINDOWEVENT_SIZE_CHANGED == ev.window()->event)
                {
                    Application::info("%s: size changed: [%" PRId32 "x%" PRId32 "]",
                            __FUNCTION__, ev.window()->data1, ev.window()->data2);
                }
                else if(SDL_WINDOWEVENT_RESIZED == ev.window()->event)
                {
                    Application::info("%s: event resized: [%" PRId32 "x%" PRId32 "]",
                            __FUNCTION__, ev.window()->data1, ev.window()->data2);
//                    Application::debug(DebugType::Sdl, "%s: resized window: [%" PRId32 "x%" PRId32 "]",
//                            __FUNCTION__, ev.window()->data1, ev.window()->data2);
                    windowResizedEvent(ev.window()->data1, ev.window()->data2);
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
                    if(windowFullScreen())
                    {
                        SDL_SetWindowFullscreen(window->get(), 0);
                        windowFlags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
                    }
                    else
                    {
                        SDL_SetWindowFullscreen(window->get(), SDL_WINDOW_FULLSCREEN);
                        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
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
                if(ev.key()->keysym.sym == 0x40000000 && ! capslockEnable)
                {
                    auto mod = SDL_GetModState();
                    SDL_SetModState(static_cast<SDL_Keymod>(mod & ~KMOD_CAPS));
                    Application::notice("%s: CAPS reset", __FUNCTION__);
                    return true;
                }
                else
                {
#ifdef __UNIX__
                    int xksym = SDL::Window::convertScanCodeToKeySym(ev.key()->keysym.scancode);

                    if(xksym == 0)
                    {
                        xksym = ev.key()->keysym.sym;
                    }

                    if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)); extXkb && useXkb)
                    {
                        int group = extXkb->getLayoutGroup();
                        auto keycodeGroup = keysymToKeycodeGroup(xksym);

                        if(group != keycodeGroup.second)
                        {
                            xksym = keycodeGroupToKeysym(keycodeGroup.first, group);
                        }
                    }
#else
                    int xksym = ev.key()->keysym.sym;
#endif
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

                    if(windowFullScreen())
                    {
                        window.reset(new SDL::Window(windowTitle, width, height, 0, 0, windowFlags, windowAccel));
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

    void Vnc2SDL::clientRecvDecodingDesktopSizeEvent(int status, int err,
            const XCB::Size & nsz, const std::vector<RFB::ScreenInfo> & screens)
    {
        needUpdate = false;

        // 1. server request: status: 0x00, error: 0x00
        if(status == 0 && err == 0)
        {
            // negotiate part
            if(! serverExtDesktopSizeNego)
            {
                serverExtDesktopSizeNego = true;

                if(! primarySize.isEmpty() && primarySize != windowSize)
                {
                    sendSetDesktopSize(primarySize);
                }
            }
            // server runtime
            else
            {
                if(windowFullScreen() && primarySize != nsz)
                {
                    Application::warning("%s: fullscreen mode: [%" PRIu16 ", %" PRIu16
                                         "], server request resize desktop: [%" PRIu16 ", %" PRIu16 "]",
                                         __FUNCTION__, primarySize.width, primarySize.height, nsz.width, nsz.height);
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
                    primarySize.reset();
                }
            }
    }

    void Vnc2SDL::clientRecvFBUpdateEvent(void)
    {
        if(! needUpdate)
        {
            needUpdate = true;
        }
    }

    void Vnc2SDL::clientRecvPixelFormatEvent(const PixelFormat & pf, const XCB::Size & wsz)
    {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                          wsz.width, wsz.height);
        const std::scoped_lock guard{ renderLock };
        bool eventResize = false;

        if(! window)
        {
            window.reset(new SDL::Window(windowTitle, wsz.width, wsz.height, 0, 0, windowFlags, windowAccel));
            eventResize = true;
        }

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

        if(eventResize)
        {
            auto pair = window->geometry();
            windowSize = XCB::Size(pair.first, pair.second);
            displayResizeEvent(windowSize);
        }
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
                                              clientPf.bitsPerPixel(),
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

    void Vnc2SDL::updateRawPixels(const void* data, const XCB::Region & wrt,
                                  uint32_t pitch, const PixelFormat & pf)
    {
        uint32_t sdlFormat = SDL_MasksToPixelFormatEnum(pf.bitsPerPixel(), pf.rmask(), pf.gmask(), pf.bmask(), pf.amask());

        if(sdlFormat != SDL_PIXELFORMAT_UNKNOWN)
        {
            return updateRawPixels2(data, wrt, pf.bitsPerPixel(), pitch, sdlFormat);
        }

        // lock part
        const std::scoped_lock guard{ renderLock };

        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> sfframe{
            SDL_CreateRGBSurfaceFrom((void*) data, wrt.width, wrt.height,
                                                   pf.bitsPerPixel(), pitch, pf.rmask(), pf.gmask(), pf.bmask(), pf.amask()),
            SDL_FreeSurface};

        if(! sfframe)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateRGBSurfaceFrom", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        updateRawPixels3(sfframe.get(), wrt);
    }

    void Vnc2SDL::updateRawPixels2(const void* data, const XCB::Region & wrt, uint8_t depth, uint32_t pitch, uint32_t sdlFormat)
    {
        // lock part
        const std::scoped_lock guard{ renderLock };

        std::unique_ptr<SDL_Surface, void(*)(SDL_Surface*)> sfframe{
            SDL_CreateRGBSurfaceWithFormatFrom((void*) data, wrt.width, wrt.height,
                                                   depth, pitch, sdlFormat),
            SDL_FreeSurface};

        if(! sfframe)
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
            throw sdl_error(NS_FuncName);
        }

        updateRawPixels3(sfframe.get(), wrt);
    }

    void Vnc2SDL::updateRawPixels3(SDL_Surface* sfframe, const XCB::Region & wrt)
    {
        if(! sfback || sfback->w != windowSize.width || sfback->h != windowSize.height)
        {
            sfback.reset(SDL_CreateRGBSurface(0, windowSize.width, windowSize.height,
                                                  clientPf.bitsPerPixel(),
                                                  clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask()));
            if(! sfback)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                       "SDL_CreateSurface", SDL_GetError());
                throw sdl_error(NS_FuncName);
            }
        }

        SDL_Rect dstrt{ .x = wrt.x, .y = wrt.y, .w = wrt.width, .h = wrt.height };

        if(0 > SDL_BlitSurface(sfframe, nullptr, sfback.get(), & dstrt))
        {
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "SDL_BlitSurface",
                                   SDL_GetError());
            throw sdl_error(NS_FuncName);
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

    std::string Vnc2SDL::clientPrefferedEncoding(void) const
    {
        return prefferedEncoding;
    }

/*
    void Vnc2SDL::clientRecvCutTextEvent(std::vector<uint8_t> && buf)
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
*/
    void Vnc2SDL::clientRecvRichCursorEvent(const XCB::Region & reg,
                                  std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask)
    {
        uint32_t key = Tools::crc32b(pixels.data(), pixels.size());
        auto it = cursors.find(key);

        if(cursors.end() == it)
        {
            auto sdlFormat = SDL_MasksToPixelFormatEnum(clientPf.bitsPerPixel(),
                             clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask());

            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            // pixels data as client format
            Application::debug(DebugType::App, "%s: create cursor, crc32b: %" PRIu32 ", size: [%" PRIu16
                               ", %" PRIu16 "], sdl format: %s",
                               __FUNCTION__, key, reg.width, reg.height, SDL_GetPixelFormatName(sdlFormat));

            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width,
                      reg.height, clientPf.bitsPerPixel(), reg.width * clientPf.bytePerPixel(),
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
                auto & pixels = (*it).second.pixels;
                auto tmp1 = Tools::buffer2hexstring(pixels.begin(), pixels.end(), 2, ",", false);
                auto tmp2 = Tools::buffer2hexstring(mask.begin(), mask.end(), 2, ",", false);

                Application::warning("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateColorCursor", SDL_GetError());
                Application::warning("%s: pixels: [%s], mask: [%s]", __FUNCTION__, tmp1.c_str(), tmp2.c_str());
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void Vnc2SDL::clientRecvLtsmCursorEvent(const XCB::Region & reg, uint32_t cursorId, std::vector<uint8_t> && pixels)
    {
        auto it = cursors.find(cursorId);

        if(cursors.end() == it)
        {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            auto cursorFmt = BGRA32;
#else
            auto cursorFmt = ARGB32;
#endif
            auto sdlFormat = SDL_MasksToPixelFormatEnum(cursorFmt.bitsPerPixel(),
                             cursorFmt.rmask(), cursorFmt.gmask(), cursorFmt.bmask(), cursorFmt.amask());

            if(pixels.size() < static_cast<size_t>(reg.width) * reg.height * 4)
            {
                Application::error("%s: invalid pixels, length: %lu", __FUNCTION__, pixels.size());
                return;
            }

            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            // pixels data as client format
            Application::debug(DebugType::App, "%s: create cursor, crc32b: %" PRIu32 ", size: [%" PRIu16
                               ", %" PRIu16 "], sdl format: %s",
                               __FUNCTION__, cursorId, reg.width, reg.height, SDL_GetPixelFormatName(sdlFormat));

            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width,
                      reg.height, clientPf.bitsPerPixel(), reg.width * cursorFmt.bytePerPixel(),
                      sdlFormat);

            if(! sf)
            {
                Application::error("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
                return;
            }

            auto pair = cursors.emplace(cursorId, ColorCursor{ .pixels = std::move(pixels) });
            it = pair.first;
            (*it).second.surface.reset(sf);
            auto curs = SDL_CreateColorCursor(sf, reg.x, reg.y);

            if(! curs)
            {
                Application::warning("%s: %s failed, error: %s", __FUNCTION__,
                                   "SDL_CreateColorCursor", SDL_GetError());
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void Vnc2SDL::clientRecvLtsmHandshakeEvent(int flags)
    {
        if(! sendOptions)
        {
            std::vector<std::string> names;
            int group = 0;

#ifdef __UNIX__
            if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)))
            {
                names = extXkb->getNames();
                group = extXkb->getLayoutGroup();
            }
#endif
            sendSystemClientVariables(clientOptions(), clientEnvironments(), names,
                                      (0 <= group && group < names.size() ? names[group] : ""));
            sendOptions = true;
        }
    }

#ifdef __UNIX__
    void Vnc2SDL::xcbXkbGroupChangedEvent(int group)
    {
        if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)); extXkb && useXkb)
        {
            sendSystemKeyboardChange(extXkb->getNames(), group);
        }
    }
#endif

    json_plain Vnc2SDL::clientEnvironments(void) const
    {
        JsonObjectStream jo;
#ifdef __UNIX__
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
            { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" }
        };

        for(const auto & lc : lcall)
        {
            auto ptr = std::setlocale(lc.first, "");
            jo.push(lc.second, ptr ? ptr : "C");
        }
#endif
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
#ifdef __UNIX__
        jo.push("build", "linix");
#else
 #ifdef __WIN32__
        jo.push("build", "mingw");
 #else
        jo.push("build", "other");
 #endif
#endif
        jo.push("hostname", "localhost");
        jo.push("ipaddr", "127.0.0.1");
        jo.push("platform", SDL_GetPlatform());
        jo.push("ltsm:client", LTSM_VNC2SDL_VERSION);
        jo.push("x11:nodamage", xcbNoDamage);
        jo.push("frame:rate", frameRate);
        jo.push("enc:opts", JsonArrayStream(encodingOptions.begin(), encodingOptions.end()).flush());

        if(username.empty())
        {
            if(auto env = std::getenv("USER"))
            {
                jo.push("username", env);
            }
            else if(auto env = std::getenv("USERNAME"))
            {
                jo.push("username", env);
            }
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
            jo.push("redirect:cups", printerUrl);
        }

        if(! saneUrl.empty())
        {
            Application::info("%s: %s url: %s", __FUNCTION__, "sane", saneUrl.c_str());
            jo.push("redirect:sane", saneUrl);
        }

        if(! shareFolders.empty())
        {
            jo.push("redirect:fuse", JsonArrayStream(shareFolders.begin(), shareFolders.end()).flush());
        }

        if(pcscEnable)
        {
            jo.push("redirect:pcsc", "enable");
        }

#ifdef LTSM_PKCS11_AUTH
        if(! pkcs11Auth.empty())
        {
            jo.push("pkcs11:auth", pkcs11Auth);
        }
#endif

        if(audioEnable)
        {
            std::forward_list<std::string> allowEncoding = { "auto", "pcm" };
#ifdef LTSM_WITH_OPUS
            allowEncoding.emplace_front("opus");
#endif

            if(std::any_of(allowEncoding.begin(), allowEncoding.end(), [&](auto & enc) { return enc == audioEncoding; }))
            {
                jo.push("redirect:audio", audioEncoding);
            }
            else
            {
                Application::warning("%s: unsupported audio: %s", __FUNCTION__, audioEncoding.c_str());
            }
        }

        return jo.flush();
    }

    void Vnc2SDL::systemLoginSuccess(const JsonObject & jo)
    {
        if(jo.getBoolean("action", false))
        {
        }
        else
        {
            auto error = jo.getString("error");
            Application::error("%s: %s failed, error: %s", __FUNCTION__, "login",
                               error.c_str());
        }
    }

    void Vnc2SDL::clientRecvBellEvent(void)
    {
#ifdef __UNIX__
        bell(75);
#endif
    }

    bool Vnc2SDL::createChannelAllow(const Channel::ConnectorType & type, const std::string & content,
                                     const Channel::ConnectorMode & mode) const
    {
        if(type == Channel::ConnectorType::Fuse)
        {
            if(! std::any_of(shareFolders.begin(), shareFolders.end(), [&](auto & val) { return val == content; }))
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "share", content.c_str());
                return false;
            }
        }

        return true;
    }

    void Vnc2SDL::windowResizedEvent(int width, int height)
    {
        windowSize = XCB::Size(width, height);
        sendSetDesktopSize(windowSize);
        sendFrameBufferUpdate(false);
    }
}

#ifdef __WIN32__
int main(int argc, char** argv)
#else
int main(int argc, const char** argv)
#endif
{
    // init network
#ifdef __WIN32__
    WSADATA wsaData;

    if(int ret = WSAStartup(MAKEWORD(2, 2), & wsaData); ret != 0)
    {
        std::cerr << "WSAStartup failed: %d" << std::endl;
        return 1;
    }
#endif

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
#ifdef __WIN32__
            LTSM::Vnc2SDL app(argc, (const char**) argv);
#else
            LTSM::Vnc2SDL app(argc, argv);
#endif
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
            LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            LTSM::Application::info("program: %s", "terminate...");
        }
    }

    SDL_Quit();
    return res;
}
