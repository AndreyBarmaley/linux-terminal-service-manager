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
using namespace boost;

namespace LTSM {
    const auto sanedef = "sock://127.0.0.1:6566";
    const auto librtdef = "/usr/lib64/librtpkcs11ecp.so";
    const auto printdef = "cmd:///usr/bin/lpr";
    const auto krb5def = "TERMSRV@remotehost.name";
    const auto windowTitle = "LTSM Client";
#ifdef __WIN32__
    const auto usercfgdef = "$LOCALAPPDATA\\ltsm\\client.cfg";
#else
    const auto usercfgdef = "$HOME/.config/ltsm/client.cfg";
#endif

    void printHelp(const char* prog) {
        std::list<std::string> videoEncodings;
        std::list<std::string> audioEncodings;
        for(const auto & enc : RFB::ClientDecoder::supportedEncodings()) {
            if(RFB::isVideoEncoding(enc)) {
                videoEncodings.emplace_back(Tools::lower(RFB::encodingName(enc)));
            }
            if(RFB::isAudioEncoding(enc)) {
                audioEncodings.emplace_back(Tools::lower(RFB::encodingName(enc)));
            }
        }
        std::cout << std::endl <<
                  prog << " version: " << LTSM_CLIENT_VERSION << std::endl;
        std::cout << std::endl <<
        "usage: " << prog <<
        ": --host <localhost> [--port 5900] [--password <pass>] [password-file <file>] " <<
        "[--version] [--debug [<types>]] [--trace] [--syslog [<tofile>]] " <<
        "[--noltsm] [--noaccel] [--fullscreen] [--geometry <WIDTHxHEIGHT>] [--resize] " <<
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
        "[--video <enc>] " <<
#ifdef LTSM_WITH_AUDIO
        "[--audio [<enc>]] " <<
#endif
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
                                  "    --fps <fps>" << std::endl <<
                                  "    --geometry <WIDTHxHEIGHT> (set window geometry)" << std::endl <<
                                  "    --dpi <DPI> (set X11 dpi)" << std::endl <<
                                  "    --resize (allow resizable window)" << std::endl <<
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
                                  "    --qoi (the same as --video ltsm_qoi)" << std::endl <<
#endif
#ifdef LTSM_DECODING_LZ4
                                  "    --lz4 (the same as --video ltsm_lz4)" << std::endl <<
#endif
#ifdef LTSM_DECODING_TJPG
                                  "    --tjpg (the same as --video ltsm_tjpg)" << std::endl <<
#endif
#endif
#ifdef LTSM_DECODING_FFMPEG
#ifdef LTSM_DECODING_H264
                                  "    --h264 (the same as --video ffmpeg_h264)" << std::endl <<
#endif
#ifdef LTSM_DECODING_AV1
                                  "    --av1 (the same as --video ffmpeg_av1)" << std::endl <<
#endif
#ifdef LTSM_DECODING_VP8
                                  "    --vp8 (the same as --video ffmpeg_vp8)" << std::endl <<
#endif
#endif
                                  "    --video <enc> (set preffered encoding: " << Tools::join(videoEncodings, ",") << ")" << std::endl <<
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
                                  "    --loop (always reconnecting)" << std::endl;

#ifdef LTSM_WITH_AUDIO
        if(! audioEncodings.empty()) {
            std::cout <<
                                  "    --audio [<enc>] (audio support, and preffered encoding: " << Tools::join(audioEncodings, ",") << ")" << std::endl;
        }
#endif
        std::cout <<
                                  "    --printer [" << printdef << "] (redirect printer)" << std::endl <<
                                  "    --sane [" << sanedef << "] (redirect scanner)" << std::endl <<
#ifdef LTSM_WITH_PCSC
                                  "    --smartcard (redirect smartcard)" << std::endl <<
#endif
#ifdef LTSM_PKCS11_AUTH
                                  "    --pkcs11-auth [" << librtdef << "] (pkcs11 autenfication, and the user's certificate is in the LDAP database)" << std::endl <<
#endif
                                  "    --load <path> (external params from config)" << std::endl <<
                                  "    --save [" << usercfgdef << "](save params to local config)" << std::endl;

        if(! audioEncodings.empty()) {
            std::cout << std::endl << "supported audio encodings: " << std::endl <<
                                  "    [" << audioEncodings.front() << "] " <<
                                  Tools::rangeJoin(std::next(audioEncodings.begin()), audioEncodings.end(), " ") << std::endl;
        }

        if(! videoEncodings.empty()) {
            std::cout << std::endl << "supported video encodings: " << std::endl <<
                                  "    [" << videoEncodings.front() << "] " <<
                                  Tools::rangeJoin(std::next(videoEncodings.begin()), videoEncodings.end(), " ") << std::endl;
        }

        std::cout << std::endl << "encoding options: " << std::endl;

        for(const auto & enc : RFB::ClientDecoder::supportedEncodings()) {
            if(auto opts = RFB::encodingOpts(enc); ! opts.empty()) {
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
    void saveConfig(Iter beg, Iter end, std::filesystem::path file) {
        if(! std::filesystem::is_directory(file.parent_path())) {
            std::filesystem::create_directories(file.parent_path());
        }

        std::ofstream ofs(file, std::ofstream::out | std::ofstream::trunc);

        if(ofs) {
            for(auto it = beg; it != end; ++it) {
                ofs << *it;

                if(auto val = std::next(it); val != end && ! startsWith(*val, "--")) {
                    ofs << " " << *val;
                    it = val;
                }

                ofs << std::endl;
            }

            std::cout << "save success, to file: " << file << std::endl;
        }
    }

    void hidePasswordArgument(char* pass) {
        while(pass && *pass) {
            *pass = '*';
            pass++;
        }
    }

    ClientApp::ClientApp(int argc, const char** argv)
        : Application("ltsm_client")
#ifdef LTSM_WITH_X11
        , RFB::X11Client(ioc())
#else
        , RFB::WinClient(ioc())
#endif
        , signals_{ioc()}
        , rfb_strand_{ioc().get_executor()}
        , sdl_strand_{ioc().get_executor()}
#ifdef __UNIX__
        , x11_strand_{ioc().get_executor()}
#endif
        {

        Application::setDebugTarget(DebugTarget::Console);
        Application::setDebugLevel(DebugLevel::Info);
#ifdef LTSM_WITH_GNUTLS
        rfbsec.authVenCrypt = true;
        rfbsec.tlsDebug = 2;
#else
        rfbsec.authVenCrypt = false;
#endif

        auto argBeg = argv + 1;
        auto argEnd = argv + argc;

#ifdef __WIN32__

        if(auto home = getenv("LOCALAPPDATA")) {
            loadConfig(Tools::replace(usercfgdef, "$LOCALAPPDATA", home));
        }

#else
        loadConfig(std::filesystem::path("/etc") / "ltsm" / "client.cfg");

        if(auto home = getenv("HOME")) {
            loadConfig(Tools::replace(usercfgdef, "$HOME", home));
        }

#endif

        for(auto it = argBeg; it != argEnd; ++it) {
            if(auto val = std::next(it); val != argEnd && ! startsWith(*val, "--")) {
                parseCommand(*it, *val);

                if(0 == strcmp(*it, "--password")) {
                    hidePasswordArgument(const_cast<char*>(*val));
                }

                it = val;
            } else {
                parseCommand(*it, "");
            }
        }

        if(auto it = std::ranges::find_if(argBeg, argEnd, [](std::string_view arg) {
            return arg == "--load"; }); it != argEnd) {
            if(it = std::next(it); it != argEnd) {
                loadConfig(*it);
            }
        }

        if(0 == videoEncoding) {
#ifdef LTSM_WITH_QOI
            videoEncoding = RFB::ENCODING_LTSM_QOI;
#endif
        }

        if(0 == audioEncoding) {
            audioEncoding = RFB::ENCODING_LTSM_PCM;
#ifdef LTSM_WITH_OPUS
            audioEncoding = RFB::ENCODING_LTSM_OPUS;
#endif
        }

        updateSecurity();    

        // fullscreen
        if(windowFullScreen()) {
            SDL_DisplayMode mode;

            if(0 == SDL_GetDisplayMode(0, 0, & mode)) {
                primarySize = XCB::Size(mode.w, mode.h);

                if(primarySize.width < primarySize.height) {
                    std::swap(primarySize.width, primarySize.height);
                }
            }
        }

        if(frameRate == 0) {
            switch(videoEncoding) {
                case RFB::ENCODING_LTSM_H264:
                case RFB::ENCODING_LTSM_AV1:
                case RFB::ENCODING_LTSM_VP8:
                    // set default
                    frameRate = 16;
                    break;

                default:
                    break;
            }
        }

        appStart = std::chrono::steady_clock::now();
    }

    void ClientApp::loadConfig(const std::filesystem::path & config) {
        if(! std::filesystem::is_regular_file(config)) {
            return;
        }

        std::ifstream ifs(config);

        while(ifs) {
            std::string line;
            std::getline(ifs, line);

            if(line.empty()) {
                continue;
            }

            if(! startsWith(line, "--")) {
                continue;
            }

            auto it1 = line.begin();
            auto it2 = std::ranges::find(it1, line.end(), 0x20);
            std::string_view cmd = string2view(it1, it2);

            if(it2 != line.end()) {
                std::string_view arg = string2view(it2 + 1, line.end());
                Application::info("{}: {} {}", NS_FuncNameV, cmd, arg);
                parseCommand(cmd, arg);
            } else {
                Application::info("{}: {}", NS_FuncNameV, cmd);
                parseCommand(cmd, "");
            }
        }
    }

    void ClientApp::parseCommand(std::string_view cmd, std::string_view arg) {
        if(cmd == "--nocaps") {
            capslockEnable = false;
        } else if(cmd == "--noltsm") {
            ltsmSupport = false;
        } else if(cmd == "--noaccel") {
            windowAccel = false;
        }

#ifdef LTSM_WITH_GNUTLS
        else if(cmd == "--notls") {
            rfbsec.authVenCrypt = false;
        }

#endif
        else if(cmd == "--noxkb") {
            useXkb = false;
        } else if(cmd == "--loop") {
            alwaysRunning = true;
        } else if(cmd == "--fullscreen") {
            windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        } else if(cmd == "--resize") {
            windowFlags |= SDL_WINDOW_RESIZABLE;
        } else if(cmd == "--nodamage") {
            xcbNoDamage = true;
        }

#ifdef LTSM_WITH_PCSC
        else if(cmd == "--pcsc" || cmd == "--smartcard") {
            pcscEnable = true;
        }

#endif
        else if(cmd == "--extclip") {
            setExtClipboardLocalCaps(ExtClipCaps::TypeText | ExtClipCaps::TypeRtf | ExtClipCaps::TypeHtml |
                                     ExtClipCaps::OpRequest | ExtClipCaps::OpNotify | ExtClipCaps::OpProvide);
        }

#ifdef LTSM_DECODING
        else if(cmd == "--qoi") {
            videoEncoding = RFB::ENCODING_LTSM_QOI;
        } else if(cmd == "--lz4") {
            videoEncoding = RFB::ENCODING_LTSM_LZ4;
        } else if(startsWith(cmd, "--tjpg")) {
            if(auto opts = Tools::split(cmd, ','); 1 < opts.size()) {
                opts.pop_front();
                videoEncodingOptions.assign(opts.begin(), opts.end());
            }
            videoEncoding = RFB::ENCODING_LTSM_TJPG;
        }

#endif
#ifdef LTSM_DECODING_FFMPEG
#ifdef LTSM_DECODING_H264
        else if(cmd == "--h264") {
            videoEncoding = RFB::ENCODING_LTSM_H264;
        }

#endif
#ifdef LTSM_DECODING_AV1
        else if(cmd == "--av1") {
            videoEncoding = RFB::ENCODING_LTSM_AV1;
        }

#endif
#ifdef LTSM_DECODING_VP8
        else if(cmd == "--vp8") {
            videoEncoding = RFB::ENCODING_LTSM_VP8;
        }

#endif
#endif
        else if(cmd == "--video") {
            if(arg.size()) {
                auto opts = Tools::split(arg, ',');
                auto val = opts.front();
                videoEncoding = RFB::encodingType(val);
                opts.pop_front();
                videoEncodingOptions.assign(opts.begin(), opts.end());

                if(std::ranges::none_of(ClientDecoder::supportedEncodings(),
                    [&](auto & enc) { return RFB::isVideoEncoding(enc) && enc == videoEncoding; })) {
                    Application::warning("{}: incorrect video encoding: {}", NS_FuncNameV, val);
                    videoEncoding = 0;
                }
            }
        }

#ifdef LTSM_WITH_GSSAPI
        else if(cmd == "--kerberos") {
            rfbsec.authKrb5 = true;
            rfbsec.krb5Service = "TERMSRV";

            if(arg.size()) {
                rfbsec.krb5Service.assign(arg.begin(), arg.end());
            }
        }

#endif
#ifdef LTSM_WITH_AUDIO
        else if(cmd == "--audio") {
            audioEnable = true;

            if(arg.size()) {
                auto opts = Tools::split(arg, ',');
                auto val = opts.front();
                audioEncoding = RFB::encodingType(val);
                opts.pop_front();
                audioEncodingOptions.assign(opts.begin(), opts.end());

                if(std::ranges::none_of(ClientDecoder::supportedEncodings(),
                    [&](auto & enc) { return RFB::isAudioEncoding(enc) && enc == audioEncoding; })) {
                    Application::warning("{}: incorrect audio encoding: {}", NS_FuncNameV, val);
                    audioEncoding = 0;
                    audioEnable = false;
                }
            }
        }
#endif
        else if(cmd == "--printer") {
            printerUrl.assign(printdef);

            if(arg.size()) {
                auto url = Channel::parseUrl(arg);

                if(url.first == Channel::ConnectorType::Unknown) {
                    Application::warning("{}: parse {} failed, unknown url: {}",
                                         NS_FuncNameV, "printer", arg);
                } else {
                    printerUrl.assign(arg.begin(), arg.end());
                }
            }
        } else if(cmd == "--sane") {
            saneUrl.assign(sanedef);

            if(arg.size()) {
                auto url = Channel::parseUrl(arg);

                if(url.first == Channel::ConnectorType::Unknown) {
                    Application::warning("{}: parse {} failed, unknown url: {}",
                                         NS_FuncNameV, "sane", arg);
                } else {
                    saneUrl.assign(arg.begin(), arg.end());
                }
            }
        }

#ifdef LTSM_PKCS11_AUTH
        else if(cmd == "--pkcs11-auth") {
            pkcs11Auth.assign(librtdef);

            if(arg.size()) {
                pkcs11Auth.assign(arg.begin(), arg.end());
            }

            if(! std::filesystem::exists(pkcs11Auth)) {
                Application::warning("{}: parse {} failed, not exist: {}",
                                     NS_FuncNameV, "pkcs11-auth", pkcs11Auth);
                pkcs11Auth.clear();
            }
        }

#endif
        else if(cmd == "--trace") {
            Application::setDebugLevel(DebugLevel::Trace);
        } else if(cmd == "--debug") {
            if(! Application::isDebugLevel(DebugLevel::Trace)) {
                Application::setDebugLevel(DebugLevel::Debug);
            }

            if(arg.size()) {
                Application::setDebugTypes(Tools::split(arg, ','));
            }
        } else if(cmd == "--syslog") {
            if(arg.size()) {
                Application::setDebugTarget(DebugTarget::SyslogFile, arg);
            } else {
                Application::setDebugTarget(DebugTarget::Syslog);
            }
        } else if(cmd == "--host" && arg.size()) {
            host.assign(arg.begin(), arg.end());
        } else if(cmd == "--seamless" && arg.size()) {
            seamless.assign(arg.begin(), arg.end());
        } else if(cmd == "--share-folder" && arg.size()) {
            if(std::filesystem::is_directory(arg)) {
                shareFolders.emplace_front(arg.begin(), arg.end());
            } else {
                Application::warning("{}: parse {} failed, not exist: {}",
                                     NS_FuncNameV, "share-folder", arg);
            }
        } else if(cmd == "--password" && arg.size()) {
            rfbsec.passwdFile.assign(arg.begin(), arg.end());
        } else if(cmd == "--password-file" && arg.size()) {
            passfile.assign(arg.begin(), arg.end());
        } else if(cmd == "--username" && arg.size()) {
            username.assign(arg.begin(), arg.end());
        } else if(cmd == "--port" && arg.size()) {
            try {
                port = std::stoi(view2string(arg));
            } catch(const std::invalid_argument &) {
                std::cerr << "invalid port" << std::endl;
                port = 5900;
            }
            if(0 > port || 0xFFFF < port) {
                Application::warning("{}: invalid port: {}", NS_FuncNameV, port);
                port = 5900;
            }

        } else if(cmd == "--fps" && arg.size()) {
            try {
                frameRate = std::stoi(view2string(arg));
            } catch(const std::invalid_argument &) {
                std::cerr << "invalid framerate" << std::endl;
                frameRate = 16;
            }
            if(5 > frameRate || 25 < frameRate) {
                Application::warning("{}: invalid framerate: {}", NS_FuncNameV, frameRate);
                frameRate = 16;
            }
        } else if(cmd == "--dpi" && arg.size()) {
            try {
                xcbDpi = std::stoi(view2string(arg));
            } catch(const std::invalid_argument &) {
                std::cerr << "invalid dpi" << std::endl;
                xcbDpi = 0;
            }
            if(50 > xcbDpi || 250 < xcbDpi) {
                Application::warning("{}: invalid dpi: {}", NS_FuncNameV, xcbDpi);
                xcbDpi = 0;
            }
        } else if(cmd == "--geometry" && arg.size()) {
            size_t idx;

            try {
                auto width = std::stoi(view2string(arg), & idx, 0);
                std::string_view arg2 = string2view(arg.begin() + idx + 1, arg.end());
                auto height = std::stoi(view2string(arg2), nullptr, 0);
                primarySize = XCB::Size(width, height);
            } catch(const std::invalid_argument &) {
                std::cerr << "invalid geometry" << std::endl;
            }
            if(320 > primarySize.width || 0xFFFF < primarySize.width ||
                240 > primarySize.height || 0xFFFF < primarySize.height) {
                Application::warning("{}: invalid geometry: {}x{}", NS_FuncNameV, primarySize.width, primarySize.height);
                primarySize.reset();
            }
        }

#ifdef LTSM_WITH_GNUTLS
        else if(cmd == "--tls-priority" && arg.size()) {
            rfbsec.tlsPriority.assign(arg.begin(), arg.end());
        } else if(cmd == "--tls-ca-file" && arg.size()) {
            rfbsec.caFile.assign(arg.begin(), arg.end());
        } else if(cmd == "--tls-cert-file" && arg.size()) {
            rfbsec.certFile.assign(arg.begin(), arg.end());
        } else if(cmd == "--tls-key-file" && arg.size()) {
            rfbsec.keyFile.assign(arg.begin(), arg.end());
        }

#endif
        else if(cmd == "--load") {
            // skip exception
        } else if(cmd == "--save") {
            // skip exception
        } else {
            throw std::invalid_argument(view2string(cmd));
        }
    }

    bool ClientApp::windowFullScreen(void) const {
        return windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    bool ClientApp::windowResizable(void) const {
        return windowFlags & SDL_WINDOW_RESIZABLE;
    }

    bool ClientApp::isAlwaysRunning(void) const {
        return alwaysRunning;
    }

    const char* ClientApp::pkcs11Library(void) const {
#ifdef LTSM_PKCS11_AUTH
        return pkcs11Auth.c_str();
#else
        return nullptr;
#endif
    }

    void ClientApp::updateSecurity(void) {
        if(pkcs11Auth.size() && rfbsec.passwdFile.size() && username.size()) {
            pkcs11Auth.clear();
        }

        if(rfbsec.passwdFile.empty()) {
            if(auto env = std::getenv("LTSM_PASSWORD")) {
                rfbsec.passwdFile.assign(env);
            }

            if(passfile == "-" || Tools::lower(passfile) == "stdin") {
                std::getline(std::cin, rfbsec.passwdFile);
            } else if(std::filesystem::is_regular_file(passfile)) {
                std::ifstream ifs(passfile);

                if(ifs) {
                    std::getline(ifs, rfbsec.passwdFile);
                }
            }
        }

        rfbsec.authVnc = ! rfbsec.passwdFile.empty();
        rfbsec.tlsAnonMode = rfbsec.keyFile.empty();

        if(rfbsec.authKrb5 && rfbsec.krb5Service.empty()) {
            Application::warning("{}: kerberos remote service empty", NS_FuncNameV);
            rfbsec.authKrb5 = false;
        }

        if(rfbsec.authKrb5 && rfbsec.krb5Name.empty()) {
            if(username.empty()) {
                if(auto env = std::getenv("USER")) {
                    rfbsec.krb5Name.assign(env);
                } else if(auto env = std::getenv("USERNAME")) {
                    rfbsec.krb5Name.assign(env);
                }
            } else {
                rfbsec.krb5Name.assign(username);
            }
        }

        if(rfbsec.authKrb5) {
            // fixed service format: SERVICE@hostname
            if(std::string::npos == rfbsec.krb5Service.find("@")) {
                rfbsec.krb5Service.append("@").append(host);
            }

            Application::debug(DebugType::Gss, "{}: used kerberos, remote service: {}, local name: {}",
                                NS_FuncNameV, rfbsec.krb5Service, rfbsec.krb5Name);
        }
    }

    asio::awaitable<void> ClientApp::signalsHandler(void) {
        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        try {
            for(;;) {
                int signal = co_await signals_.async_wait(asio::use_awaitable);
                if(signal == SIGTERM || signal == SIGINT) {
                    asio::post(ioc(), std::bind(&ClientApp::stop, this));
                    co_return;
                }
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

    asio::awaitable<void> ClientApp::sdlEventsLoop(void) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer{ex};

        try {
            for(;;) {
                bool events = co_await sdlEventProcessing();

                if(events) {
                    std::this_thread::yield();
                    continue;
                }

                timer.expires_after(30ms);
                co_await timer.async_wait(asio::use_awaitable);
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
            asio::post(ioc(), std::bind(&ClientApp::stop, this));
        }
    }

    void ClientApp::sdlRenderFrame(void) const {
        window->renderPresent(true);
    }

    void ClientApp::stop(void) {
        this->rfbMessagesShutdown();

        rfb_cancel_.emit(asio::cancellation_type::terminal);
        sdl_cancel_.emit(asio::cancellation_type::terminal);
#ifdef __UNIX__
        x11_cancel_.emit(asio::cancellation_type::terminal);
#endif
        signals_.cancel();
    }

#ifdef __UNIX__
    asio::awaitable<void> ClientApp::x11EventsLoop(void) {
        auto ex = co_await asio::this_coro::executor;
        asio::posix::stream_descriptor sd{ex, XCB::RootDisplay::getFd()};

        try {
            for(;;) {
                if(auto err = XCB::RootDisplay::hasError()) {
                    Application::error("{}: xcb error, code: {}", NS_FuncNameV, err);
                    throw system::system_error(asio::error::operation_aborted);
                }

                co_await sd.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);

                while(auto ev = XCB::RootDisplay::pollEvent()) {
                    if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB))) {
                        uint16_t opcode = 0;
                        if(extXkb->isEventError(ev, & opcode)) {
                            Application::warning("{}: {} error: {:#06x}", NS_FuncNameV, "xkb", opcode);
                        }
                    }
                }
            }
        } catch(const system::system_error& err) {
            if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
            asio::post(ioc(), std::bind(&ClientApp::stop, this));
        }

        sd.release();
    }
#endif

    int ClientApp::start(void) {
        if(! ClientDecoder::socketConnect(host, port)) {
            return -1;
        }

        // connected
        if(! rfbHandshake(rfbsec)) {
            return -1;
        }

        asio::co_spawn(ioc(), signalsHandler(), asio::detached);

        asio::co_spawn(rfb_strand_, [this]() -> asio::awaitable<void> {
            try {
                co_await this->rfbMessagesLoopAwait();
            } catch(const system::system_error& err) {
                if(auto ec = err.code(); ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}", "rfbMessagesLoopAwait", ec.message(), ec.value());
                }
            }
            co_return;
        }, asio::bind_cancellation_slot(rfb_cancel_.slot(), asio::detached));

#ifdef __UNIX__
        asio::co_spawn(x11_strand_, x11EventsLoop(),
                asio::bind_cancellation_slot(x11_cancel_.slot(), asio::detached));
#endif

        // asio::thread_pool thread_pool{concurency_};
        ioc().run();

        return EXIT_SUCCESS;
    }

    asio::awaitable<void> ClientApp::sdlMouseMotion(SDL_Event && ev) {
        const auto & me = ev.motion;
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        sendPointerEvent(0xFF & me.state, me.x, me.y);
        co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
    }

    asio::awaitable<void> ClientApp::sdlMouseButton(SDL_Event && ev) {
        const auto & be = ev.button;
        const uint8_t buttons = ev.type == SDL_MOUSEBUTTONDOWN ? SDL_BUTTON(be.button) : 0;
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        sendPointerEvent(buttons, be.x, be.y);
        co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
    }

    asio::awaitable<void> ClientApp::sdlMouseWheel(SDL_Event && ev) {
        const auto & we = ev.wheel;

        if(0 == we.y) {
            co_return;
        }

        int mouseX, mouseY;
        [[maybe_unused]] auto state = SDL_GetMouseState(& mouseX, & mouseY);

        // press/release up/down
        const uint8_t buttons = SDL_BUTTON(0 < we.y ? SDL_BUTTON_X1 : SDL_BUTTON_X2);
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        sendPointerEvent(buttons, mouseX, mouseY);
        sendPointerEvent(0, mouseX, mouseY);
        co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
    }

    const char* sdlWindowEventName(uint8_t id) {
        switch(id) {
            case SDL_WINDOWEVENT_NONE:
                return "none";

            case SDL_WINDOWEVENT_SHOWN:
                return "show";

            case SDL_WINDOWEVENT_HIDDEN:
                return "hidden";

            case SDL_WINDOWEVENT_EXPOSED:
                return "exposed";

            case SDL_WINDOWEVENT_MOVED:
                return "moved";

            case SDL_WINDOWEVENT_RESIZED:
                return "resized";

            case SDL_WINDOWEVENT_SIZE_CHANGED:
                return "size changed";

            case SDL_WINDOWEVENT_MINIMIZED:
                return "minimized";

            case SDL_WINDOWEVENT_MAXIMIZED:
                return "maximized";

            case SDL_WINDOWEVENT_RESTORED:
                return "restored";

            case SDL_WINDOWEVENT_ENTER:
                return "enter";

            case SDL_WINDOWEVENT_LEAVE:
                return "leave";

            case SDL_WINDOWEVENT_FOCUS_GAINED:
                return "focus gained";

            case SDL_WINDOWEVENT_FOCUS_LOST:
                return "focus lost";

            case SDL_WINDOWEVENT_CLOSE:
                return "close";

            case SDL_WINDOWEVENT_TAKE_FOCUS:
                return "take focus";

            case SDL_WINDOWEVENT_HIT_TEST:
                return "hit test";

#if SDL_VERSION_ATLEAST(2, 0, 18)

            case SDL_WINDOWEVENT_ICCPROF_CHANGED:
                return "iccprof changed";

            case SDL_WINDOWEVENT_DISPLAY_CHANGED:
                return "display changed";
#endif

            default:
                break;
        }

        return "unknown";
    }

    asio::awaitable<void> ClientApp::windowResizedEvent(const XCB::Size & wsz) {
        auto time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - appStart);
        // skip: starting window resized
        if(time.count() > 3) {
            windowSize = wsz;
            co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
            sendSetDesktopSize(wsz);
            sendFrameBufferUpdate(false);
            co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
        }
    }

    asio::awaitable<void> ClientApp::sdlWindowEvent(SDL_Event && ev) {
        const auto & we = ev.window;
        Application::debug(DebugType::App, "{}: window event: {}", NS_FuncNameV, sdlWindowEventName(we.event));

        switch(we.event) {
            case SDL_WINDOWEVENT_EXPOSED:
                window->renderPresent(false);
                break;

            case SDL_WINDOWEVENT_FOCUS_GAINED:
                focusLost = false;
                break;

            case SDL_WINDOWEVENT_FOCUS_LOST:
                focusLost = true;
                break;

            case SDL_WINDOWEVENT_SIZE_CHANGED:
                Application::debug(DebugType::App, "{}: size changed: [{}, {}]",
                                   NS_FuncNameV, we.data1, we.data2);
                break;

            case SDL_WINDOWEVENT_RESIZED:
                Application::debug(DebugType::App, "{}: event resized: [{}, {}]",
                                   NS_FuncNameV, we.data1, we.data2);
                co_await windowResizedEvent(XCB::Size(we.data1, we.data2));
                break;

            default:
                break;
        }
    }

    asio::awaitable<void> ClientApp::sdlKeyboardEvent(SDL_Event && ev) {
        const auto & ke = ev.key;

        // pressed
        if(ke.state == SDL_PRESSED) {
            Application::debug(DebugType::App, "{}: SDL Keysym - scancode: {:#010x}, keycode: {:#010x}",
                               NS_FuncNameV, static_cast<int>(ke.keysym.scancode), ke.keysym.sym);

            // ctrl + F10 -> fast close
            if(ke.keysym.sym == SDLK_F10 &&
               (KMOD_CTRL & SDL_GetModState())) {
                Application::warning("{}: hotkey received ({}), {}", NS_FuncNameV, "ctrl + F10", "close application");
                asio::post(ioc(), std::bind(&ClientApp::stop, this));
                co_return;
            }

            // ctrl + F11 -> fullscreen toggle
            if(ke.keysym.sym == SDLK_F11 &&
               (KMOD_CTRL & SDL_GetModState())) {
                Application::warning("{}: hotkey received ({}), {}", NS_FuncNameV, "ctrl + F11", "fullscreen toggle");

                if(windowFullScreen()) {
                    windowFlags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP;
                    window->setFullScreen(false);
                } else {
                    window->setFullScreen(true);
                    windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
                }

                co_return;
            }
        }

        if(ke.keysym.sym == 0x40000000 && ! capslockEnable) {
            auto mod = SDL_GetModState();
            SDL_SetModState(static_cast<SDL_Keymod>(mod & ~KMOD_CAPS));
            Application::debug(DebugType::App, "{}: CAPS reset", NS_FuncNameV);
            co_return;
        }

        const bool pressed = ev.type == SDL_KEYDOWN;
        co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
        sendSystemKeyboardEvent(pressed, ke.keysym.scancode, ke.keysym.sym);
        co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
        co_return;
    }

    enum LocalEvent { Resize = 776, ResizeCont = 777 };

    asio::awaitable<void> ClientApp::sdlUserEvent(SDL_Event && ev) {
        const auto & ue = ev.user;
        // resize event
        if(ue.code == LocalEvent::Resize ||
           ue.code == LocalEvent::ResizeCont) {
            const XCB::Size windowSz((ptrdiff_t) ue.data1, (ptrdiff_t) ue.data2);
            bool contUpdateResume = ue.code == LocalEvent::ResizeCont;
            cursors.clear();

            try {
                window->resize(windowSz);
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                co_return;
            }

            // get real size
            windowSize = window->geometry();

            co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
            displayResizeEvent(windowSize);
            // full update
            sendFrameBufferUpdate(false);
            if(contUpdateResume) {
                sendContinuousUpdates(true, {0, 0, windowSize.width, windowSize.height});
            }
            co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
        }
        co_return;
    }

    asio::awaitable<void> ClientApp::sdlDropCompleteEvent(SDL_Event && ev) {
        if(! dropFiles.empty()) {
            co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
            ChannelClient::sendSystemTransferFiles(std::move(dropFiles));
            co_await asio::dispatch(sdl_strand_, asio::use_awaitable);
            dropFiles.clear();
        }
    }

    asio::awaitable<bool> ClientApp::sdlEventProcessing(void) {
        SDL_Event ev;

        if(! SDL_PollEvent(& ev)) {
            co_return false;
        }

        switch(ev.type) {
            case SDL_MOUSEMOTION:
                co_await sdlMouseMotion(std::move(ev));
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                co_await sdlMouseButton(std::move(ev));
                break;

            case SDL_MOUSEWHEEL:
                co_await sdlMouseWheel(std::move(ev));
                break;

            case SDL_WINDOWEVENT:
                co_await sdlWindowEvent(std::move(ev));
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                co_await sdlKeyboardEvent(std::move(ev));
                break;

            case SDL_DROPFILE:
                if(ev.drop.file) {
                    dropFiles.emplace_front(ev.drop.file);
                    SDL_free(ev.drop.file);
                }
                break;

            case SDL_DROPCOMPLETE:
                co_await sdlDropCompleteEvent(std::move(ev));
                break;

            case SDL_USEREVENT:
                co_await sdlUserEvent(std::move(ev));
                break;

            case SDL_QUIT:
                asio::post(ioc(), std::bind(&ClientApp::stop, this));
                break;

            default:
                co_return false;
        }

        co_return true;
    }

    void ClientApp::clientRecvFBUpdateEvent(void) {
        asio::dispatch(sdl_strand_, [this](){
            sdlRenderFrame();
        });
    }

    bool ClientApp::pushEventWindowResize(const XCB::Size & nsz) {

        if(windowSize == nsz) {
            Application::warning("{}: the window has the same size: {}", NS_FuncNameV, nsz);
            return true;
        }

        Application::debug(DebugType::App, "{}: new size: {}", NS_FuncNameV, nsz);

        bool contUpdateResume = false;

        if(isContinueUpdatesProcessed()) {
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

        if(0 > SDL_PushEvent((SDL_Event*) & event)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "SDL_PushEvent",
                               SDL_GetError());
            return false;
        }

        return true;
    }

    void ClientApp::clientRecvDecodingDesktopSizeEvent(int status, int err,
            const XCB::Size & nsz, const std::vector<RFB::ScreenInfo> & screens) {

        Application::debug(DebugType::App, "{}: status: {}, error: {}, size:: {}",
                            NS_FuncNameV, status, err, nsz);

        // 1. server request: status: 0x00, error: 0x00
        if(status == 0 && err == 0) {
            // negotiate part
            if(! serverExtDesktopSizeNego) {
                serverExtDesktopSizeNego = true;

                Application::debug(DebugType::App, "{}: nego part, primary: {}, window: {}",
                            NS_FuncNameV, primarySize, windowSize);

                if(! primarySize.isEmpty() && primarySize != windowSize) {
                    sendSetDesktopSize(primarySize);
                }
            } else {
                // server runtime
                if(windowFullScreen() && primarySize != nsz) {
                    Application::warning("{}: fullscreen mode, server request resize: {}, current primary: {}",
                                         NS_FuncNameV, nsz, primarySize);
                }

                pushEventWindowResize(nsz);
            }
        } else if(status == 1) {
            // 3. server reply
            if(! nsz.isEmpty()) {
                pushEventWindowResize(nsz);
            }

            if(err) {
                Application::error("{}: status: {}, error code: {}", NS_FuncNameV, status, err);
                //if(! nsz.isEmpty())
                //    primarySize.reset();
            }
        }
    }

    void ClientApp::clientRecvPixelFormatEvent(const PixelFormat & pf, const XCB::Size & wsz) {
        Application::info("{}: size: {}", NS_FuncNameV, wsz);
        bool initialized = false;

        if(! window) {
            window = std::make_unique<SDL::Window>(windowTitle, wsz, wsz, windowFlags, windowAccel);
            initialized = true;
        }

        int bpp;
        uint32_t rmask, gmask, bmask, amask;

        if(SDL_TRUE != SDL_PixelFormatEnumToMasks(window->pixelFormat(), &bpp, &rmask,
                &gmask, &bmask, &amask)) {
            Application::error("{}: {} failed, error: {}", NS_FuncNameV,
                               "SDL_PixelFormatEnumToMasks", SDL_GetError());
            throw sdl_error(NS_FuncNameS);
        }

        clientPf = PixelFormat(bpp, rmask, gmask, bmask, amask);
        windowSize = window->geometry();

        if(initialized) {
            // start sdl loop
            asio::co_spawn(sdl_strand_, sdlEventsLoop(),
                    asio::bind_cancellation_slot(sdl_cancel_.slot(), asio::detached));

            asio::post(rfb_strand_, [wsz = windowSize, this]() {
                this->displayResizeEvent(wsz);
            });
        }
    }

    void ClientApp::setPixel(const XCB::Point & dst, uint32_t pixel) {
        fillPixel({dst, XCB::Size{1, 1}}, pixel);
    }

    void ClientApp::fillPixel(const XCB::Region & wrt, uint32_t pixel) {
        if(wrt.isEmpty()) {
            return;
        }

        // move strand
        asio::dispatch(sdl_strand_, [wrt, pixel, this](){
            auto col = clientPf.color(pixel);
            auto dstrt = SDL_Rect{ .x = wrt.x, .y = wrt.y, .w = wrt.width, .h = wrt.height };
            auto dstcol = SDL_Color{ .r = col.r, .g = col.g, .b = col.b, .a = 255 };
            window->renderColor(&dstcol, &dstrt);
        });
    }

    void ClientApp::updateRawPixels(const XCB::Region & wrt, std::vector<uint8_t>&& buf,
                                  uint32_t pitch, const PixelFormat & pf) {
        if(wrt.isEmpty()) {
            return;
        }

        if(auto sdlFormat = SDL_MasksToPixelFormatEnum(pf.bitsPerPixel(),
            pf.rmask(), pf.gmask(), pf.bmask(), pf.amask()); sdlFormat != SDL_PIXELFORMAT_UNKNOWN) {
            updateRawPixels2(wrt, std::move(buf), pitch, sdlFormat);
        }
    }

    void ClientApp::updateRawPixels2(const XCB::Region & wrt, std::vector<uint8_t>&& buf, uint32_t pitch, uint32_t sdlFormat) {
        if(wrt.isEmpty()) {
            return;
        }

        // move strand
        asio::dispatch(sdl_strand_, [wrt, buf = std::move(buf), pitch, sdlFormat, this](){
            auto tx = window->createTexture(wrt.toSize(), SDL_TEXTUREACCESS_STATIC, sdlFormat);
            tx.updateRect(nullptr, buf.data(), pitch);
            const SDL_Rect dstrt{ .x = wrt.x, .y = wrt.y, .w = wrt.width, .h = wrt.height };
            window->renderTexture(tx.get(), nullptr, nullptr, &dstrt);
        });
    }

    const PixelFormat & ClientApp::clientFormat(void) const {
        return clientPf;
    }

    XCB::Size ClientApp::clientSize(void) const {
        return windowSize;
    }

    int ClientApp::clientPrefferedVideoEncoding(void) const {
        return videoEncoding;
    }

    int ClientApp::clientPrefferedAudioEncoding(void) const {
        return audioEncoding;
    }

    void ClientApp::clientRecvRichCursorEvent(const XCB::Region & reg,
                                            std::vector<uint8_t> && pixels, std::vector<uint8_t> && mask) {
        uint32_t key = Tools::crc32b(pixels);
        auto it = cursors.find(key);

        if(cursors.end() == it) {
            auto sdlFormat = SDL_MasksToPixelFormatEnum(clientPf.bitsPerPixel(),
                             clientPf.rmask(), clientPf.gmask(), clientPf.bmask(), clientPf.amask());

            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV,
                                   "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            // pixels data as client format
            Application::debug(DebugType::App, "{}: create cursor, crc32b: {}, size: {}, sdl format: {}",
                               NS_FuncNameV, key, reg.toSize(), SDL_GetPixelFormatName(sdlFormat));

            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width,
                      reg.height, clientPf.bitsPerPixel(), reg.width * clientPf.bytePerPixel(),
                      sdlFormat);

            if(! sf) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV,
                                   "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
                return;
            }

            auto pair = cursors.emplace(key, ColorCursor{ .pixels = std::move(pixels) });
            it = pair.first;
            (*it).second.surface.reset(sf);
            auto curs = SDL_CreateColorCursor(sf, reg.x, reg.y);

            if(! curs) {
                auto & pixels = (*it).second.pixels;
                auto tmp1 = Tools::hexString(pixels, 2, ",", false);
                auto tmp2 = Tools::hexString(mask, 2, ",", false);

                Application::warning("{}: {} failed, error: {}", NS_FuncNameV,
                                     "SDL_CreateColorCursor", SDL_GetError());
                Application::warning("{}: pixels: [{}], mask: [{}]", NS_FuncNameV, tmp1, tmp2);
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void ClientApp::clientRecvLtsmCursorEvent(const XCB::Region & reg, uint32_t cursorId, std::vector<uint8_t> && pixels) {
        auto it = cursors.find(cursorId);

        if(cursors.end() == it) {
            if(pixels.empty()) {
                Application::warning("{}: cursor not found, id: {:#010x}", NS_FuncNameV, cursorId);
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

            if(pixels.size() < static_cast<size_t>(reg.width) * reg.height * 4) {
                Application::error("{}: invalid pixels, length: {}, id: {:#010x}", NS_FuncNameV, pixels.size(), cursorId);
                return;
            }

            if(sdlFormat == SDL_PIXELFORMAT_UNKNOWN) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV,
                                   "SDL_MasksToPixelFormatEnum", SDL_GetError());
                return;
            }

            // pixels data as client format
            Application::debug(DebugType::App, "{}: create cursor, crc32b: {}, size: {}, sdl format: {}",
                               NS_FuncNameV, cursorId, reg.toSize(), SDL_GetPixelFormatName(sdlFormat));

            auto sf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), reg.width,
                      reg.height, clientPf.bitsPerPixel(), reg.width * cursorFmt.bytePerPixel(),
                      sdlFormat);

            if(! sf) {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV,
                                   "SDL_CreateRGBSurfaceWithFormatFrom", SDL_GetError());
                return;
            }

            auto pair = cursors.emplace(cursorId, ColorCursor{ .pixels = std::move(pixels) });
            it = pair.first;
            (*it).second.surface.reset(sf);
            auto curs = SDL_CreateColorCursor(sf, reg.x, reg.y);

            if(! curs) {
                Application::warning("{}: {} failed, error: {}", NS_FuncNameV,
                                     "SDL_CreateColorCursor", SDL_GetError());

                Application::warning("{}: send cursor failed, id: {:#010x}", NS_FuncNameV, cursorId);
                sendSystemCursorFailed(cursorId);
                return;
            }

            (*it).second.cursor.reset(curs);
        }

        SDL_SetCursor((*it).second.cursor.get());
    }

    void ClientApp::clientRecvLtsmHandshakeEvent(int flags) {
#ifdef __UNIX__
        asio::co_spawn(x11_strand_, [this]() -> asio::awaitable<void> {
            if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB))) {
                auto names = extXkb->getNames();
                auto group = extXkb->getLayoutGroup();
                // switch to rfb_strand
                co_await asio::dispatch(rfb_strand_, asio::use_awaitable);
                this->sendSystemClientVariables(this->clientOptions(), this->clientEnvironments(), names,
                                  (0 <= group && group < names.size() ? names[group] : ""));
            }
            co_return;
        }, asio::detached);
#else
        asio::dispatch(rfb_strand_, [this]() {
            this->sendSystemClientVariables(this->clientOptions(), this->clientEnvironments(), {}, "");
        });
#endif
    }

#ifdef __UNIX__
    void ClientApp::xcbXkbGroupChangedEvent(int group) {
        if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)); extXkb && useXkb) {
            asio::dispatch(rfb_strand_, [names = extXkb->getNames(), group, this]() {
                this->sendSystemKeyboardChange(names, group);
            });
        }
    }
#endif

    json_plain ClientApp::clientEnvironments(void) const {
        JsonObjectStream jo;
#ifdef __UNIX__
        // locale
        std::initializer_list<std::pair<int, std::string>> lcall = { { LC_CTYPE, "LC_TYPE" }, { LC_NUMERIC, "LC_NUMERIC" }, { LC_TIME, "LC_TIME" },
            { LC_COLLATE, "LC_COLLATE" }, { LC_MONETARY, "LC_MONETARY" }, { LC_MESSAGES, "LC_MESSAGES" }
        };

        for(const auto & lc : lcall) {
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
        if(! seamless.empty()) {
            jo.push("XSESSION", seamless);
        }

        return jo.flush();
    }

    json_plain ClientApp::clientOptions(void) const {
        JsonArrayStream opts;
        opts.push(videoEncodingOptions);
        opts.push(audioEncodingOptions);

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
        jo.push("ltsm:client", LTSM_CLIENT_VERSION);
        jo.push("x11:nodamage", xcbNoDamage);
        jo.push("x11:dpi", xcbDpi);
        jo.push("enc:opts", opts.flush());

        if(frameRate) {
            jo.push("frame:rate", frameRate);
        }

        if(username.empty()) {
            if(auto env = std::getenv("USER")) {
                jo.push("username", env);
            } else if(auto env = std::getenv("USERNAME")) {
                jo.push("username", env);
            }
        } else {
            jo.push("username", username);
        }

        if(! rfbsec.passwdFile.empty()) {
            jo.push("password", rfbsec.passwdFile);
        }

        if(! rfbsec.certFile.empty()) {
            jo.push("certificate", Tools::fileToString(rfbsec.certFile));
        }

        if(! printerUrl.empty()) {
            Application::info("{}: {} url: {}", NS_FuncNameV, "printer",
                              printerUrl);
            jo.push("redirect:cups", printerUrl);
        }

        if(! saneUrl.empty()) {
            Application::info("{}: {} url: {}", NS_FuncNameV, "sane", saneUrl);
            jo.push("redirect:sane", saneUrl);
        }

        if(! shareFolders.empty()) {
            jo.push("redirect:fuse", JsonArrayStream(shareFolders).flush());
        }

        if(pcscEnable) {
            jo.push("redirect:pcsc", "enable");
        }

#ifdef LTSM_PKCS11_AUTH

        if(! pkcs11Auth.empty()) {
            jo.push("pkcs11:auth", pkcs11Auth);
        }

#endif

        if(audioEnable) {
            jo.push("redirect:audio", "enable");
        }

        return jo.flush();
    }

    void ClientApp::systemLoginSuccess(const JsonObject & jo) {
        if(jo.getBoolean("action", false)) {
            if(! primarySize.isEmpty() && primarySize != windowSize) {
                sendSetDesktopSize(primarySize);
            }
        } else {
            auto error = jo.getString("error");
            Application::error("{}: {} failed, error: {}", NS_FuncNameV, "login",
                               error);
        }
    }

    void ClientApp::clientRecvBellEvent(void) {
#ifdef __UNIX__
        bell(75);
#endif
    }

    bool ClientApp::createChannelAllow(const Channel::ConnectorType & type, const std::string & content,
                                     const Channel::ConnectorMode & mode) const {
        if(type == Channel::ConnectorType::Fuse) {
            if(std::ranges::none_of(shareFolders, [&](auto & val) { return val == content; })) {
                Application::error("{}: {} failed, path: `{}'", NS_FuncNameV, "share", content);
                return false;
            }
        }

        return true;
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
    auto home = getenv("HOME");
    if(! home) {
        std::cerr << "HOME environment not found" << std::endl;
        return EXIT_FAILURE;
    }
    auto localcfg = Tools::replace(usercfgdef, "$HOME", home);
#endif

    auto argBeg = argv + 1;
    auto argEnd = argv + argc;

    if((argBeg == argEnd && ! std::filesystem::is_regular_file(localcfg)) ||
        std::ranges::any_of(argBeg, argEnd, [](std::string_view arg) { return arg == "--help" || arg == "-h"; })) {
        printHelp(argv[0]);
        return 0;
    }

    if(auto it = std::ranges::find_if(argBeg, argEnd,
        [](std::string_view arg) { return arg == "--save"; }); it != argEnd) {
        std::string_view path = localcfg;

        if(auto it2 = std::next(it); it2 != argEnd) {
            std::string_view path2 = *it2;

            if(! startsWith(path2, "--")) {
                path = path2;
            }
        }

        saveConfig(argBeg, it, path);
        return 0;
    }

    // init network
#ifdef __WIN32__
    WSADATA wsaData;

    if(int ret = WSAStartup(MAKEWORD(2, 2), & wsaData); ret != 0) {
        std::cerr << "WSAStartup failed: {}" << std::endl;
        return 1;
    }

#endif

    if(0 > SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "sdl init video failed" << std::endl;
        return -1;
    }

    // enable drop file
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool programRestarting = true;
    int res = 0;

    while(programRestarting) {
        try {
#ifdef __WIN32__
            ClientApp app(argc, (const char**) argv);
#else
            ClientApp app(argc, argv);
#endif

            if(! app.isAlwaysRunning()) {
                programRestarting = false;
            }

            res = app.start();
        } catch(const std::invalid_argument & err) {
            std::cerr << "unknown params: " << err.what() << std::endl << std::endl;
            return -1;
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            Application::info("program: {}", "terminate...");
        }
    }

    SDL_Quit();
    return res;
}
