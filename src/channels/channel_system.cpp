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
    std::pair<std::string, uint16_t> parseAddrPort(const std::string &);
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
        case ConnectorType::Fd:
            return "fd";

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

std::pair<std::string, uint16_t>
Channel::Connector::parseAddrPort(const std::string & addrPort) {
    Application::debug(DebugType::Channels, "{}: addr: `{}'", NS_FuncNameV, addrPort);

    // format url
    // url1: hostname:port
    // url2: xx.xx.xx.xx:port
    auto list = Tools::split(addrPort, ':');

    if(2 != list.size()) {
        Application::error("{}: invalid format, address:: `{}'", NS_FuncNameV, addrPort);
        throw channel_error(NS_FuncNameS);
    }

    auto addr = list.front().empty() ? "127.0.0.1" : list.front();
    int port = std::stoi(list.back());

    if(port < 1 || UINT16_MAX < port) {
        Application::error("{}: {}, port: {}", NS_FuncNameV, "invalid port", port);
        throw channel_error(NS_FuncNameS);
    }

    return std::make_pair(addr, static_cast<uint16_t>(port));
}

/// ChannelBase
size_t ChannelBase::countValidChannels(void) const {
    const std::scoped_lock guard{lockch};
    return std::count_if(channels_.begin(), channels_.end(), [](auto & ptr) {
        return !! ptr;
    });
}

Channel::ConnectorBase* ChannelBase::findChannel(CID channel) {
    const std::scoped_lock guard{lockch};

    if(auto& ptr = channels_[channel]) {
        return ptr.get();
    }

    return nullptr;
}

void ChannelBase::emplaceChannel(CID channel, Channel::ConnectorBasePtr&& ptr) {
    const std::scoped_lock guard{lockch};
    channels_[channel] = std::move(ptr);
}

void ChannelBase::destroyChannel(CID channel) {
    const std::scoped_lock guard{this->lockch};

    if(auto& ptr = channels_[channel]) {
        ptr.reset();
        Application::info("{}: {}, id: {}", NS_FuncNameV, "channel removed", channel);
    } else {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not running", channel);
    }
}

void ChannelBase::shutdownChannels(void) {
    const std::scoped_lock guard{lockch};

    for(auto & ptr : channels_) {
        if(ptr) {
            ptr.reset();
        }
    }
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

    if(! isAllowChannel(channelConn.get())) {
        Application::error("{}: ltsm channel disable", NS_FuncNameV);
        throw std::invalid_argument(NS_FuncNameS);
    }

    if(! channelConn->isRunning()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not running", channel);
        throw std::invalid_argument(NS_FuncNameS);
    }

    if(! channelConn->isWriteAllow()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel write disable", channel);
        throw std::invalid_argument(NS_FuncNameS);
    }

    channelConn->pushData(std::move(buf));
}

void ChannelBase::systemChannelCloseEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    Application::info("{}: channel: {}", NS_FuncNameV, channel);
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

void ChannelBase::createChannelAudio(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_AUDIO)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createClientAudioConnector(channel, url, mode, chOpts, *this));
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported audio", url);
    throw channel_error(NS_FuncNameS);
#endif
}

void ChannelBase::createChannelFuse(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_FUSE)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createClientFuseConnector(channel, url, mode, chOpts, *this));
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported fuse", url);
    throw channel_error(NS_FuncNameS);
#endif
}

void ChannelBase::createChannelPcsc(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_WITH_PCSC)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createClientPcscConnector(channel, url, mode, chOpts, *this));
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported pcsc", url);
    throw channel_error(NS_FuncNameS);
#endif
}

void ChannelBase::createChannelFd(CID channel, int fd, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    Application::debug(DebugType::Channels, "{}: id: {}, fd: {}, mode: {}", NS_FuncNameV, channel, fd, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createFdConnector(channel, fd, mode, chOpts, *this));
}

#ifdef __UNIX__
void ChannelBase::createChannelUnix(CID channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Unix, path.string(), mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", path);
        throw channel_error(NS_FuncNameS);
    }

    Application::debug(DebugType::Channels, "{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createUnixConnector(channel, path, mode, chOpts, *this));
}

void ChannelBase::createChannelSocket(CID channel, std::pair<std::string, uint16_t> ipAddrPort, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Socket, ipAddrPort.first, mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", ipAddrPort.first);
        throw channel_error(NS_FuncNameS);
    }

    if(serverSide() && ! startsWith(ipAddrPort.first, "127.")) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "server side allow socket only for localhost", channel);
        throw channel_error(NS_FuncNameS);
    }

    Application::debug(DebugType::Channels, "{}: id: {}, addr: {}, port: {}, mode: {}", NS_FuncNameV, channel, ipAddrPort.first, ipAddrPort.second, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createTcpConnector(channel, ipAddrPort.first, ipAddrPort.second, mode, chOpts, *this));
}

#endif // __UNIX__

void ChannelBase::createChannelPkcs11(CID channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#if defined(LTSM_CLIENT) && defined(LTSM_PKCS11_AUTH)
    Application::debug(DebugType::Channels, "{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createClientPkcs11Connector(channel, url, mode, chOpts, *this));
#else
    Application::error("{}: {}, url: `{}'", NS_FuncNameV, "unsupported pkcs11", url);
    throw channel_error(NS_FuncNameS);
#endif
}

void ChannelBase::createChannelFile(CID channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
#ifdef __WIN32__
    if(! allowCreateChannel(Channel::ConnectorType::File, path.string(), mode))
#else
    if(! allowCreateChannel(Channel::ConnectorType::File, path.string(), mode))
#endif
    {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", path);
        throw channel_error(NS_FuncNameS);
    }

    Application::debug(DebugType::Channels, "{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createFileConnector(channel, path, mode, chOpts, *this));
}

void ChannelBase::createChannelCommand(CID channel, const std::string & runcmd, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Command, runcmd, mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", runcmd);
        throw channel_error(NS_FuncNameS);
    }

    Application::debug(DebugType::Channels, "{}: id: {}, run cmd: `{}', mode: {}", NS_FuncNameV, channel, runcmd, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createCommandConnector(channel, runcmd, mode, chOpts, *this));
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
    Application::info("{}: id: {}", NS_FuncNameV, channel);
    sendLtsmChannelData(ChannelTypeSystem, JsonObjectStream().push("cmd", SystemCommand::ChannelClose).push("id", channel).flush());
}

void ChannelBase::sendSystemChannelConnected(CID channel, int flags, int error) {
    sendLtsmChannelData(ChannelTypeSystem, JsonObjectStream().
                        push("cmd", SystemCommand::ChannelConnected).
                        push("flags", flags).
                        push("error", error).
                        push("id", channel).flush());
}

void ChannelBase::recvLtsmProto(CID channel, std::vector<uint8_t> && buf) {
    Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

    if(isChannelDebug(channel)) {
        auto str = Tools::hexString(buf, 2);
        Application::trace(DebugType::Channels, "{}: id: {}, size: {}, content: [{}]",
                           NS_FuncNameV, channel, buf.size(), str);
    }

    recvLtsmEvent(channel, std::move(buf));
}

void ChannelBase::setChannelDebug(CID channel, bool debug) {
    if(debug) {
        channel_debug_ = channel;
    } else if(channel_debug_ == channel) {
        channel_debug_ = -1;
    }
}

Channel::ConnectorStatus ChannelBase::channelStatus(CID channel) const {
    if(auto& ptr = channels_[channel]) {
        return ptr->connectorStatus();
    }

    return Channel::ConnectorStatus::Unknown;
}

void ChannelBase::setChannelStatus(CID channel, const Channel::ConnectorStatus& st) {
    if(auto& ptr = channels_[channel]) {
        ptr->setConnectorStatus(st);
    } else {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not running", channel);
        throw channel_error(NS_FuncNameS);
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

    auto replyError = [this,channel,flags]() {
        sendSystemChannelConnected(channel, flags, true /* replyError */);
    };

    if(channel <= ChannelTypeSystem || channel >= ChannelTypeReserved) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", channel);
        return replyError();
    }

    const auto mode = Channel::connectorMode(smode);

    if(mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "unknown channel mode", channel);
        return replyError();
    }

    if(findChannel(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel busy", channel);
        return replyError();
    }

    Application::info("{}: id: {}, type: {}, mode: {}, speed: {}, flags: {:#010x}", NS_FuncNameV, channel, stype, smode, sspeed, flags);
    bool success = true;

    Channel::ConnectorType type = Channel::connectorType(stype);
    Channel::Opts chopts{ Channel::connectorSpeed(sspeed), flags };

    try {
        switch(type) {
            case Channel::ConnectorType::File:
                createChannelFile(channel, jo.getString("path"), mode, chopts);
                break;
            case Channel::ConnectorType::Audio:
                createChannelAudio(channel, jo.getString("audio"), mode, chopts);
                break;
            case Channel::ConnectorType::Fuse:
                createChannelFuse(channel, jo.getString("fuse"), mode, chopts);
                break;
            case Channel::ConnectorType::Pcsc:
                createChannelPcsc(channel, jo.getString("pcsc"), mode, chopts);
                break;

#ifdef __UNIX__
            case Channel::ConnectorType::Unix:
                createChannelUnix(channel, jo.getString("path"), mode, chopts);
                break;
            case Channel::ConnectorType::Socket:
                createChannelSocket(channel, std::make_pair(jo.getString("ipaddr"), jo.getInteger("port")), mode, chopts);
                break;
#endif
#ifdef LTSM_PKCS11_AUTH
            case Channel::ConnectorType::Pkcs11:
                createChannelPkcs11(channel, jo.getString("pkcs11"), mode, chopts);
                break;
#endif
            case Channel::ConnectorType::Command:
                createChannelCommand(channel, jo.getString("runcmd"), mode, chopts);
                break;
            default:
                Application::error("{}: {} `{}', id: {}", NS_FuncNameV, "unknown channel type", stype, channel);
                success = false;
        }
    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        success = false;
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        success = false;
    }

    if(success) {
        // set local connected
        setChannelConnected(channel);
        sendSystemChannelConnected(channel, flags, false /* replyError */);
    } else {
        sendSystemChannelConnected(channel, flags, true /* replyError */);
    }
}

void ChannelClient::systemChannelListenEvent(const JsonObject & jo) {
}

void ChannelClient::systemChannelConnectedEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    int error = jo.getInteger("error");
    int flags = jo.getInteger("flags", 0);

    if(error) {
        Application::error("{}: error: {}, id: {}", NS_FuncNameV, error, channel);
        throw channel_error(NS_FuncNameS);
    }

    if(channelConnected(channel)) {
        Application::info("{}: channel: {}, flags: {:08x}", NS_FuncNameV, channel, flags);
        setChannelRunning(channel);
    } else {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel not connected", channel);
        throw channel_error(NS_FuncNameS);
    }
}

#ifdef __UNIX__
// ChannelListener
asio::local::stream_protocol::endpoint ChannelListener::createUnixEndpoint(const Channel::UrlMode & serverOpts) const {
    auto & path = serverOpts.content();
    std::error_code err;

    if(std::filesystem::exists(path, err)) {
        if(! std::filesystem::is_socket(path, err)) {
            Application::error("{}: {}, path: `{}'", NS_FuncNameV, "not socket", path);
            throw channel_error(NS_FuncNameS);
        }

        std::filesystem::remove(path, err);
    }

    return asio::local::stream_protocol::endpoint{path};
}

asio::ip::tcp::endpoint ChannelListener::createTcpEndpoint(const Channel::UrlMode & serverOpts) const {
    auto [ ipaddr, port ] = Channel::Connector::parseAddrPort(serverOpts.content());
    return asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), port};
}

void ChannelListener::plannedEmplaceSpawn(Channel::Planned && job) {
    Application::info("{}: accept client, url: `{}', mode: {}, fd: {}",
        NS_FuncNameV, job.clientOpts.url, Channel::Connector::modeString(job.clientOpts.mode), job.serverFd);

    asio::co_spawn(chan_strand(), plannedEmplaceAwait(std::move(job)), [func=NS_FuncNameV](std::exception_ptr ptr) {
        try {
            if(ptr) {
                std::rethrow_exception(ptr);
            }
        } catch(const system::system_error& err) {
            auto ec = err.code();
            Application::error("{}: system error: {}, code: {}", func, ec.message(), ec.value());
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", func, err.what());
        }
    });
}

asio::awaitable<void> ChannelListener::createListenerAwait(Channel::UrlMode clientOpts,
        Channel::UrlMode serverOpts, Channel::Opts channelOpts, int listenLimit) {

    if(auto it = listeners_.find(serverOpts.url); it != listeners_.end()) {
        Application::warning("{}: {}, server url: {}", NS_FuncNameV, "listen present", serverOpts.url);
        co_return;
    }

    const bool is_unix = (serverOpts.type() == Channel::ConnectorType::Unix);

    if(serverOpts.type() != Channel::ConnectorType::Socket && ! is_unix) {
        Application::warning("{}: {}, server url: {}", NS_FuncNameV, "invalid socket type", serverOpts.url);
        co_return;
    }

    if(clientOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "client");
        co_return;
    }
 
    if(serverOpts.mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: unknown {} mode", NS_FuncNameV, "server");
        co_return;
    }
 
    if(clientOpts.type() == Channel::ConnectorType::Unknown) {
        Application::error("{}: unknown client url: `{}'", NS_FuncNameV, clientOpts.url);
        co_return;
    }

    if(serverOpts.mode == clientOpts.mode &&
       (serverOpts.mode == Channel::ConnectorMode::ReadOnly || serverOpts.mode == Channel::ConnectorMode::WriteOnly)) {
        Application::error("{}: incorrect modes pair (wo,wo) or (ro,ro)", NS_FuncNameV);
        co_return;
    }

    auto ex = co_await asio::this_coro::executor;
    auto job = Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = channelOpts, .serverFd = 0 };

    auto sig = std::make_unique<asio::cancellation_signal>();
    const auto& slot = sig->slot();
    listeners_[serverOpts.url.data()] = std::move(sig);

    try {
        Application::info("{}: server url: {}, client url: {}", NS_FuncNameV, serverOpts.url, clientOpts.url);

        if(is_unix) {
            auto endpoint = createUnixEndpoint(serverOpts);
            using protocol = asio::local::stream_protocol;
            protocol::acceptor acceptor{ex};

            acceptor.open(endpoint.protocol());
            acceptor.bind(endpoint);
            if(0 < listenLimit) {
                acceptor.listen(listenLimit);
            }
            Application::debug(DebugType::Channels, "listen path: {}", NS_FuncNameV, endpoint.path());
            // wait accept
            co_await asio::co_spawn(ex,
                acceptorAcceptAwait(std::move(acceptor), job),
                boost::asio::bind_cancellation_slot(slot, boost::asio::use_awaitable));
        } else {
            auto endpoint = createTcpEndpoint(serverOpts);
            using protocol = asio::ip::tcp;
            protocol::acceptor acceptor{ex};

            acceptor.open(endpoint.protocol());
            acceptor.set_option(protocol::socket::reuse_address(true));
            acceptor.bind(endpoint);
            if(0 < listenLimit) {
                acceptor.listen(listenLimit);
            }
            Application::debug(DebugType::Channels, "listen port: {}", NS_FuncNameV, endpoint.port());
            // wait accept
            co_await asio::co_spawn(ex,
                acceptorAcceptAwait(std::move(acceptor), job),
                boost::asio::bind_cancellation_slot(slot, boost::asio::use_awaitable));
        }
    } catch(const system::system_error& err) {
        if(auto ec = err.code(); ec != asio::error::operation_aborted) {
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        }
    } catch(const std::exception & err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    listeners_.erase(serverOpts.url);
    co_return;
}

asio::awaitable<void> ChannelListener::destroyListenerAwait(std::string url) {
    if(auto it = listeners_.find(url); it != listeners_.end()) {
        if(it->second) {
            it->second->emit(asio::cancellation_type::terminal);
        }
        Application::info("{}: server url: {}", NS_FuncNameV, url);
        listeners_.erase(it);
    }
    co_return;
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

void ChannelListener::exceptionHandler(std::exception_ptr ptr) {
    if(ptr) {
        std::rethrow_exception(ptr);
    }
}

void ChannelListener::systemChannelConnectedEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    int error = jo.getInteger("error");
    int flags = jo.getInteger("flags", 0);

    asio::co_spawn(chan_strand(), systemChannelConnectedAwait(channel, flags, error), [this](std::exception_ptr ptr) {
        exceptionHandler(ptr);
    });
}

asio::awaitable<void> ChannelListener::systemChannelConnectedAwait(CID channel, int flags, int error) {
    // find planed
    auto it = std::ranges::find_if(channels_planned_, [ = ](auto & st) {
        return st.channel == channel;
    });

    if(it == channels_planned_.end()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "job planning not found", channel);
        throw channel_error(NS_FuncNameS);
    }

    // move planed to running
    auto job = std::move(*it);
    channels_planned_.erase(it);
    planned_counts_.fetch_sub(1);

    job.chOpts.flags = flags;

    auto jobFailed = [&job]() {
        if(0 <= job.serverFd) {
            shutdown(job.serverFd, SHUT_RDWR);
            close(job.serverFd);
            job.serverFd = -1;
        }
    };

    auto replyError = [this,channel,flags]() {
        sendSystemChannelConnected(channel, flags, true /* replyError */);
    };

    if(error) {
        Application::error("{}: error: {}, id: {}", NS_FuncNameV, error, channel);
        jobFailed();
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    if(channel <= ChannelTypeSystem || channel >= ChannelTypeReserved) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", channel);
        jobFailed();
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    if(findChannel(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel busy", channel);
        jobFailed();
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    bool success = false;

    try {
        co_await createChannelAwait(channel, job);
        success = true;
    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    if(! success) {
        jobFailed();
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    Application::info("{}: channel: {}, flags: {:08x}", NS_FuncNameV, channel, flags);
    // set local connected
    setChannelConnected(channel);
    sendSystemChannelConnected(channel, flags, false /* replyError */);
    setChannelRunning(channel);

    co_return;
}

uint32_t ChannelListener::countFreeChannels(void) const {
    const auto channels_valid = countValidChannels();
    const auto used = 2 + channels_valid + planned_counts_.load();

    if(used > ChannelTypeLast) {
        Application::error("{}: used channel count is large, count: {}", NS_FuncNameV, used);
        throw channel_error(NS_FuncNameS);
    }

    return ChannelTypeLast - used;
}

bool ChannelListener::createChannel(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts) {
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

    auto job = Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = chOpts };
    asio::co_spawn(chan_strand(), plannedEmplaceAwait(std::move(job)), [this](std::exception_ptr ptr) {
        exceptionHandler(ptr);
    });

    // next part: ChannelBase::systemChannelConnected

    return true;
}

asio::awaitable<void> ChannelListener::plannedEmplaceAwait(Channel::Planned job) {
    // find free channel
    CID channel = 1;

    auto findPlanned = [this](CID id) {
        return std::ranges::any_of(channels_planned_, [ = ](auto & st) {
            return st.channel == id;
        });
    };

    for(; channel < ChannelTypeReserved; ++channel) {
        if(! findChannel(channel) && ! findPlanned(channel)) {
            break;
        }
    }

    if(channel == ChannelTypeReserved) {
        Application::error("{}: all channels busy", NS_FuncNameV);
        throw channel_error(NS_FuncNameS);
    }

    job.channel = channel;

    channels_planned_.emplace_back(std::move(job));
    planned_counts_.fetch_add(1);

    const auto & back = channels_planned_.back();

    // send channel open to client
    sendSystemChannelOpen(back.channel, back.clientOpts, back.chOpts);
    co_return;
}

asio::awaitable<void> ChannelListener::createChannelAwait(CID channel, const Channel::Planned& job) {

    Application::info("{}: {}, id: {}, client url: `{}', server url: `{}'",
                      NS_FuncNameV, "found planned job", channel, job.clientOpts.url, job.serverOpts.url);

    switch(job.serverOpts.type()) {
        case Channel::ConnectorType::Fd:
            createChannelFd(channel, job.serverFd, job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::Unix:
            createChannelUnix(channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::Socket:
            createChannelSocket(channel, Channel::Connector::parseAddrPort(job.serverOpts.content()), job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::File:
            createChannelFile(channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::Command:
            createChannelCommand(channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
            break;

        default:
            Application::error("{}: {}, id: {}", NS_FuncNameV, "channel type not implemented", channel);
            throw channel_error(NS_FuncNameS);
    }

    co_return;
}

bool ChannelListener::isAllowChannel(const Channel::ConnectorBase* conn) const {
    if(conn->isAllowSessionFor(true) != isUserSession()) {
        Application::error("{}: ltsm channel disable for session: `{}'", NS_FuncNameV, (isUserSession() ? "user" : "login"));
        return false;
    }

    return true;
}

#endif

/// ConnectorBase
bool Channel::ConnectorBase::isAllowSessionFor(bool user) const {
    return (flags_ & static_cast<uint32_t>(OptsFlags::AllowLoginSession)) ? ! user : user;
}

std::pair<std::chrono::milliseconds,uint32_t> Channel::ConnectorBase::speedInfo(void) const {
    switch(speed_) {
        // ~10k/sec
        default:
            return std::make_pair(std::chrono::milliseconds(200), 8192);

        // ~40k/sec
        case Speed::Slow:
            return std::make_pair(std::chrono::milliseconds(100), 16384);

        // ~80k/sec
        case Speed::Medium:
            return std::make_pair(std::chrono::milliseconds(70), 16384);

        // ~800k/sec
        case Speed::Fast:
            return std::make_pair(std::chrono::milliseconds(40), 32768);

        // ~1600k/sec
        case Speed::UltraFast:
            return std::make_pair(std::chrono::milliseconds(20), 32768);

        case Speed::Ultra5:
            return std::make_pair(std::chrono::milliseconds(5), 32768);
    }
}

/// ConnectorFD_R
Channel::ConnectorFD_R::ConnectorFD_R(CID ch, int fd, const Opts & opts, ChannelBase & srv)
    : ConnectorBase(ch, ConnectorMode::ReadOnly, opts, srv),
        sd_{srv.chan_strand(), fd}, tm_delay_{srv.chan_strand()} {
    // read loop
    asio::co_spawn(srv.chan_strand(), readLoopAwait(),
        boost::asio::bind_cancellation_slot(read_cancel_.slot(), [this](std::exception_ptr ptr) {
            loop_running_.exchange(false);
        })
    );
}

Channel::ConnectorFD_R::~ConnectorFD_R() {
    sd_.cancel();
    tm_delay_.cancel();
    read_cancel_.emit(asio::cancellation_type::terminal);
    setConnectorStatus(ConnectorStatus::Error);
    // wait loop ended
    if(loop_running_.load()) {
        Application::info("{}: wait ended", NS_FuncNameV);
        while(loop_running_.load()) {
            std::this_thread::yield();
        }
    }
}

asio::awaitable<void> Channel::ConnectorFD_R::readLoopAwait(void) {
    loop_running_.exchange(true);

    try {
        auto info = speedInfo();
        std::vector<uint8_t> buf(info.second);

        for(;;) {
            if(connectorStatus() == Channel::ConnectorStatus::Error) {
                Application::error("{}: status error", NS_FuncNameV);
                co_return;
            }

            tm_delay_.expires_after(info.first);
            co_await tm_delay_.async_wait(asio::use_awaitable);

            if(connectorStatus() != Channel::ConnectorStatus::Running) {
                continue;
            }

            // read local
            auto transferred = co_await sd_.async_read_some(asio::buffer(buf), asio::use_awaitable);
            buf.resize(transferred);

            if(isZlib()) {
                buf = ZLib::deflate(buf, Z_BEST_SPEED + 2);
            }

            // send to remote
            co_await getOwner()->sendLtsmChannelAwait(channel(), buf);
        }
    } catch(const system::system_error& err) {
        if(auto ec = err.code(); ec != asio::error::operation_aborted) {
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            getOwner()->sendSystemChannelError(channel(), ec.value(), std::string(NS_FuncNameV).append(": ").append(ec.message()));
        }
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        getOwner()->sendSystemChannelError(channel(), -1, std::string(NS_FuncNameV).append(": ").append(err.what()));
    }

    setConnectorStatus(ConnectorStatus::Error);
    getOwner()->sendSystemChannelClose(channel());

    co_return;
}

/// ConnectorFD_W
Channel::ConnectorFD_W::ConnectorFD_W(CID ch, int fd, const Opts & opts, ChannelBase & srv)
    : ConnectorBase(ch, ConnectorMode::WriteOnly, opts, srv), sd_{srv.chan_strand(), fd} {
}

Channel::ConnectorFD_W::~ConnectorFD_W() {
    sd_.cancel();
    // wait all write process
    if(auto num = write_process_.load()) {
        Application::info("{}: wait ended, process: {}", NS_FuncNameV, num);
        while(write_process_.load()) {
            std::this_thread::yield();
        }
    }
}

boost::asio::awaitable<void> Channel::ConnectorFD_W::writeDataAwait(std::vector<uint8_t> buf) {
    bool error = false;

    try {
        if(isZlib()) {
            auto buf2 = ZLib::inflate(buf);
            co_await asio::async_write(sd_, asio::const_buffer(buf2.data(), buf2.size()), boost::asio::transfer_all(), asio::use_awaitable);
        } else {
            co_await asio::async_write(sd_, asio::const_buffer(buf.data(), buf.size()), boost::asio::transfer_all(), asio::use_awaitable);
        }
    } catch(const system::system_error& err) {
        if(auto ec = err.code(); ec != asio::error::operation_aborted) {
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            getOwner()->sendSystemChannelError(channel(), ec.value(), std::string(NS_FuncNameV).append(": ").append(ec.message()));
        }
        error = true;
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        getOwner()->sendSystemChannelError(channel(), -1, std::string(NS_FuncNameV).append(": ").append(err.what()));
        error = true;
    }

    if(error) {
        setConnectorStatus(ConnectorStatus::Error);
        getOwner()->sendSystemChannelClose(channel());
    }

    co_return;
}

void Channel::ConnectorFD_W::pushData(std::vector<uint8_t> && buf) {
    if(10 < write_process_.load()) {
        Application::error("{}: overload", NS_FuncNameV);
        throw channel_error(NS_FuncNameS);
    }

    write_process_.fetch_add(1);

    asio::co_spawn(getOwner()->chan_strand(), writeDataAwait(std::move(buf)),
        [this](std::exception_ptr ptr) {
            // complete token
            write_process_.fetch_sub(1);
        }
    );
}

/// ConnectorFD_RW
Channel::ConnectorFD_RW::ConnectorFD_RW(CID ch, int fd, const Opts & opts, ChannelBase & srv)
    : ConnectorFD_R(ch, fd, opts, srv), fdw_(ch, dup(fd), opts, srv) {
    // overwrite mode
    setConnectorMode(ConnectorMode::ReadWrite);
}

void Channel::ConnectorFD_RW::pushData(std::vector<uint8_t> && buf) {
    fdw_.pushData(std::move(buf));
}

// ConnectorCMD_W
Channel::ConnectorCMD_W::ConnectorCMD_W(CID channel, FILE* file, const Opts & opts, ChannelBase & owner)
    : ConnectorFD_W(channel, fileno(file), opts, owner), fcmd(file) {
}

Channel::ConnectorCMD_W::~ConnectorCMD_W() {
    if(fcmd) {
        pclose(fcmd);
    }
}

// ConnectorCMD_R
Channel::ConnectorCMD_R::ConnectorCMD_R(CID channel, FILE* file, const Opts & opts, ChannelBase & owner)
    : ConnectorFD_R(channel, fileno(file), opts, owner), fcmd(file) {
}

Channel::ConnectorCMD_R::~ConnectorCMD_R() {
    if(fcmd) {
        pclose(fcmd);
    }
}

#ifdef __UNIX__
namespace Asio {
template<typename Executor>
int unixConnect(const std::filesystem::path & path, Executor ex) {
    asio::local::stream_protocol::socket sock{ex};
    asio::local::stream_protocol::endpoint endpoint{path.c_str()};

    sock.connect(endpoint);
    return sock.release();
}

template<typename Executor>
int tcpConnect(const std::string & addr, uint16_t port, Executor ex) {
    asio::ip::tcp::socket sock{ex};
    asio::ip::tcp::endpoint endpoint(
        asio::ip::make_address_v4(addr), port);

    sock.connect(endpoint);
    return sock.release();
}
}

/// createUnixConnector
Channel::ConnectorBasePtr
Channel::createUnixConnector(CID channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    std::error_code err;
    if(! std::filesystem::is_socket(path, err)) {
        Application::error("{}: {} failed, code: {}, error: {}, path: `{}'",
                           NS_FuncNameV, "is_socket", err.value(), err.message(), path.string());
        throw channel_error(NS_FuncNameS);
    }

    int fd = Asio::unixConnect(path, sender.chan_strand());
    Application::info("{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    if(0 > fd) {
        Application::error("{}: {}, id: {}, addr: `{}'", NS_FuncNameV, "socket failed", channel, path);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

/// createTcpConnector
Channel::ConnectorBasePtr
Channel::createTcpConnector(CID channel, const std::string & ipaddr, uint16_t port, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, addr: `{}', port: {}, mode: {}", NS_FuncNameV, channel, ipaddr, port, Channel::Connector::modeString(mode));

    int fd = Asio::tcpConnect(ipaddr, port, sender.chan_strand());

    if(0 > fd) {
        Application::error("{}: {}, id: {}, addr: `{}', port: {}", NS_FuncNameV, "socket failed", channel, ipaddr, port);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

#endif

Channel::ConnectorBasePtr
Channel::createFdConnector(CID channel, int fd, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, fd: {}, mode: {}", NS_FuncNameV, channel, fd, Channel::Connector::modeString(mode));

    if(0 > fd) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "fd failed", channel);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadWrite) {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly) {
        return std::make_unique<ConnectorFD_R>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

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
        return std::make_unique<ConnectorFD_R>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        return std::make_unique<ConnectorFD_W>(channel, fd, chOpts, sender);
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
        list.push_front(cmd.string());
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
