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
#include <fstream>
#include <iostream>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "librfb_client.h"
#include "ltsm_client.h"

using namespace std::chrono_literals;

namespace LTSM
{
    const auto sanedef = "sock://127.0.0.1:6566";
    const auto librtdef = "/usr/lib64/librtpkcs11ecp.so";
    const auto printdef = "cmd:///usr/bin/lpr";
    const auto krb5def = "TERMSRV@remotehost.name";
    const auto windowTitle = "LTSM_client";
#ifdef __WIN32__
    const auto usercfgdef = "$LOCALAPPDATA\\ltsm\\client.cfg";
#else
    const auto usercfgdef = "$HOME/.config/ltsm/client.cfg";
#endif

    void printHelp(const char* prog)
    {
        auto encodings = RFB::ClientDecoder::supportedEncodings();
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
                  "[--pkcs11-auth [" << librtdef << "]] " <<
#endif
#ifdef LTSM_WITH_PCSC
                  "[--smartcard] " <<
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
                  "    --smartcard (redirect smartcard)" << std::endl <<
#endif
#ifdef LTSM_PKCS11_AUTH
                  "    --pkcs11-auth [" << librtdef << "] (pkcs11 autenfication, and the user's certificate is in the LDAP database)" <<
#endif
                  " ] (audio support)" << std::endl <<
                  "    --load <path> (external params from config)" << std::endl <<
                  "    --save [" << usercfgdef << "](save params to local config)" << std::endl;

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

        std::cout << std::endl << "load priority: " << std::endl <<
#ifndef __WIN32__
                "     - /etc/ltsm/client.cfg" << std::endl <<
#endif
                "     - " << usercfgdef << std::endl <<
                "     - set --param1 --param2" << std::endl <<
                "     - load from ext config --load <path>" << std::endl;

        std::cout << std::endl << "save example: " << std::endl <<
                "     " << prog << " --host 172.17.0.2 --nocaps --geometry 1280x1024 --save" << std::endl;

        std::cout << std::endl;
    }

    template<typename Iter>
    void saveConfig(Iter beg, Iter end, std::filesystem::path file)
    {
        if(! std::filesystem::is_directory(file.parent_path()))
            std::filesystem::create_directories(file.parent_path());

        std::ofstream ofs(file, std::ofstream::out | std::ofstream::trunc);

        if(ofs)
        {
            for(auto it = beg; it != end; ++it)
            {
                ofs << *it;

                if(auto val = std::next(it); val != end && ! startsWith(*val, "--"))
                {
                    ofs << " " << *val;
                    it = val;
                }

                ofs << std::endl;
            }

            std::cout << "save success, to file: " << file << std::endl;
        }
    }

    void hidePasswordArgument(char* pass)
    {
        while(pass && *pass)
        {
            *pass = '*';
            pass++;
        }
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

        auto argBeg = argv + 1;
        auto argEnd = argv + argc;

#ifdef __WIN32__
        if(auto home = getenv("LOCALAPPDATA"))
            loadConfig(Tools::replace(usercfgdef, "$LOCALAPPDATA", home));
#else
        loadConfig(std::filesystem::path("/etc") / "ltsm" / "client.cfg");

        if(auto home = getenv("HOME"))
            loadConfig(Tools::replace(usercfgdef, "$HOME", home));
#endif

        for(auto it = argBeg; it != argEnd; ++it)
        {
            if(auto val = std::next(it); val != argEnd && ! startsWith(*val, "--"))
            {
                parseCommand(*it, *val);

                if(0 == strcmp(*it, "--password"))
                    hidePasswordArgument(const_cast<char*>(*val));

                it = val;
            }
            else
            {
                parseCommand(*it, "");
            }
        }

        if(auto it = std::find_if(argBeg, argEnd, [](std::string_view arg){ return arg == "--load"; }); it != argEnd)
        {
            if(it = std::next(it); it != argEnd)
                loadConfig(*it);
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

    void Vnc2SDL::loadConfig(const std::filesystem::path & config)
    {
        if(! std::filesystem::is_regular_file(config))
            return;
        
        std::ifstream ifs(config);
        while(ifs)
        {
            std::string line;
            std::getline(ifs, line);

            if(line.empty())
                continue;

            if(! startsWith(line, "--"))
                continue;

            auto it1 = line.begin();
            auto it2 = std::find(it1, line.end(), 0x20);
            std::string_view cmd = string2view(it1, it2);

            if(it2 != line.end())
            {
                std::string_view arg = string2view(it2 + 1, line.end());
                Application::info("%s: %.*s %.*s", __FUNCTION__, (int) cmd.size(), cmd.data(), (int) arg.size(), arg.data());
                parseCommand(cmd, arg);
            }
            else
            {
                Application::info("%s: %.*s", __FUNCTION__, (int) cmd.size(), cmd.data());
                parseCommand(cmd, "");
            }
        }
    }

    void Vnc2SDL::parseCommand(std::string_view cmd, std::string_view arg)
    {
        if(cmd == "--nocaps")
        {
            capslockEnable = false;
        }
        else if(cmd == "--noltsm")
        {
            ltsmSupport = false;
        }
        else if(cmd == "--noaccel")
        {
            windowAccel = false;
        }
#ifdef LTSM_WITH_GNUTLS
        else if(cmd == "--notls")
        {
            rfbsec.authVenCrypt = false;
        }
#endif
        else if(cmd == "--noxkb")
        {
            useXkb = false;
        }
        else if(cmd == "--loop")
        {
            alwaysRunning = true;
        }
        else if(cmd == "--fullscreen")
        {
            windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        else if(cmd == "--fixed")
        {
            windowFlags &= ~SDL_WINDOW_RESIZABLE;
        }
        else if(cmd == "--nodamage")
        {
            xcbNoDamage = true;
        }
#ifdef LTSM_WITH_PCSC
        else if(cmd == "--pcsc" || cmd == "--smartcard")
        {
            pcscEnable = true;
        }
#endif
        else if(cmd == "--extclip")
        {
            setExtClipboardLocalCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                            ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);
        }

#ifdef LTSM_DECODING
        else if(cmd == "--qoi")
        {
            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_LTSM_QOI)));
        }
        else if(cmd == "--lz4")
        {
            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_LTSM_LZ4)));
        }
        else if(startsWith(cmd, "--tjpg"))
        {
            if(auto opts = Tools::split(cmd, ','); 1 < opts.size())
            {
                opts.pop_front();
                encodingOptions.assign(opts.begin(), opts.end());
            }

            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_LTSM_TJPG)));
        }
#endif
#ifdef LTSM_DECODING_FFMPEG
 #ifdef LTSM_DECODING_H264
        else if(cmd == "--h264")
        {
            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_FFMPEG_H264)));
        }
 #endif
 #ifdef LTSM_DECODING_AV1
        else if(cmd == "--av1")
        {
            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_FFMPEG_AV1)));
        }
 #endif
 #ifdef LTSM_DECODING_VP8
        else if(cmd == "--vp8")
        {
            prefferedEncoding.assign(Tools::lower(RFB::encodingName(
                    RFB::ENCODING_FFMPEG_VP8)));
        }
 #endif
#endif
        else if(cmd == "--encoding")
        {
            if(arg.size())
            {
                auto opts = Tools::split(arg, ',');

                prefferedEncoding.assign(Tools::lower(opts.front()));

                opts.pop_front();
                encodingOptions.assign(opts.begin(), opts.end());
            }

            auto encodings = ClientDecoder::supportedEncodings(extClipboardLocalCaps());

            if(std::none_of(encodings.begin(), encodings.end(), [&](auto & str) { return Tools::lower(RFB::encodingName(str)) == prefferedEncoding; }))
            {
                Application::warning("%s: incorrect encoding: %s", __FUNCTION__,
                                     prefferedEncoding.c_str());
                prefferedEncoding.clear();
            }
        }

#ifdef LTSM_WITH_GSSAPI
        else if(cmd == "--kerberos")
        {
            rfbsec.authKrb5 = true;
            rfbsec.krb5Service = "TERMSRV";

            if(arg.size())
            {
                rfbsec.krb5Service.assign(arg.begin(), arg.end());
            }
        }

#endif
        else if(cmd == "--audio")
        {
            audioEnable = true;

            if(arg.size())
            {
                audioEncoding.assign(arg.begin(), arg.end());
            }
        }
        else if(cmd == "--printer")
        {
            printerUrl.assign(printdef);

            if(arg.size())
            {
                auto url = Channel::parseUrl(arg);

                if(url.first == Channel::ConnectorType::Unknown)
                {
                    Application::warning("%s: parse %s failed, unknown url: %.*s",
                            __FUNCTION__, "printer", (int) arg.size(), arg.data());
                }
                else
                {
                    printerUrl.assign(arg.begin(), arg.end());
                }
            }
        }
        else if(cmd == "--sane")
        {
            saneUrl.assign(sanedef);

            if(arg.size())
            {
                auto url = Channel::parseUrl(arg);

                if(url.first == Channel::ConnectorType::Unknown)
                {
                    Application::warning("%s: parse %s failed, unknown url: %.*s",
                            __FUNCTION__, "sane", (int) arg.size(), arg.data());
                }
                else
                {
                    saneUrl.assign(arg.begin(), arg.end());
                }
            }
        }
#ifdef LTSM_PKCS11_AUTH
        else if(cmd == "--pkcs11-auth")
        {
            pkcs11Auth.assign(librtdef);

            if(arg.size())
            {
                pkcs11Auth.assign(arg.begin(), arg.end());
            }

            if(! std::filesystem::exists(pkcs11Auth))
            {
                Application::warning("%s: parse %s failed, not exist: %s",
                        __FUNCTION__, "pkcs11-auth", pkcs11Auth.c_str());
                pkcs11Auth.clear();
            }
        }
#endif
        else if(cmd == "--trace")
        {
            Application::setDebugLevel(DebugLevel::Trace);
        }
        else if(cmd == "--debug")
        {
            if(! Application::isDebugLevel(DebugLevel::Trace))
                Application::setDebugLevel(DebugLevel::Debug);

            if(arg.size())
            {
                Application::setDebugTypes(Tools::split(arg, ','));
            }
        }
        else if(cmd == "--syslog")
        {
            if(arg.size())
            {
                Application::setDebugTargetFile(arg);
            }
            else
            {
                Application::setDebugTarget(DebugTarget::Syslog);
            }
        }
        else if(cmd == "--host" && arg.size())
        {
            host.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--seamless" && arg.size())
        {
            seamless.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--share-folder" && arg.size())
        {
            if(std::filesystem::is_directory(arg))
            {
                shareFolders.emplace_front(arg.begin(), arg.end());
            }
            else
            {
                Application::warning("%s: parse %s failed, not exist: %.*s",
                        __FUNCTION__, "share-folder", (int) arg.size(), arg.data());
            }
        }
        else if(cmd == "--password" && arg.size())
        {
            rfbsec.passwdFile.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--password-file" && arg.size())
        {
            passfile.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--username" && arg.size())
        {
            username.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--port" && arg.size())
        {
            try
            {
                port = std::stoi(view2string(arg));
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "incorrect port number" << std::endl;
                port = 5900;
            }
        }
        else if(cmd == "--framerate" && arg.size())
        {
            try
            {
                frameRate = std::stoi(view2string(arg));

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
        }
        else if(cmd == "--geometry" && arg.size())
        {
            size_t idx;

            try
            {
                auto width = std::stoi(view2string(arg), & idx, 0);
                std::string_view arg2 = string2view(arg.begin() + idx + 1, arg.end());
                auto height = std::stoi(view2string(arg2), nullptr, 0);
                primarySize = XCB::Size(width, height);
            }
            catch(const std::invalid_argument &)
            {
                std::cerr << "invalid geometry" << std::endl;
            }
        }
#ifdef LTSM_WITH_GNUTLS
        else if(cmd == "--tls-priority" && arg.size())
        {
            rfbsec.tlsPriority.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--tls-ca-file" && arg.size())
        {
            rfbsec.caFile.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--tls-cert-file" && arg.size())
        {
            rfbsec.certFile.assign(arg.begin(), arg.end());
        }
        else if(cmd == "--tls-key-file" && arg.size())
        {
            rfbsec.keyFile.assign(arg.begin(), arg.end());
        }
#endif
        else if(cmd == "--load")
        {
            // skip exception
        }
        else if(cmd == "--save")
        {
            // skip exception
        }
        else
        {
            throw std::invalid_argument(view2string(cmd));
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

    bool Vnc2SDL::sdlMouseEvent(const SDL::GenericEvent & ev)
    {
        // left 0x01, middle 0x02, right 0x04, scrollUp: 0x08,
        // scrollDn: 0x10, scrollLf: 0x20, scrollRt: 0x40, back: 0x80
        switch(ev.type())
        {
            case SDL_MOUSEMOTION:
                if(auto me = ev.motion())
                {
                    sendPointerEvent(0xFF & me->state, me->x, me->y);
                    return true;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if(auto be = ev.button())
                {
                    sendPointerEvent(ev.type() == SDL_MOUSEBUTTONDOWN ? SDL_BUTTON(be->button) : 0, be->x, be->y);
                    return true;
                }
                break;

            case SDL_MOUSEWHEEL:
                // scroll up
                if(auto we = ev.wheel())
                {
                    if(0 == we->y)
                        return false;

                    int mouseX, mouseY;
                    auto state = SDL_GetMouseState(& mouseX, & mouseY);

                    // press/release up/down
                    sendPointerEvent(SDL_BUTTON(0 < we->y ? SDL_BUTTON_X1 : SDL_BUTTON_X2), mouseX, mouseY);

                    SDL_GetMouseState(& mouseX, & mouseY);
                    sendPointerEvent(0, mouseX, mouseY);
                    return true;
                }
            default:
                break;
        }
        
        return false;
    }

    const char* sdlWindowEventName(uint8_t id)
    {
        switch(id)
        {
            case SDL_WINDOWEVENT_NONE: return "none";
            case SDL_WINDOWEVENT_SHOWN: return "show";
            case SDL_WINDOWEVENT_HIDDEN: return "hidden";
            case SDL_WINDOWEVENT_EXPOSED: return "exposed";
            case SDL_WINDOWEVENT_MOVED: return "moved";
            case SDL_WINDOWEVENT_RESIZED: return "resized";
            case SDL_WINDOWEVENT_SIZE_CHANGED: return "size changed";
            case SDL_WINDOWEVENT_MINIMIZED: return "minimized";
            case SDL_WINDOWEVENT_MAXIMIZED: return "maximized";
            case SDL_WINDOWEVENT_RESTORED: return "restored";
            case SDL_WINDOWEVENT_ENTER: return "enter";
            case SDL_WINDOWEVENT_LEAVE: return "leave";
            case SDL_WINDOWEVENT_FOCUS_GAINED: return "focus gained";
            case SDL_WINDOWEVENT_FOCUS_LOST: return "focus lost";
            case SDL_WINDOWEVENT_CLOSE: return "close";
            case SDL_WINDOWEVENT_TAKE_FOCUS: return "take focus";
            case SDL_WINDOWEVENT_HIT_TEST: return "hit test";

#if SDL_VERSION_ATLEAST(2, 0, 18)
            case SDL_WINDOWEVENT_ICCPROF_CHANGED: return "iccprof changed";
            case SDL_WINDOWEVENT_DISPLAY_CHANGED: return "display changed";
#endif
            default: break;
        }
        return "unknown";
    }

    bool Vnc2SDL::sdlWindowEvent(const SDL::GenericEvent & ev)
    {
        if(auto we = ev.window())
        {
            Application::debug(DebugType::App, "%s: window event: %s", __FUNCTION__, sdlWindowEventName(we->event));

            switch(we->event)
            {
                case SDL_WINDOWEVENT_EXPOSED:
                    window->renderPresent(false);
                    return true;

                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    focusLost = false;
                    return true;

                case SDL_WINDOWEVENT_FOCUS_LOST:
                    focusLost = true;
                    return true;

                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    Application::debug(DebugType::App, "%s: size changed: [%" PRId32 "x%" PRId32 "]",
                            __FUNCTION__, we->data1, we->data2);
                    return true;

                case SDL_WINDOWEVENT_RESIZED:
                    Application::debug(DebugType::App, "%s: event resized: [%" PRId32 "x%" PRId32 "]",
                            __FUNCTION__, we->data1, we->data2);
                    windowResizedEvent(we->data1, we->data2);
                    return true;
                
                default:
                    break;
            }
        }

        return false;
    }

    bool Vnc2SDL::sdlKeyboardEvent(const SDL::GenericEvent & ev)
    {
        if(auto ke = ev.key())
        {
            // pressed
            if(ke->state == SDL_PRESSED)
            {
                Application::debug(DebugType::App, "%s: SDL Keysym - scancode: 0x%08" PRIx32 ", keycode: 0x%08" PRIx32,
                            __FUNCTION__, ke->keysym.scancode, ke->keysym.sym);

                // ctrl + F10 -> fast close
                if(ke->keysym.sym == SDLK_F10 &&
                        (KMOD_CTRL & SDL_GetModState()))
                {
                    Application::warning("%s: hotkey received (%s), %s", __FUNCTION__, "ctrl + F10", "close application");
                    return sdlQuitEvent();
                }

                // ctrl + F11 -> fullscreen toggle
                if(ke->keysym.sym == SDLK_F11 &&
                        (KMOD_CTRL & SDL_GetModState()))
                {
                    Application::warning("%s: hotkey received (%s), %s", __FUNCTION__, "ctrl + F11", "fullscreen toggle");
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
                    return true;
                }
            }
            
            // continue, released
            if(ke->keysym.sym == 0x40000000 && ! capslockEnable)
            {
                auto mod = SDL_GetModState();
                SDL_SetModState(static_cast<SDL_Keymod>(mod & ~KMOD_CAPS));
                Application::notice("%s: CAPS reset", __FUNCTION__);
                return true;
            }

            if(20250808 <= remoteLtsmVersion())
            {
                sendSystemKeyboardEvent(ev.type() == SDL_KEYDOWN, ke->keysym.scancode, ke->keysym.sym);
                return true;
            }

#ifdef __UNIX__
            int xksym = SDL::Window::convertScanCodeToKeySym(ke->keysym.scancode);

            if(xksym == 0)
            {
                xksym = ke->keysym.sym;
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
            int xksym = ke->keysym.sym;
#endif
            sendKeyEvent(ke->state == SDL_PRESSED, xksym);
            return true;
        }
        
        return false;
    }

    enum LocalEvent { Resize = 776, ResizeCont = 777 };

    bool Vnc2SDL::sdlUserEvent(const SDL::GenericEvent & ev)
    {
        if(auto ue = ev.user())
        {
            // resize event
            if(ue->code == LocalEvent::Resize ||
                    ue->code == LocalEvent::ResizeCont)
            {
                auto width = (size_t) ue->data1;
                auto height = (size_t) ue->data2;
                bool contUpdateResume = ue->code == LocalEvent::ResizeCont;

                cursors.clear();

                if(windowFullScreen())
                {
                    window = std::make_unique<SDL::Window>(windowTitle, width, height, 0, 0, windowFlags, windowAccel);
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

                return true;
            }
        }
        
        return false;
    }

    bool Vnc2SDL::sdlDropFileEvent(const SDL::GenericEvent & ev)
    {
        if(auto de = ev.drop())
        {
            std::unique_ptr<char, void(*)(void*)> drop{ de->file, SDL_free };
            dropFiles.emplace_front(drop.get());
            dropStart = std::chrono::steady_clock::now();
            return true;
        }

        return false;
    }

    bool Vnc2SDL::sdlQuitEvent(void)
    {
        RFB::ClientDecoder::rfbMessagesShutdown();
        return true;
    }

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
            case SDL_MOUSEWHEEL:
                return sdlMouseEvent(ev);

            case SDL_WINDOWEVENT:
                return sdlWindowEvent(ev);

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                return sdlKeyboardEvent(ev);

            case SDL_DROPFILE:
                return sdlDropFileEvent(ev);

            case SDL_USEREVENT:
                return sdlUserEvent(ev);

            case SDL_QUIT:
                return sdlQuitEvent();
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
            else
            {
            // server runtime
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
        if(status == 1)
        {
            // 3. server reply
            if(! nsz.isEmpty())
                pushEventWindowResize(nsz);

            if(err)
            {
                Application::error("%s: status: %d, error code: %d", __FUNCTION__, status, err);

                //if(! nsz.isEmpty())
                //    primarySize.reset();
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
            window = std::make_unique<SDL::Window>(windowTitle, wsz.width, wsz.height, 0, 0, windowFlags, windowAccel);
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

    void Vnc2SDL::updateRawPixels(const XCB::Region & wrt, const void* data,
                                  uint32_t pitch, const PixelFormat & pf)
    {
        uint32_t sdlFormat = SDL_MasksToPixelFormatEnum(pf.bitsPerPixel(), pf.rmask(), pf.gmask(), pf.bmask(), pf.amask());

        if(sdlFormat != SDL_PIXELFORMAT_UNKNOWN)
        {
            return updateRawPixels2(wrt, data, pf.bitsPerPixel(), pitch, sdlFormat);
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

        updateRawPixels3(wrt, sfframe.get());
    }

    void Vnc2SDL::updateRawPixels2(const XCB::Region & wrt, const void* data, uint8_t depth, uint32_t pitch, uint32_t sdlFormat)
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

        updateRawPixels3(wrt, sfframe.get());
    }

    void Vnc2SDL::updateRawPixels3(const XCB::Region & wrt, SDL_Surface* sfframe)
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
            if(pixels.empty())
            {
                Application::error("%s: cursor not found, id: 0x%08" PRIx32, __FUNCTION__, cursorId);
                sendSystemCursorFailed(cursorId);
                return;
            }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            auto cursorFmt = BGRA32;
#else
            auto cursorFmt = ARGB32;
#endif
            auto sdlFormat = SDL_MasksToPixelFormatEnum(cursorFmt.bitsPerPixel(),
                             cursorFmt.rmask(), cursorFmt.gmask(), cursorFmt.bmask(), cursorFmt.amask());

            if(pixels.size() < static_cast<size_t>(reg.width) * reg.height * 4)
            {
                Application::error("%s: invalid pixels, length: %lu, id: 0x%08" PRIx32, __FUNCTION__, pixels.size(), cursorId);
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

                Application::warning("%s: send cursor failed, id: 0x%08" PRIx32, __FUNCTION__, cursorId);
                sendSystemCursorFailed(cursorId);
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void Vnc2SDL::clientRecvLtsmHandshakeEvent(int flags)
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
            if(! primarySize.isEmpty() && primarySize != windowSize)
            {
                sendSetDesktopSize(primarySize);
            }
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

using namespace LTSM;

#ifdef __WIN32__
int main(int argc, char** argv)
#else
int main(int argc, const char** argv)
#endif
{
#ifdef __WIN32__
    auto localcfg = Tools::replace(usercfgdef, "$LOCALAPPDATA", getenv("LOCALAPPDATA"));
#else
    auto localcfg = Tools::replace(usercfgdef, "$HOME", getenv("HOME"));
#endif

    auto argBeg = argv + 1;
    auto argEnd = argv + argc;

    if((argBeg == argEnd && ! std::filesystem::is_regular_file(localcfg)) ||
        std::any_of(argBeg, argEnd, [](std::string_view arg){ return arg == "--help" || arg == "-h"; }))
    {
        printHelp(argv[0]);
        return 0;
    }

    if(auto it = std::find_if(argBeg, argEnd, [](std::string_view arg){ return arg == "--save"; }); it != argEnd)
    {
        std::string_view path = localcfg;

        if(auto it2 = std::next(it); it2 != argEnd)
        {
            std::string_view path2 = *it2;
            if(! startsWith(path2, "--"))
                path = path2;
        }

        saveConfig(argBeg, it, path);
        return 0;
    }

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
            Vnc2SDL app(argc, (const char**) argv);
#else
            Vnc2SDL app(argc, argv);
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
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            Application::info("program: %s", "terminate...");
        }
    }

    SDL_Quit();
    return res;
}
