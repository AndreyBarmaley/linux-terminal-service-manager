/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifdef __UNIX__
#include <sys/socket.h>
#endif

#include <sys/stat.h>

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <utility>
#include <fstream>
#include <exception>
#include <filesystem>

#include "channel_system.h"
#include "ltsm_application.h"
#include "ltsm_zlib.h"
#include "ltsm_tools.h"
#include "ltsm_librfb.h"
#include "ltsm_json_wrapper.h"

using namespace std::chrono_literals;
using namespace boost;
using namespace LTSM;

namespace LTSM::Channel::Connector {
    void loopWriter(ConnectorBase*, Remote2Local*);
    void loopReader(ConnectorBase*, Local2Remote*);

    std::pair<std::string, int> parseAddrPort(const std::string &);
}

///
std::string Channel::createUrl(const ConnectorType & type, std::string_view body) {
    return std::string(Connector::typeString(type)).append("://").append(body);
}

Channel::ConnectorType Channel::connectorType(std::string_view str) {

    for(auto type : {
            ConnectorType::Unix, ConnectorType::Socket, ConnectorType::File, ConnectorType::Command,
            ConnectorType::Fuse, ConnectorType::Audio, ConnectorType::Pcsc, ConnectorType::Pkcs11
        }) {
        if(str == Connector::typeString(type)) {
            return type;
        }
    }

    return ConnectorType::Unknown;
}

Channel::ConnectorMode Channel::connectorMode(std::string_view str) {
    for(auto mode : {
            ConnectorMode::ReadOnly, ConnectorMode::ReadWrite, ConnectorMode::WriteOnly
        }) {
        if(str == Connector::modeString(mode)) {
            return mode;
        }
    }

    return ConnectorMode::Unknown;
}

Channel::Speed Channel::connectorSpeed(std::string_view str) {
    for(auto speed : {
            Speed::VerySlow, Speed::Slow, Speed::Medium, Speed::Fast, Speed::UltraFast, Speed::Ultra5
        }) {
        if(str == Connector::speedString(speed)) {
            return speed;
        }
    }

    return Speed::VerySlow;
}

const char* Channel::Connector::typeString(const ConnectorType & type) {
    switch(type) {
        case ConnectorType::Unix:
            return "unix";

        case ConnectorType::File:
            return "file";

        case ConnectorType::Socket:
            return "socket";

        case ConnectorType::Command:
            return "command";

        case ConnectorType::Fuse:
            return "fuse";

        case ConnectorType::Audio:
            return "audio";

        case ConnectorType::Pcsc:
            return "pcsc";

        case ConnectorType::Pkcs11:
            return "pkcs11";

        default:
            break;
    }

    return "unknown";
}

const char* Channel::Connector::modeString(const ConnectorMode & mode) {
    switch(mode) {
        // default mode - unix: rw, socket: rw, file(present): ro, file(not found): wo
        case ConnectorMode::ReadWrite:
            return "rw";

        case ConnectorMode::ReadOnly:
            return "ro";

        case ConnectorMode::WriteOnly:
            return "wo";

        default:
            break;
    }

    return "unknown";
}

const char* Channel::Connector::speedString(const Speed & speed) {
    switch(speed) {
        case Speed::Slow:
            return "slow";

        case Speed::Medium:
            return "medium";

        case Speed::Fast:
            return "fast";

        case Speed::UltraFast:
            return "ultra";

        case Speed::Ultra5:
            return "ultra5";

        default:
            break;
    }

    return "very";
}

std::pair<Channel::ConnectorType, std::string>
Channel::parseUrl(std::string_view url) {
    if(startsWith(url, "file://")) {
        return std::make_pair(Channel::ConnectorType::File, view2string(url.substr(7)));
    }

    if(startsWith(url, "unix://")) {
        return std::make_pair(Channel::ConnectorType::Unix, view2string(url.substr(7)));
    }

    if(startsWith(url, "sock://")) {
        return std::make_pair(Channel::ConnectorType::Socket, view2string(url.substr(7)));
    }

    if(startsWith(url, "socket://")) {
        return std::make_pair(Channel::ConnectorType::Socket, view2string(url.substr(9)));
    }

    if(startsWith(url, "cmd://")) {
        return std::make_pair(Channel::ConnectorType::Command, view2string(url.substr(6)));
    }

    if(startsWith(url, "command://")) {
        return std::make_pair(Channel::ConnectorType::Command, view2string(url.substr(10)));
    }

    if(startsWith(url, "fuse://")) {
        return std::make_pair(Channel::ConnectorType::Fuse, view2string(url.substr(7)));
    }

    if(startsWith(url, "audio://")) {
        return std::make_pair(Channel::ConnectorType::Audio, view2string(url.substr(8)));
    }

    if(startsWith(url, "pcsc://")) {
        return std::make_pair(Channel::ConnectorType::Pcsc, view2string(url.substr(7)));
    }

    if(startsWith(url, "pkcs11://")) {
        return std::make_pair(Channel::ConnectorType::Pkcs11, view2string(url.substr(9)));
    }

    return std::make_pair(Channel::ConnectorType::Unknown, view2string(url));
}

std::pair<std::string, int>
Channel::Connector::parseAddrPort(const std::string & addrPort) {
    Application::debug(DebugType::Channels, "{}: addr: `{}'", NS_FuncNameV, addrPort);

    // format url
    // url1: hostname:port
    // url2: xx.xx.xx.xx:port
    auto list = Tools::split(addrPort, ':');

    int port = -1;
    std::string addr = "127.0.0.1";

    if(2 != list.size()) {
        return std::make_pair(addr, port);
    }

    // check addr
    if(auto octets = Tools::split(list.front(), '.'); 4 == octets.size()) {
        bool error = false;

        try {
            // check numbers
            if(std::ranges::any_of(octets, [](auto & val) { return 255 < std::stoi(val); })) {
                error = true;
            }
        } catch(const std::exception & err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            error = true;
        }

        if(error) {
            Application::error("{}: {}, addr: `{}'", NS_FuncNameV, "incorrect ipaddr", addrPort);
        }
    } else
        // resolv hostname
    {
        std::string addr2 = TCPSocket::resolvHostname(list.front());

        if(addr2.empty()) {
            Application::error("{}: {}, addr: `{}'", NS_FuncNameV, "incorrect hostname", addrPort);
        } else {
            addr = addr2;
        }
    }

    // check port
    try {
        port = std::stoi(list.back());
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return std::make_pair(addr, port);
}

/// ChannelBase
Channel::ConnectorBase* ChannelBase::findChannel(CID channel) {
    const std::scoped_lock guard{lockch};
    if(auto& ptr = channels_[channel]) {
        return ptr.get();
    }
    return nullptr;
}

Channel::Planned* ChannelBase::findPlanned(CID channel) {
    const std::scoped_lock guard{lockpl};
    auto it = std::ranges::find_if(channelsPlanned, [=](auto & st) {
        return st.channel == channel;
    });

    return it != channelsPlanned.end() ? & (*it) : nullptr;
}

size_t ChannelBase::countFreeChannels(void) const {
    const std::scoped_lock guard{lockch, lockpl};

    auto channels_valid = std::count_if(channels_.begin(), channels_.end(), [](auto& ptr){ return !!ptr; });
    auto used = (2 + channels_valid + channelsPlanned.size());

    if(used > ChannelLimit) {
        Application::error("{}: used channel count is large, count: {}", NS_FuncNameV, used);
        throw channel_error(NS_FuncNameS);
    }

    return ChannelLimit - used;
}

void ChannelBase::recvLtsmEvent(CID channel, std::vector<uint8_t> && buf) {
    if(channel == ChannelTypeReserved) {
        Application::error("{}: reserved channel blocked", NS_FuncNameV);
        throw std::invalid_argument(NS_FuncNameS);
    }

    if(channel == ChannelTypeSystem) {
        JsonContent jc;
        jc.parseBinary(reinterpret_cast<const char*>(buf.data()), buf.size());

        if(! jc.isObject()) {
            Application::error("{}: {}", NS_FuncNameV, "json broken");
            throw std::invalid_argument(NS_FuncNameS);
        }

        recvChannelSystem(jc);
    } else {
        recvChannelData(channel, std::move(buf));
    }
}

void ChannelBase::recvChannelSystem(const JsonContent & jc) {
    auto jo = jc.toObject();
    auto cmd = jo.getString("cmd");

    if(cmd.empty()) {
        Application::error("{}: {}", NS_FuncNameV, "format message broken");
        throw std::invalid_argument(NS_FuncNameS);
    }

    Application::debug(DebugType::Rfb, "{}: cmd: {}", NS_FuncNameV, cmd);

    if(cmd == SystemCommand::ChannelClose) {
        return systemChannelCloseEvent(jo);
    }

    if(cmd == SystemCommand::ChannelConnected) {
        return systemChannelConnectedEvent(jo);
    }

    if(cmd == SystemCommand::ChannelError) {
        return systemChannelErrorEvent(jo);
    }

    recvChannelSystemEvent(cmd, jo);
}

void ChannelBase::recvChannelData(CID channel, std::vector<uint8_t> && buf) {
    Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

    auto& channelConn = channels_[channel];

    if(! channelConn) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not found", channel);
        throw std::invalid_argument(NS_FuncNameS);
    }

#ifndef LTSM_CLIENT

    if(channelConn->isAllowSessionFor(true) != isUserSession()) {
        Application::error("{}: ltsm channel disable for session: `{}'", NS_FuncNameV, (isUserSession() ? "user" : "login"));
        throw std::invalid_argument(NS_FuncNameS);
    }

#endif

    if(! channelConn->isRemoteConnected()) {
        Application::error("{}: {}, id: {}, error: {}", NS_FuncNameV, "channel not connected", channel, channelConn->error());
        throw std::invalid_argument(NS_FuncNameS);
    }

    if(! channelConn->isRunning()) {
        Application::error("{}: {}, id: {}, error: {}", NS_FuncNameV, "channel not running", channel, channelConn->error());
        throw std::invalid_argument(NS_FuncNameS);
    }

    channelConn->pushData(std::move(buf));
}

bool ChannelBase::channelPlannedCreate(CID channel, const Channel::Planned & job) {

    if(0 <= job.serverFd) {
        Application::info("{}: {}, id: {}, client url: `{}', server url: `{}'", NS_FuncNameV, "found planned job", channel, job.clientOpts.url, "listener");

        switch(job.serverOpts.type()) {
#ifdef __UNIX__

            case Channel::ConnectorType::Unix:
                createChannelUnixFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                break;

            case Channel::ConnectorType::Socket:
                createChannelSocketFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                break;
#endif

            default:
                Application::error("{}: {}, id: {}", NS_FuncNameV, "channel type not implemented", channel);
                throw channel_error(NS_FuncNameS);
        }
    } else if(! job.serverOpts.content().empty()) {
        Application::info("{}: {}, id: {}, client url: `{}', server url: `{}'", NS_FuncNameV, "found planned job", channel, job.clientOpts.url, job.serverOpts.url);

        switch(job.serverOpts.type()) {
#ifdef __UNIX__

            case Channel::ConnectorType::Unix:
                createChannelUnix(job.channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
                break;

            case Channel::ConnectorType::Socket:
                createChannelSocket(job.channel, Channel::Connector::parseAddrPort(job.serverOpts.content()), job.serverOpts.mode, job.chOpts);
                break;
#endif

            case Channel::ConnectorType::File:
                createChannelFile(job.channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
                break;

            case Channel::ConnectorType::Command:
                createChannelCommand(job.channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
                break;

            default:
                Application::error("{}: {}, id: {}", NS_FuncNameV, "channel type not implemented", channel);
                return false;
        }
    }
    
    return true;
}

void ChannelBase::systemChannelConnectedEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    bool error = jo.getBoolean("error");
    int flags = jo.getInteger("flags", 0);

    // move planed to running
    const std::scoped_lock guard{lockpl};
    auto it = std::ranges::find_if(channelsPlanned, [=](auto & st) {
        return st.channel == channel;
    });

    // FIXME найди условие если не так.. это СЕРВЕР
    if(it != channelsPlanned.end()) {
        auto job = std::move(*it);
        channelsPlanned.erase(it);
        job.chOpts.flags = flags;

        auto jobFailed = [&job]() {
            if(0 <= job.serverFd) {
                close(job.serverFd);
                job.serverFd = -1;
            }
        };

        if(error) {
            Application::error("{}: {}, id: {}", NS_FuncNameV, "client connect error", channel);
            jobFailed();
            throw channel_error(NS_FuncNameS);
        }

        if(job.channel <= ChannelTypeSystem || job.channel >= ChannelTypeReserved) {
            Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", job.channel);
            jobFailed();
            throw channel_error(NS_FuncNameS);
        }

        if(auto& ptr = channels_[job.channel]) {
            Application::error("{}: {}, id: {}", NS_FuncNameV, "channel busy", channel);
            jobFailed();
            throw channel_error(NS_FuncNameS);
        }

        if(! channelPlannedCreate(channel, job)) {
            jobFailed();
            throw channel_error(NS_FuncNameS);
        }
    }

    // set connected flag
    if(auto& ptr = channels_[channel]) {
        ptr->setRemoteConnected(true);
    } else {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not running", channel);
        throw channel_error(NS_FuncNameS);
    }
}

void ChannelBase::systemChannelCloseEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    destroyChannel(channel);
}

asio::awaitable<bool> ChannelBase::sendSystemTransferFiles(std::forward_list<std::string> files) {
    Application::info("{}", NS_FuncNameV);

    std::erase_if(files, [](auto & file) {
        std::error_code err;

        if(! std::filesystem::is_regular_file(file, err)) {
            Application::warning("{}: {} failed, code: {}, error: {}, path: `{}'",
                NS_FuncNameV, "is_regular_file", err.value(), err.message(), file);
            return true;
        }

        if(0 != access(file.c_str(), R_OK)) {
            Application::warning("{}: skip not readable, file: {}", "sendSystemTransferFiles", file);
            return true;
        }

        return false;
    });

    if(files.empty()) {
        Application::error("{}: failed,  empty list", NS_FuncNameV);
        co_return false;
    }

    JsonObjectStream jo;
    jo.push("cmd", SystemCommand::TransferFiles);

    JsonArrayStream ja;

    for(const auto & fname : files) {
        std::error_code err;

        if(auto fsize = std::filesystem::file_size(fname, err)) {
            ja.push(JsonObjectStream().push("file", fname).push("size", static_cast<size_t>(fsize)).flush());
        }
    }

    jo.push("files", ja.flush());

    sendLtsmChannelData(ChannelTypeSystem, jo.flush());
    co_return true;
}

bool ChannelBase::createChannel(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts) {
    if(clientOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "client");
        return false;
    }

    if(serverOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "server");
        return false;
    }

    if(serverOpts.mode == clientOpts.mode &&
       (serverOpts.mode == Channel::ConnectorMode::ReadOnly || serverOpts.mode == Channel::ConnectorMode::WriteOnly)) {
        Application::error("{}: incorrect modes pair (wo,wo) or (ro,ro)", NS_FuncNameV);
        return false;
    }

    Application::debug(DebugType::Channels, "{}: server url: `{}', client url: `{}'", NS_FuncNameV, serverOpts.url, clientOpts.url);

    if(clientOpts.type() == Channel::ConnectorType::Unknown) {
        Application::error("{}: unknown client url: `{}'", NS_FuncNameV, clientOpts.url);
        return false;
    }

    if(serverOpts.type() == Channel::ConnectorType::Unknown) {
        Application::error("{}: unknown server url: `{}'", NS_FuncNameV, serverOpts.url);
        return false;
    }

    // find free channel
    CID channel = 1;

    for(; channel < ChannelTypeReserved; ++channel) {
        if(! channels_[channel] && ! findPlanned(channel)) {
            break;
        }
    }

    if(channel == ChannelTypeReserved) {
        Application::error("{}: all channels busy", NS_FuncNameV);
        return false;
    } else {
        const std::scoped_lock guard{lockpl};

        channelsPlanned.emplace_back(
            Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = chOpts, .channel = channel });
    }

    // send channel open to client
    sendSystemChannelOpen(channel, clientOpts, chOpts);

    // next part: ChannelBase::systemChannelConnected

    return true;
}

bool ChannelBase::createChannelBaseAudio(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_AUDIO)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createClientAudioConnector(channel, url, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported audio", url);
    return false;
#endif
}

bool ChannelBase::createChannelBaseFuse(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_FUSE)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createClientFuseConnector(channel, url, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported fuse", url);
    return false;
#endif
}

bool ChannelBase::createChannelBasePcsc(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_PCSC)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createClientPcscConnector(channel, url, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported pcsc", url);
    return false;
#endif
}

#ifdef __UNIX__
bool ChannelBase::createChannelUnix(CID channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Unix, path.native(), mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", path);
        return false;
    }

    Application::debug(DebugType::Channels, "{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createUnixConnector(channel, path, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}

bool ChannelBase::createChannelUnixFd(CID channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    Application::debug(DebugType::Channels, "{}: id: {}, sock: {}, mode: {}", NS_FuncNameV, channel, sock, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createUnixConnector(channel, sock, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}

#endif // __UNIX__

bool ChannelBase::createChannelBasePkcs11(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_PKCS11_AUTH)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createClientPkcs11Connector(channel, url, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported pkcs11", url);
    return false;
#endif
}

bool ChannelBase::createChannelFile(CID channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#ifdef __WIN32__
    if(! allowCreateChannel(Channel::ConnectorType::File, path.string(), mode))
#else
    if(! allowCreateChannel(Channel::ConnectorType::File, path.native(), mode))
#endif
    {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", path);
        return false;
    }

    Application::debug(DebugType::Channels, "{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createFileConnector(channel, path, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}

bool ChannelBase::createChannelCommand(CID channel, const std::string & runcmd, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Command, runcmd, mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", runcmd);
        return false;
    }

    Application::debug(DebugType::Channels, "{}: id: {}, run cmd: `{}', mode: {}", NS_FuncNameV, channel, runcmd, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createCommandConnector(channel, runcmd, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}

#ifdef __UNIX__
bool ChannelBase::createChannelSocket(CID channel, std::pair<std::string, int> ipAddrPort, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Socket, ipAddrPort.first, mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", ipAddrPort.first);
        return false;
    }

    Application::debug(DebugType::Channels, "{}: id: {}, addr: {}, port: {}, mode: {}", NS_FuncNameV, channel, ipAddrPort.first, ipAddrPort.second, Channel::Connector::modeString(mode));

    if(serverSide() && ! startsWith(ipAddrPort.first, "127.")) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "server side allow socket only for localhost", channel);
        return false;
    }

    if(0 > ipAddrPort.second) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "incorrect connection info", channel);
        return false;
    }

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createTcpConnector(channel, ipAddrPort.first, ipAddrPort.second, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}

bool ChannelBase::createChannelSocketFd(CID channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    Application::debug(DebugType::Channels, "{}: id: {}, sock: {}, mode: {}", NS_FuncNameV, channel, sock, Channel::Connector::modeString(mode));

    try {
        const std::scoped_lock guard{lockch};
        channels_[channel] = std::move(Channel::createTcpConnector(channel, sock, mode, chOpts, *this));
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    return true;
}
#endif

void ChannelBase::plannedEmplace(Channel::Planned && val) {
    const std::scoped_lock guard{lockpl};

    channelsPlanned.emplace_back(std::move(val));
}

void ChannelBase::destroyChannel(CID channel) {
    const std::scoped_lock guard{this->lockch};

    if(auto& ptr = channels_[channel]) {
        ptr->setRunning(false);
        ptr.reset();
        Application::info("{}: {}, id: {}", NS_FuncNameV, "channel removed", channel);
    } else {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not running", channel);
    }
}

void ChannelBase::sendSystemChannelOpen(CID channel, const Channel::UrlMode & clientOpts, const Channel::Opts & chOpts) {
    Application::info("{}: id: {}, content: `{}'", NS_FuncNameV, channel, clientOpts.content());
    JsonObjectStream jo;

    jo.push("cmd", SystemCommand::ChannelOpen);
    jo.push("id", channel);
    jo.push("type", Channel::Connector::typeString(clientOpts.type()));
    jo.push("mode", Channel::Connector::modeString(clientOpts.mode));
    jo.push("speed", Channel::Connector::speedString(chOpts.speed));
    jo.push("flags", chOpts.flags);

    if(clientOpts.type() == Channel::ConnectorType::Socket) {
        auto [ ipaddr, port ] = Channel::Connector::parseAddrPort(clientOpts.content());
        jo.push("port", port);
        jo.push("ipaddr", ipaddr);
    } else if(clientOpts.type() == Channel::ConnectorType::Command) {
        jo.push("runcmd", clientOpts.content());
    } else if(clientOpts.type() == Channel::ConnectorType::Fuse) {
        jo.push("fuse", clientOpts.content());
    } else if(clientOpts.type() == Channel::ConnectorType::Audio) {
        jo.push("audio", clientOpts.content());
    } else if(clientOpts.type() == Channel::ConnectorType::Pcsc) {
        jo.push("pcsc", clientOpts.content());
    } else if(clientOpts.type() == Channel::ConnectorType::Pkcs11) {
        jo.push("pkcs11", clientOpts.content());
    } else {
        jo.push("path", clientOpts.content());
    }

    sendLtsmChannelData(ChannelTypeSystem, jo.flush());
}

void ChannelBase::sendSystemChannelError(CID channel, int code, const std::string & err) {
    sendLtsmChannelData(ChannelTypeSystem, JsonObjectStream().push("cmd", SystemCommand::ChannelError).push("id", channel).push("code", code).push("error", err).flush());
}

void ChannelBase::sendSystemChannelClose(CID channel) {
    sendLtsmChannelData(ChannelTypeSystem, JsonObjectStream().push("cmd", SystemCommand::ChannelClose).push("id", channel).flush());
}

void ChannelBase::sendSystemChannelConnected(CID channel, int flags, bool noerror) {
    sendLtsmChannelData(ChannelTypeSystem, JsonObjectStream().
                        push("cmd", SystemCommand::ChannelConnected).
                        push("flags", flags).
                        push("error", ! noerror).
                        push("id", channel).flush());
}

void ChannelBase::recvLtsmProto(CID channel, std::vector<uint8_t> && buf)
{
    Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

    if(channelDebug == channel) {
        auto str = Tools::hexString(buf, 2);
        Application::trace(DebugType::Channels, "{}: id: {}, size: {}, content: [{}]",
                           NS_FuncNameV, channel, buf.size(), str);
    }

    recvLtsmEvent(channel, std::move(buf));
}

void ChannelBase::setChannelDebug(CID channel, bool debug) {
    if(debug) {
        channelDebug = channel;
    } else if(channelDebug == channel) {
        channelDebug = -1;
    }
}

void ChannelBase::channelsShutdown(void) {
    const std::scoped_lock guard{lockch};

    for(auto & ptr : channels_) {
        if(ptr) {
            ptr->setRunning(false);
            ptr.reset();
        }
    }
}

/// ChannelClient
void ChannelClient::sendSystemClientVariables(const json_plain & vars, const json_plain & env, const std::vector<std::string> & layouts, const std::string & group) {
    JsonObjectStream jo;
    jo.push("cmd", SystemCommand::ClientVariables);
    jo.push("options", vars);
    jo.push("environments", env);

    JsonObjectStream jo2;
    jo2.push("layouts", JsonArrayStream(layouts).flush());
    jo2.push("current", group);

    jo.push("keyboard", jo2.flush());

    sendLtsmChannelData(ChannelTypeSystem, jo.flush());
}

void ChannelClient::sendSystemCursorFailed(int cursorId) {
    JsonObjectStream jo;
    jo.push("cmd", SystemCommand::CursorFailed);
    jo.push("cursor", cursorId);

    sendLtsmChannelData(ChannelTypeSystem, jo.flush());
}

void ChannelClient::sendSystemKeyboardChange(const std::vector<std::string> & names, int group) {
    if(0 <= group && group < names.size()) {
        JsonObjectStream jo;
        jo.push("cmd", SystemCommand::KeyboardChange);
        jo.push("layout", names[group]);
        jo.push("group", group);
        jo.push("names", JsonArrayStream(names).flush());

        sendLtsmChannelData(ChannelTypeSystem, jo.flush());
    }
}

void ChannelClient::recvChannelSystemEvent(const std::string& cmd, const JsonObject & jo) {
    if(cmd == SystemCommand::ChannelOpen) {
        return systemChannelOpenEvent(jo);
    }

    if(cmd == SystemCommand::ChannelListen) {
        return systemChannelListenEvent(jo);
    }

    if(cmd == SystemCommand::LoginSuccess) {
        return systemLoginSuccessEvent(jo);
    }

    Application::error("{}: {}", NS_FuncNameV, "unknown cmd");
    throw std::invalid_argument(NS_FuncNameS);
}

void ChannelClient::systemChannelOpenEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    auto stype = jo.getString("type");
    auto smode = jo.getString("mode");
    auto sspeed = jo.getString("speed");
    int flags = jo.getInteger("flags", 0);
    bool replyError = false;

    Application::info("{}: id: {}, type: {}, mode: {}, speed: {}, flags: {:#010x}", NS_FuncNameV, channel, stype, smode, sspeed, flags);

/*
    if(! isUserSession()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "not user session", channel);
        replyError = true;
    }
*/

    if(channel <= ChannelTypeSystem || channel >= ChannelTypeReserved) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", channel);
        replyError = true;
    }

    Channel::ConnectorMode mode = Channel::connectorMode(smode);

    if(mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "unknown channel mode", channel);
        replyError = true;
    }

    if(findChannel(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel busy", channel);
        replyError = true;
    }

    if(! replyError) {
        Channel::ConnectorType type = Channel::connectorType(stype);
        Channel::Opts chopts{ Channel::connectorSpeed(sspeed), flags };

        if(type == Channel::ConnectorType::File) {
            replyError = ! createChannelFile(channel, jo.getString("path"), mode, chopts);
        } else if(type == Channel::ConnectorType::Audio) {
            replyError = ! createChannelBaseAudio(channel, jo.getString("audio"), mode, chopts);
        } else if(type == Channel::ConnectorType::Fuse) {
            replyError = ! createChannelBaseFuse(channel, jo.getString("fuse"), mode, chopts);
        } else if(type == Channel::ConnectorType::Pcsc) {
            replyError = ! createChannelBasePcsc(channel, jo.getString("pcsc"), mode, chopts);
        }

#ifdef __UNIX__
        else if(type == Channel::ConnectorType::Unix) {
            replyError = ! createChannelUnix(channel, jo.getString("path"), mode, chopts);
        } else if(type == Channel::ConnectorType::Socket) {
            replyError = ! createChannelSocket(channel, std::make_pair(jo.getString("ipaddr"), jo.getInteger("port")), mode, chopts);
        }

#endif
#ifdef LTSM_PKCS11_AUTH
        else if(type == Channel::ConnectorType::Pkcs11) {
            replyError = ! createChannelBasePkcs11(channel, jo.getString("pkcs11"), mode, chopts);
        }

#endif
        else if(type == Channel::ConnectorType::Command) {
            replyError = ! createChannelCommand(channel, jo.getString("runcmd"), mode, chopts);
        } else {
            Application::error("{}: {} `{}', id: {}", NS_FuncNameV, "unknown channel type", stype, channel);
            replyError = true;
        }
    }

    if(replyError) {
        sendSystemChannelConnected(channel, flags, false);
    }
}

void ChannelClient::systemChannelListenEvent(const JsonObject & jo) {
}

#ifdef __UNIX__
// ChannelListener
bool ChannelListener::createListener(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, size_t listen, const Channel::Opts & chOpts) {
    Application::debug(DebugType::Channels, "{}: client: {}, server: {}", NS_FuncNameV, clientOpts.url, serverOpts.url);

    try {
        const std::scoped_lock guard{lockls};

        if(std::ranges::any_of(listeners, [&](auto & ptr) { return ptr->getServerUrl() == serverOpts.url; })) {
            Application::debug(DebugType::Channels, "{}: listen present, url: {}", NS_FuncNameV, serverOpts.url);
            return true;
        }

        if(serverOpts.type() == Channel::ConnectorType::Socket) {
            listeners.emplace_back(Channel::createTcpListener(serverOpts, listen, clientOpts, chOpts, *this));
            return true;
        } else if(serverOpts.type() == Channel::ConnectorType::Unix) {
            listeners.emplace_back(Channel::createUnixListener(serverOpts, listen, clientOpts, chOpts, *this));
            return true;
        }
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    Application::error("{}: allow unix or socket format only, url: `{}'", NS_FuncNameV, serverOpts.url);
    return false;
}

void ChannelListener::destroyListener(const std::string & clientUrl, const std::string & serverUrl) {
    const std::scoped_lock guard{lockls};
    auto it = std::ranges::find_if(listeners, [&](auto & ptr) {
        return ptr && ptr->getClientUrl() == clientUrl;
    });

    if(it != listeners.end()) {
        (*it)->setRunning(false);

        std::this_thread::sleep_for(100ms);
        listeners.erase(it);
        Application::info("{}: client url: `{}'", NS_FuncNameV, clientUrl);
    }
}

bool ChannelListener::createChannelAcceptFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts) {
    if(clientOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "client");
        return false;
    }

    if(serverOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "server");
        return false;
    }

    if(serverOpts.mode == clientOpts.mode &&
       (serverOpts.mode == Channel::ConnectorMode::ReadOnly || serverOpts.mode == Channel::ConnectorMode::WriteOnly)) {
        Application::error("{}: incorrect modes pair (wo,wo) or (ro,ro)", NS_FuncNameV);
        return false;
    }

    // parse url client
    Application::debug(DebugType::Channels, "client url: `{}', mode: {}", clientOpts.url, Channel::Connector::modeString(clientOpts.mode));

    if(clientOpts.type() == Channel::ConnectorType::Unknown) {
        Application::error("{}: unknown client url: `{}'", NS_FuncNameV, clientOpts.url);
        return false;
    }

    // find free channel
    CID channel = 1;

    for(; channel < ChannelTypeReserved; ++channel) {
        if(! findChannel(channel) && ! findPlanned(channel)) {
            break;
        }
    }

    if(channel == ChannelTypeReserved) {
        Application::error("{}: all channels busy", NS_FuncNameV);
        return false;
    } else {
        plannedEmplace(Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = chOpts, .serverFd = sock, .channel = channel });
    }

    // send channel open to client
    sendSystemChannelOpen(channel, clientOpts, chOpts);

    // next part: ChannelBase::systemChannelConnected

    return true;
}

void ChannelListener::recvChannelSystemEvent(const std::string& cmd, const JsonObject & jo) {
    if(cmd == SystemCommand::ClientVariables) {
        return systemClientVariablesEvent(jo);
    }

    if(cmd == SystemCommand::KeyboardChange) {
        return systemKeyboardChangeEvent(jo);
    }

    if(cmd == SystemCommand::CursorFailed) {
        return systemCursorFailedEvent(jo);
    }

    if(cmd == SystemCommand::TransferFiles) {
        return systemTransferFilesEvent(jo);
    }

    Application::error("{}: {}", NS_FuncNameV, "unknown cmd");
    throw std::invalid_argument(NS_FuncNameS);
}

#endif

// Remote2Local
Channel::Remote2Local::Remote2Local(CID cid, int flags) : id(cid) {
    zlib = static_cast<uint32_t>(OptsFlags::ZLibCompression) & flags;
}

Channel::Remote2Local::~Remote2Local() {
    Application::info("{}: channel: {}, receive: {} byte, transfer: {} byte, error: {}", "Remote2Local", id, transfer1, transfer2, error);
}

bool Channel::Remote2Local::isEmpty(void) const {
    const std::scoped_lock guard{lockQueue};
    return queueBufs.empty();
}

void Channel::Remote2Local::pushData(std::vector<uint8_t> && buf) {
    const std::scoped_lock guard{lockQueue};
    queueBufs.emplace_back(std::move(buf));
}

std::vector<uint8_t> Channel::Remote2Local::popData(void) {
    const std::scoped_lock guard{lockQueue};

    if(queueBufs.empty())
        return {};

    auto queueSz = queueBufs.size();

    if(queueSz > 10) {
        // descrease delay
        if(delay > std::chrono::milliseconds{10}) {
            Application::warning("{}: id: {}, queue large: {}, change delay to {}ms", NS_FuncNameV, id, queueSz, delay.count());
            delay -= std::chrono::milliseconds{10};
        } else {
            Application::warning("{}: id: {}, queue large: {}, fixme: `{}'", NS_FuncNameV, id, queueSz, "fixme: remote decrease speed");
        }
    }

    auto buf = std::move(queueBufs.front());
    queueBufs.pop_front();

    return buf;
}

bool Channel::Remote2Local::writeData(void) {
    auto buf = popData();

    if(buf.empty()) {
        return true;
    }

    transfer1 += buf.size();

    if(zlib) {
        buf = ZLib::inflate(buf);
        // Application::debug(DebugType::Channels, "{}: inflate, size1: {}, size2: {}", NS_FuncNameV, buf.size(), buf2.size());
    }

    size_t writesz = 0;

    while(writesz < buf.size()) {
        ssize_t real = writeDataFrom(buf.data() + writesz, buf.size() - writesz);

        if(0 < real) {
            writesz += real;
            transfer2 += real;
            continue;
        }

        if(EAGAIN == errno || EINTR == errno) {
            continue;
        }

        error = errno;
        return false;
    }

    return true;
}

void Channel::Remote2Local::setSpeed(const Channel::Speed & speed) {
    switch(speed) {
        case Speed::VerySlow:
            delay = std::chrono::milliseconds(200);
            break;

        case Speed::Slow:
            delay = std::chrono::milliseconds(100);
            break;

        case Speed::Medium:
            delay = std::chrono::milliseconds(70);
            break;

        case Speed::Fast:
            delay = std::chrono::milliseconds(40);
            break;

        case Speed::UltraFast:
            delay = std::chrono::milliseconds(20);
            break;

        case Speed::Ultra5:
            delay = std::chrono::milliseconds(5);
            break;
    }
}

/// Remote2Local_FD
Channel::Remote2Local_FD::Remote2Local_FD(CID cid, int fd0, bool close, int flags)
    : Remote2Local(cid, flags), fd(fd0), needClose(close) {
}

Channel::Remote2Local_FD::~Remote2Local_FD() {
    if(needClose && 0 <= fd) {
        close(fd);
    }
}

ssize_t Channel::Remote2Local_FD::writeDataFrom(const void* buf, size_t len) {
    return ::write(fd, buf, len);
}

// Local2Remote
Channel::Local2Remote::Local2Remote(CID cid, int flags) : id(cid) {
    zlib = static_cast<uint32_t>(OptsFlags::ZLibCompression) & flags;
    buf.reserve(UINT16_MAX);
}

Channel::Local2Remote::~Local2Remote() {
    Application::info("{}: channel: {}, receive: {} byte, transfer: {} byte, error: {}", "Local2Remote", id, transfer1, transfer2, error);
}

bool Channel::Local2Remote::readData(void) {
    size_t dtsz = 0;

    try {
        if(hasInput()) {
            dtsz = hasData();
        }
    } catch(const std::exception & err) {
        error = errno;
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return false;
    }

    if(0 == dtsz) {
        buf.clear();
        return true;
    }

    buf.resize(std::min(dtsz, blocksz));
    ssize_t real = readDataTo(buf.data(), buf.size());

    if(0 < real) {
        buf.resize(real);
        transfer1 += real;

        if(zlib) {
            buf = ZLib::deflate(buf, Z_BEST_SPEED + 2);
            transfer2 += buf.size();
            // Application::debug(DebugType::Channels, "{}: deflate, size1: {}, size2: {}", NS_FuncNameV, buf2.size(), buf.size());
        } else {
            transfer2 += real;
        }

        return true;
    }

    // eof
    if(0 == real) {
        return false;
    }

    if(EAGAIN == errno || EINTR == errno) {
        buf.clear();
        return true;
    }

    error = errno;
    return false;
}

void Channel::Local2Remote::setSpeed(const Channel::Speed & speed) {
    switch(speed) {
        // ~10k/sec
        case Speed::VerySlow:
            blocksz = 8192;
            delay = std::chrono::milliseconds(200);
            break;

        // ~40k/sec
        case Speed::Slow:
            blocksz = 16384;
            delay = std::chrono::milliseconds(100);
            break;

        // ~80k/sec
        case Speed::Medium:
            blocksz = 16384;
            delay = std::chrono::milliseconds(70);
            break;

        // ~800k/sec
        case Speed::Fast:
            blocksz = 32768;
            delay = std::chrono::milliseconds(40);
            break;

        // ~1600k/sec
        case Speed::UltraFast:
            delay = std::chrono::milliseconds(20);
            blocksz = 32768;
            break;

        case Speed::Ultra5:
            delay = std::chrono::milliseconds(5);
            blocksz = 32768;
            break;
    }
}

/// Local2Remote_FD
Channel::Local2Remote_FD::Local2Remote_FD(CID cid, int fd0, bool close, int flags)
    : Local2Remote(cid, flags), fd(fd0), needClose(close) {
}

Channel::Local2Remote_FD::~Local2Remote_FD() {
    if(needClose && 0 <= fd) {
        close(fd);
    }
}

bool Channel::Local2Remote_FD::hasInput(void) const {
    return NetworkStream::hasInput(fd);
}

size_t Channel::Local2Remote_FD::hasData(void) const {
    return NetworkStream::hasData(fd);
}

ssize_t Channel::Local2Remote_FD::readDataTo(void* buf, size_t len) {
    return ::read(fd, buf, len);
}

/// ConnectorBase
Channel::ConnectorBase::ConnectorBase(CID ch, const ConnectorMode & mod, const Opts & chOpts, ChannelBase & srv)
    : cid(ch), owner(& srv), mode(mod), flags(chOpts.flags) {
    owner->sendSystemChannelConnected(ch, chOpts.flags, true);
}

bool Channel::ConnectorBase::isAllowSessionFor(bool user) const {
    return (flags & static_cast<uint32_t>(OptsFlags::AllowLoginSession)) ? ! user : user;
}

bool Channel::ConnectorBase::isRunning(void) const {
    return loopRunning;
}

bool Channel::ConnectorBase::isRemoteConnected(void) const {
    return remoteConnected;
}

void Channel::ConnectorBase::setRunning(bool f) {
    loopRunning = f;
}

void Channel::ConnectorBase::setRemoteConnected(bool f) {
    remoteConnected = f;
}

void Channel::Connector::loopWriter(ConnectorBase* cn, Remote2Local* st) {
    bool error = false;
    auto owner = cn->getOwner();

    if(! owner) {
        Application::error("{}: id: {}, {} failed", NS_FuncNameV, st->cid(), "owner");
        return;
    }

    while(cn->isRunning()) {
        if(st->isEmpty()) {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        }

        if(! st->writeData()) {
            error = true;
            cn->setRunning(false);
        }
    }

    if(error) {
        owner->sendSystemChannelError(st->cid(), st->getError(), std::string(NS_FuncNameV).append(": ").append(strerror(st->getError())));

        Application::error("{}: id: {}, error: {}", NS_FuncNameV, st->cid(), strerror(st->getError()));
    } else {
        // all data write
        while(! st->isEmpty()) {
            if(! st->writeData()) {
                break;
            }
        }
    }

    // read/write priority send
    if(! cn->isMode(ConnectorMode::ReadWrite) || cn->isMode(ConnectorMode::WriteOnly)) {
        owner->sendSystemChannelClose(st->cid());
    }
}

void Channel::Connector::loopReader(ConnectorBase* cn, Local2Remote* st) {
    bool error = false;
    auto owner = cn->getOwner();

    if(! owner) {
        Application::error("{}: id: {}, {} failed", NS_FuncNameV, st->cid(), "owner");
        return;
    }

    while(cn->isRunning()) {
        if(! st->readData()) {
            error = true;
            cn->setRunning(false);
        }

        if(st->getBuf().empty()) {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        } else {
            auto & buf = st->getBuf();
            owner->sendLtsmChannelData(st->cid(), std::move(buf));
        }
    }

    if(error) {
        owner->sendSystemChannelError(st->cid(), st->getError(), std::string(NS_FuncNameV).append(": ").append(strerror(st->getError())));
        Application::error("{}: id: {}, error: {}", NS_FuncNameV, st->cid(), strerror(st->getError()));
    }

    // read/write priority send
    if(cn->isMode(ConnectorMode::ReadWrite) || cn->isMode(ConnectorMode::ReadOnly)) {
        owner->sendSystemChannelClose(st->cid());
    }
}

/// ConnectorFD_R
Channel::ConnectorFD_R::ConnectorFD_R(CID ch, int fd0, bool close, const Opts & chOpts, ChannelBase & srv)
    : ConnectorBase(ch, ConnectorMode::ReadOnly, chOpts, srv) {
    // start threads
    setRunning(true);

    localRemote = std::make_unique<Local2Remote_FD>(ch, fd0, close, chOpts.flags);
    localRemote->setSpeed(chOpts.speed);

    if(localRemote) {
        thr = std::thread(Connector::loopReader, this, localRemote.get());
    }
}

Channel::ConnectorFD_R::~ConnectorFD_R() {
    setRunning(false);

    if(thr.joinable()) {
        thr.join();
    }
}

int Channel::ConnectorFD_R::error(void) const {
    return localRemote ? localRemote->getError() : 0;
}

void Channel::ConnectorFD_R::setSpeed(const Channel::Speed & speed) {
    if(localRemote) {
        localRemote->setSpeed(speed);
    }
}

/// ConnectorFD_W
Channel::ConnectorFD_W::ConnectorFD_W(CID ch, int fd0, bool close, const Opts & chOpts, ChannelBase & srv)
    : ConnectorBase(ch, ConnectorMode::WriteOnly, chOpts, srv) {
    // start threads
    setRunning(true);

    remoteLocal = std::make_unique<Remote2Local_FD>(ch, fd0, close, chOpts.flags);
    remoteLocal->setSpeed(chOpts.speed);

    if(remoteLocal) {
        thw = std::thread(Connector::loopWriter, this, remoteLocal.get());
    }
}

Channel::ConnectorFD_W::~ConnectorFD_W() {
    setRunning(false);

    if(thw.joinable()) {
        thw.join();
    }
}

int Channel::ConnectorFD_W::error(void) const {
    return remoteLocal ? remoteLocal->getError() : 0;
}

void Channel::ConnectorFD_W::setSpeed(const Channel::Speed & speed) {
    if(remoteLocal) {
        remoteLocal->setSpeed(speed);
    }
}

void Channel::ConnectorFD_W::pushData(std::vector<uint8_t> && buf) {
    if(! buf.empty() && remoteLocal) {
        remoteLocal->pushData(std::move(buf));
    }
}

/// ConnectorFD_RW
Channel::ConnectorFD_RW::ConnectorFD_RW(CID ch, int fd0, const Opts & chOpts, ChannelBase & srv)
    : ConnectorBase(ch, ConnectorMode::ReadWrite, chOpts, srv) {
    // start threads
    setRunning(true);

    localRemote = std::make_unique<Local2Remote_FD>(ch, fd0, true, chOpts.flags);
    localRemote->setSpeed(chOpts.speed);

    remoteLocal = std::make_unique<Remote2Local_FD>(ch, fd0, true, chOpts.flags);
    remoteLocal->setSpeed(chOpts.speed);

    if(localRemote) {
        thr = std::thread(Connector::loopReader, this, localRemote.get());
    }

    if(remoteLocal) {
        thw = std::thread(Connector::loopWriter, this, remoteLocal.get());
    }
}

Channel::ConnectorFD_RW::~ConnectorFD_RW() {
    setRunning(false);

    if(thr.joinable()) {
        thr.join();
    }

    if(thw.joinable()) {
        thw.join();
    }
}

int Channel::ConnectorFD_RW::error(void) const {
    int err1 = remoteLocal ? remoteLocal->getError() : 0;
    int err2 = localRemote ? localRemote->getError() : 0;

    return err1 ? err1 : err2;
}

void Channel::ConnectorFD_RW::setSpeed(const Channel::Speed & speed) {
    if(localRemote) {
        localRemote->setSpeed(speed);
    }

    if(remoteLocal) {
        remoteLocal->setSpeed(speed);
    }
}

void Channel::ConnectorFD_RW::pushData(std::vector<uint8_t> && buf) {
    if(! buf.empty() && remoteLocal) {
        remoteLocal->pushData(std::move(buf));
    }
}

// ConnectorCMD_W
Channel::ConnectorCMD_W::ConnectorCMD_W(CID channel, FILE* ptr, const Opts & chOpts, ChannelBase & owner)
    : ConnectorFD_W(channel, fileno(ptr), false, chOpts, owner), fcmd(ptr) {
}

Channel::ConnectorCMD_W::~ConnectorCMD_W() {
    if(fcmd) {
        pclose(fcmd);
    }
}

// ConnectorCMD_R
Channel::ConnectorCMD_R::ConnectorCMD_R(CID channel, FILE* ptr, const Opts & chOpts, ChannelBase & owner)
    : ConnectorFD_R(channel, fileno(ptr), false, chOpts, owner), fcmd(ptr) {
}

Channel::ConnectorCMD_R::~ConnectorCMD_R() {
    if(fcmd) {
        pclose(fcmd);
    }
}

#ifdef __UNIX__
/// createUnixConnector
Channel::ConnectorBasePtr
Channel::createUnixConnector(CID channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    std::error_code err;

    if(! std::filesystem::is_socket(path, err)) {
        Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
            NS_FuncNameV, "is_socket", err.value(), err.message(), path.string());
        throw channel_error(NS_FuncNameS);
    }

    Application::info("{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    int fd = UnixSocket::connect(path);

    if(0 > fd) {
        Application::error("{}: {}, id: {}, path: `{}'", NS_FuncNameV, "unix failed", channel, path);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

Channel::ConnectorBasePtr
Channel::createUnixConnector(CID channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, sock: {}, mode: {}", NS_FuncNameV, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "unix failed", channel);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, sock, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, sock, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, sock, true, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

/// createTcpConnector
Channel::ConnectorBasePtr
Channel::createTcpConnector(CID channel, const std::string & ipaddr, int port, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, addr: `{}', port: {}, mode: {}", NS_FuncNameV, channel, ipaddr, port, Channel::Connector::modeString(mode));

    int fd = TCPSocket::connect(ipaddr, port);

    if(0 > fd) {
        Application::error("{}: {}, id: {}, addr: `{}', port: {}", NS_FuncNameV, "socket failed", channel, ipaddr, port);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

Channel::ConnectorBasePtr
Channel::createTcpConnector(CID channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, sock: {}, mode: {}", NS_FuncNameV, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "socket failed", channel);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, sock, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, sock, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, sock, true, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}
#endif

/// createFileConnector
Channel::ConnectorBasePtr
Channel::createFileConnector(CID channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::Unknown) {
        Application::error("{}: {}, mode: {}", NS_FuncNameV, "file mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncNameS);
    }

    std::error_code err;

    if(mode == ConnectorMode::ReadOnly &&
       ! std::filesystem::exists(path, err)) {
        Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
            NS_FuncNameV, "exists", err.value(), err.message(), path.string());
        throw channel_error(NS_FuncNameS);
    }

    int fd = 0;

    if(mode == ConnectorMode::ReadOnly) {
#ifdef __WIN32__
        auto cpath = Tools::wstring2string(path);
        fd = open(cpath.c_str(), O_RDONLY);
#else
        fd = open(path.c_str(), O_RDONLY);
#endif
    } else if(mode == ConnectorMode::WriteOnly) {
        int flags = O_WRONLY;

        if(std::filesystem::exists(path, err)) {
            flags |= O_APPEND;
            Application::warning("{}: {}, path: `{}'", NS_FuncNameV, "file exists switch mode to append", path);
        } else {
            flags |= O_CREAT | O_EXCL;
        }

#ifdef __WIN32__
        auto cpath = Tools::wstring2string(path);
        fd = open(cpath.c_str(), flags, S_IRUSR | S_IWUSR | S_IRGRP);
#else
        fd = open(path.c_str(), flags, S_IRUSR | S_IWUSR | S_IRGRP);
#endif
    }

    if(0 > fd) {
        Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "open file", strerror(errno), errno);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

/// createCommandConnector
Channel::ConnectorBasePtr
Channel::createCommandConnector(CID channel, const std::string & runcmd, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, run cmd: `{}', mode: {}", NS_FuncNameV, channel, runcmd, Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::Unknown) {
        Application::error("{}: {}, mode: {}", NS_FuncNameV, "cmd mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncNameS);
    }

    auto list = Tools::split(runcmd, 0x20);

    if(list.empty()) {
        Application::error("{}: {}", NS_FuncNameV, "cmd empty");
        throw channel_error(NS_FuncNameS);
    }

    std::error_code err;

    if(! std::filesystem::exists(list.front(), err)) {
        Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
            NS_FuncNameV, "exists", err.value(), err.message(), list.front());
        throw channel_error(NS_FuncNameS);
    }

    FILE* fcmd = nullptr;

    if(std::filesystem::is_symlink(list.front(), err)) {
        auto cmd = Tools::resolveSymLink(list.front());
        list.pop_front();
#ifdef __WIN32__
        list.push_front(cmd.string());
#else
        list.push_front(cmd.native());
#endif
        auto runcmd2 = Tools::join(list, " ");

        fcmd = popen(runcmd2.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    } else if(std::filesystem::is_regular_file(list.front(), err)) {
        fcmd = popen(runcmd.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    }

    if(! fcmd) {
        Application::error("{}: {} failed, error: {}, code: {}", NS_FuncNameV, "open cmd", strerror(errno), errno);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorCMD_R>(channel, fcmd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorCMD_W>(channel, fcmd, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

#ifdef __UNIX__
/// Listener
Channel::Listener::Listener(int fd, const UrlMode & serverOpts, const UrlMode & clientOpts, const Channel::Opts & ch, ChannelListener & sender)
    : sopts(serverOpts), copts(clientOpts), owner(& sender), chopts(ch), srvfd(fd) {
    loopRunning = true;
    th = std::thread(loopAccept, this);
}

Channel::Listener::~Listener() {
    loopRunning = false;

    if(th.joinable()) {
        th.join();
    }

    if(0 <= srvfd) {
        close(srvfd);
    }

    if(isUnix()) {
        try {
            if(std::filesystem::exists(sopts.content()) && std::filesystem::is_socket(sopts.content())) {
                std::filesystem::remove(sopts.content());
            }
        } catch(const std::filesystem::filesystem_error &) {
        }
    }
}

bool Channel::Listener::isRunning(void) const {
    return loopRunning;
}

void Channel::Listener::setRunning(bool f) {
    loopRunning = f;
}

void Channel::Listener::loopAccept(Listener* st) {
    while(st->loopRunning) {
        bool input = false;

        try {
            input = NetworkStream::hasInput(st->srvfd);
        } catch(const std::exception & err) {
            st->loopRunning = false;

            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        }

        if(input) {
            auto sock = st->isUnix() ?
                        UnixSocket::accept(st->srvfd) : TCPSocket::accept(st->srvfd);

            if(sock < 0) {
                st->loopRunning = false;
            } else if(! st->owner->createChannelAcceptFd(st->copts, sock, st->sopts, st->chopts)) {
                close(sock);
            }
        } else {
            std::this_thread::sleep_for(250ms);
        }
    }
}

std::unique_ptr<Channel::Listener>
Channel::createUnixListener(const UrlMode & serverOpts, size_t listen,
                                  const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelListener & sender) {
    auto & path = serverOpts.content();
    std::error_code err;

    if(std::filesystem::exists(path, err) &&
       ! std::filesystem::is_socket(path, err)) {
        Application::error("{}: {}, path: `{}'", NS_FuncNameV, "not socket", path);
        throw channel_error(NS_FuncNameS);
    }

    int srvfd = UnixSocket::listen(path, listen);

    if(0 > srvfd) {
        Application::error("{}: {}, path: `{}'", NS_FuncNameV, "unix failed", path);
        throw channel_error(NS_FuncNameS);
    }

    return std::make_unique<Listener>(srvfd, serverOpts, clientOpts, chOpts, sender);
}

std::unique_ptr<Channel::Listener>
Channel::createTcpListener(const UrlMode & serverOpts, size_t listen,
                                 const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelListener & sender) {
    auto [ ipaddr, port ] = Connector::parseAddrPort(serverOpts.content());

    if(0 >= port) {
        Application::error("{}: {}, url: `{}'", NS_FuncNameV, "socket format", serverOpts.content());
        throw channel_error(NS_FuncNameS);
    }

    // hardcore server listen
    if(sender.serverSide()) {
        ipaddr = "127.0.0.1";
    }

    int srvfd = TCPSocket::listen(ipaddr, port, listen);

    if(0 > srvfd) {
        Application::error("{}: {}, ipaddr: {}, port: {}", NS_FuncNameV, "socket failed", ipaddr, port);
        throw channel_error(NS_FuncNameS);
    }

    return std::make_unique<Listener>(srvfd, serverOpts, clientOpts, chOpts, sender);
}
#endif
