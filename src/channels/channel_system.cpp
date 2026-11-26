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
#include <chrono>
#include <cstring>
#include <variant>
#include <utility>
#include <charconv>
#include <exception>
#include <filesystem>

#ifdef __WIN32__
#include <winsock2.h>
#endif

#if BOOST_VERSION >= 108700
#include <boost/process/popen.hpp>
#include <boost/process/environment.hpp>
#else
#include <boost/process/v2/popen.hpp>
#include <boost/process/v2/environment.hpp>
#endif

#include "channel_system.h"
#include "ltsm_application.h"
#include "ltsm_zlib.h"
#include "ltsm_json_wrapper.h"

using namespace std::chrono_literals;
using namespace boost;
using namespace LTSM;

namespace LTSM::Channel::Connector {
    std::pair<std::string, uint16_t> parseAddrPort(std::string_view);
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
        case ConnectorType::AcceptSock:
            return "accept";

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

Channel::Planned::Planned(const UrlMode& srv, const UrlMode& cli, const Opts& opt)
    : serverOpts(srv), clientOpts(cli), chOpts(opt) {
}

Channel::Planned::Planned(const UrlMode& srv, const UrlMode& cli, const Opts& opt, generic_socket&& sock)
    : serverOpts(srv), clientOpts(cli), chOpts(opt), acceptSock{std::move(sock)} {
    serverOpts.updateType(Channel::ConnectorType::AcceptSock);
}

Channel::Planned::~Planned() {
    acceptSockClose();
}

Channel::Planned::Planned(Planned&&) noexcept = default;
Channel::Planned& Channel::Planned::operator=(Planned&&) noexcept = default;

void Channel::Planned::acceptSockClose(void) {
    if(acceptSock && acceptSock->is_open()) {
        system::error_code ec;
        acceptSock->shutdown(generic_socket::shutdown_both, ec);
        acceptSock->close(ec);
        acceptSock.reset();
    }
}

std::pair<Channel::ConnectorType, std::string>
Channel::parseUrl(std::string_view url) {
    if(url.empty()) {
        return {ConnectorType::Unknown, ""};
    }

    struct PrefixMap {
        std::string_view prefix;
        ConnectorType type;
    };

    static constexpr std::array prefixes = {
        PrefixMap{"file://",    ConnectorType::File},
        PrefixMap{"unix://",    ConnectorType::Unix},
        PrefixMap{"sock://",    ConnectorType::Socket},
        PrefixMap{"socket://",  ConnectorType::Socket},
        PrefixMap{"cmd://",     ConnectorType::Command},
        PrefixMap{"command://", ConnectorType::Command},
        PrefixMap{"fuse://",    ConnectorType::Fuse},
        PrefixMap{"audio://",   ConnectorType::Audio},
        PrefixMap{"pcsc://",    ConnectorType::Pcsc},
        PrefixMap{"pkcs11://",  ConnectorType::Pkcs11}
    };

    for(const auto& entry : prefixes) {
        if(url.starts_with(entry.prefix)) {
            return {entry.type, view2string(url.substr(entry.prefix.size()))};
        }
    }

    return {ConnectorType::Unknown, view2string(url)};
}

std::pair<std::string, uint16_t>
Channel::Connector::parseAddrPort(std::string_view addrPort) {
    Application::debug(DebugType::Channels, "{}: address: `{}'", NS_FuncNameV, addrPort);

    // format url
    // url1: hostname:port
    // url2: xx.xx.xx.xx:port
    // IPv6 format: [::1]:port

    const auto colonPos = addrPort.rfind(':');

    if(colonPos == std::string::npos) {
        Application::error("{}: invalid format (missing port), address: `{}'", NS_FuncNameV, addrPort);
        throw channel_error(NS_FuncNameS);
    }

    std::string_view hostPart{addrPort.data(), colonPos};
    std::string_view portPart{addrPort.data() + colonPos + 1, addrPort.size() - colonPos - 1};

    // ipv6
    if(hostPart.starts_with('[') && hostPart.ends_with(']') && hostPart.size() > 2) {
        hostPart.remove_prefix(1);
        hostPart.remove_suffix(1);
    } else if(hostPart.find(':') != std::string_view::npos) {
        Application::error("{}: invalid IPv6 format, address: `{}'", NS_FuncNameV, addrPort);
        throw channel_error(NS_FuncNameS);
    }

    uint32_t portVal = 0;
    const auto [ptr, ec] = std::from_chars(portPart.data(), portPart.data() + portPart.size(), portVal);

    if(ec != std::errc{} || ptr != (portPart.data() + portPart.size()) || portVal < 1 || portVal > UINT16_MAX) {
        Application::error("{}: invalid port in address: `{}'", NS_FuncNameV, addrPort);
        throw channel_error(NS_FuncNameS);
    }

    std::string addr = hostPart.empty() ? "127.0.0.1" : view2string(hostPart);
    return {std::move(addr), static_cast<uint16_t>(portVal)};
}

/// ChannelBase
size_t ChannelBase::countValidSlots(void) const {
    return std::count_if(channels_.begin(), channels_.end(), [](auto & ptr) {
        return !! ptr;
    });
}

bool ChannelBase::isFreeSlot(CID channel) {
    if(auto& ptr = channels_[channel]) {
        return false;
    }

    return true;
}

void ChannelBase::emplaceChannel(CID channel, Channel::ConnectorBasePtr&& ptr) {
    channels_[channel] = std::move(ptr);
}

void ChannelBase::destroyChannel(CID channel, bool sendEvent) {
    if(auto& ptr = channels_[channel]) {
        if(sendEvent) {
            ptr->setSendEventClose();
        }
        ptr->cancel();
        ptr.reset();
        Application::info("{}: id: {}, {}", NS_FuncNameV, channel, "channel removed");
    } else {
        Application::debug(DebugType::Channels, "{}: id: {}, {}", NS_FuncNameV, channel, "channel not running");
    }
}

void ChannelBase::shutdownChannels(void) {
    for(auto & ptr : channels_) {
        if(ptr) {
            ptr->cancel();
            ptr.reset();
        }
    }
}

void ChannelBase::recvLtsmChannelEvent(CID channel, std::vector<uint8_t> && buf) {
    Application::debug(DebugType::Channels, "{}: id: {}, data size: {}", NS_FuncNameV, channel, buf.size());

    if(isChannelDebug(channel)) {
        auto str = Tools::hexString(buf, 2);
        Application::trace(DebugType::Channels, "{}: id: {}, size: {}, content: [{}]",
                           NS_FuncNameV, channel, buf.size(), str);
    }

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

    channelConn->updateWriteStat(buf.size());
    channelConn->pushData(std::move(buf));
}

void ChannelBase::sendChannelDataSuccess(CID channel, size_t val) const {
    if(auto& channelConn = channels_[channel]) {
        channelConn->updateReadStat(val);
    }
}

void ChannelBase::systemChannelCloseEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");

    if(isValidChannelId(channel)) {
        Application::info("{}: channel: {}", NS_FuncNameV, channel);
        destroyChannel(channel, false /* send event */);
    } else {
        Application::error("{}: invalid channel id: {}", NS_FuncNameV, channel);
    }
}

void ChannelBase::systemChannelErrorEvent(const JsonObject & jo) {
    int channel = jo.getInteger("id");
    int code = jo.getInteger("code");
    auto err = jo.getString("error");

    if(isValidChannelId(channel)) {
        Application::info("{}: channel: {}, errno: {}, error: `{}'", NS_FuncNameV, channel, code, err);
        channelErrorNotify(channel, code, err);
    } else {
        Application::error("{}: invalid channel id: {}", NS_FuncNameV, channel);
    }
}

bool ChannelBase::sendSystemTransferFiles(std::forward_list<std::string>&& files) const {
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
        return false;
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

    return true;
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

void ChannelBase::createChannelSocket(CID channel, AsyncGenericSocket&& sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    Application::debug(DebugType::Channels, "{}: id: {}, mode: {}", NS_FuncNameV, channel, Channel::Connector::modeString(mode));
    emplaceChannel(channel, Channel::createSocketConnector(channel, std::move(sock), mode, chOpts, *this));
}

#ifdef __UNIX__
void ChannelBase::createChannelUnixPath(CID channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Unix, path.string(), mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", path);
        throw channel_error(NS_FuncNameS);
    }

    try {
        auto ex = chan_strand();
        asio::local::stream_protocol::socket sock{ex};
        asio::local::stream_protocol::endpoint endpoint{path.c_str()};
        sock.connect(endpoint);

        Application::info("{}: id: {}, addr: `{}', mode: {}", NS_FuncNameV, channel, path.string(), Channel::Connector::modeString(mode));
        emplaceChannel(channel, Channel::createSocketConnector(channel, AsyncGenericSocket(std::move(sock)), mode, chOpts, *this));
    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        throw channel_error(NS_FuncNameS);
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        throw channel_error(NS_FuncNameS);
    }
}

void ChannelBase::createChannelTcpAddress(CID channel, std::pair<std::string, uint16_t> ipAddrPort, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts) {
    if(! allowCreateChannel(Channel::ConnectorType::Socket, ipAddrPort.first, mode)) {
        Application::error("{}: {}, content: `{}'", NS_FuncNameV, "blocked", ipAddrPort.first);
        throw channel_error(NS_FuncNameS);
    }

    try {
        auto ex = chan_strand();
        asio::ip::tcp::socket sock{ex};

        const auto& addr = ipAddrPort.first;
        const auto& port = ipAddrPort.second;

        if(serverSide()) {
            // server only localhost
            asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);
            sock.connect(endpoint);
        } else {
            asio::ip::tcp::endpoint endpoint(asio::ip::make_address_v4(addr), port);
            sock.connect(endpoint);
        }

        Application::info("{}: id: {}, addr: `{}', port: {}, mode: {}", NS_FuncNameV, channel, addr, port, Channel::Connector::modeString(mode));
        emplaceChannel(channel, Channel::createSocketConnector(channel, AsyncGenericSocket(std::move(sock)), mode, chOpts, *this));
    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        throw channel_error(NS_FuncNameS);
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        throw channel_error(NS_FuncNameS);
    }
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
    if(! allowCreateChannel(Channel::ConnectorType::File, path.string(), mode))
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
    auto list = Tools::split(runcmd, 0x20);
    emplaceChannel(channel, Channel::createCommandConnector(channel,
        list.front(), {std::next(list.begin()), list.end()}, mode, chOpts, *this));
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

    if(!ChannelBase::isValidChannelId(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", channel);
        return replyError();
    }

    const auto mode = Channel::connectorMode(smode);

    if(mode == Channel::ConnectorMode::Unknown) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "unknown channel mode", channel);
        return replyError();
    }

    if(! isFreeSlot(channel)) {
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
                createChannelUnixPath(channel, jo.getString("path"), mode, chopts);
                break;
            case Channel::ConnectorType::Socket:
                createChannelTcpAddress(channel, std::make_pair(jo.getString("ipaddr"), jo.getInteger("port")), mode, chopts);
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

    if(!isValidChannelId(channel)) {
        Application::error("{}: invalid channel id: {}", NS_FuncNameV, channel);
        throw channel_error(NS_FuncNameS);
    }

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
    Application::info("{}: accept client, url: `{}', mode: {}",
        NS_FuncNameV, job.clientOpts.url, Channel::Connector::modeString(job.clientOpts.mode));

    asio::post(chan_strand(), [this,job=std::move(job)]() mutable {
        try {
            plannedEmplace(std::move(job));
        } catch(const system::system_error& err) {
            auto ec = err.code();
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        } catch(const std::exception& err) {
            Application::error("{}: exception: {}", NS_FuncNameV, err.what());
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
    auto sig = std::make_unique<asio::cancellation_signal>();
    const auto& slot = sig->slot();
    listeners_[serverOpts.url] = std::move(sig);

    using EndpointVariant = std::variant<asio::ip::tcp::endpoint, asio::local::stream_protocol::endpoint>;
    const EndpointVariant endpoint = is_unix ?
        EndpointVariant{createUnixEndpoint(serverOpts)} : EndpointVariant{createTcpEndpoint(serverOpts)};

    try {
        Application::info("{}: server url: {}, client url: {}", NS_FuncNameV, serverOpts.url, clientOpts.url);
        co_await std::visit([&](auto&& ep) -> asio::awaitable<void> {
            using Protocol = typename std::decay_t<decltype(ep)>::protocol_type;
            typename Protocol::acceptor acceptor{ex};

            acceptor.open(ep.protocol());
            if constexpr (std::is_same_v<Protocol, asio::ip::tcp>) {
                acceptor.set_option(typename Protocol::socket::reuse_address(true));
            }
            acceptor.bind(ep);
            acceptor.listen(0 < listenLimit ? listenLimit : 0);

            Application::debug(DebugType::Channels, "{}: listen: {}", NS_FuncNameV, serverOpts.content());

            co_await asio::co_spawn(ex,
                acceptorAcceptAwait(std::move(acceptor), serverOpts, clientOpts, channelOpts),
                asio::bind_cancellation_slot(slot, asio::use_awaitable));
        }, endpoint);
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

void ChannelListener::destroyListener(std::string url) {
    if(auto it = listeners_.find(url); it != listeners_.end()) {
        if(it->second) {
            it->second->emit(asio::cancellation_type::terminal);
        }
        Application::info("{}: server url: {}", NS_FuncNameV, url);
        listeners_.erase(it);
    }
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

    if(!isValidChannelId(channel)) {
        Application::error("{}: invalid channel id: {}", NS_FuncNameV, channel);
        throw channel_error(NS_FuncNameS);
    }

    systemChannelConnected(channel, flags, error);
}

void ChannelListener::systemChannelConnected(CID channel, int flags, int error) {
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
    job.chOpts.flags = flags;

    channels_planned_.erase(it);
    planned_counts_.fetch_sub(1);

    auto replyError = [this,channel,flags]() {
        sendSystemChannelConnected(channel, flags, true /* replyError */);
    };

    if(error) {
        Application::error("{}: error: {}, id: {}", NS_FuncNameV, error, channel);
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    if(!ChannelBase::isValidChannelId(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel incorrect", channel);
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    if(! isFreeSlot(channel)) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "channel busy", channel);
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    bool success = false;

    try {
        createChannelPlanned(channel, std::move(job));
        success = true;
    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    if(! success) {
        replyError();
        throw channel_error(NS_FuncNameS);
    }

    Application::info("{}: channel: {}, flags: {:08x}", NS_FuncNameV, channel, flags);
    // set local connected
    setChannelConnected(channel);
    sendSystemChannelConnected(channel, flags, false /* replyError */);
    setChannelRunning(channel);
}

uint32_t ChannelListener::countFreeChannels(void) const {
    const auto channels_valid = countValidSlots();
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

    auto job = Channel::Planned(serverOpts, clientOpts, chOpts);
    plannedEmplaceSpawn(std::move(job));

    // next part: ChannelBase::systemChannelConnected

    return true;
}

void ChannelListener::plannedEmplace(Channel::Planned job) {
    // find free channel
    CID channel = 1;

    auto findPlanned = [this](CID id) {
        return std::ranges::any_of(channels_planned_, [ = ](auto & st) {
            return st.channel == id;
        });
    };

    for(; channel < ChannelTypeReserved; ++channel) {
        if(isFreeSlot(channel) && ! findPlanned(channel)) {
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
}

void ChannelListener::createChannelPlanned(CID channel, Channel::Planned&& job) {

    Application::info("{}: {}, id: {}, client url: `{}', server url: `{}'",
                      NS_FuncNameV, "found planned job", channel, job.clientOpts.url, job.serverOpts.url);

    switch(job.serverOpts.type()) {
        case Channel::ConnectorType::AcceptSock:
            if(!job.acceptSock) {
                Application::error("{}: {} failed, id: {}", NS_FuncNameV, "accept sock", channel);
                throw channel_error(NS_FuncNameS);
            }
            createChannelSocket(channel, AsyncGenericSocket(std::move(job.acceptSock.value())), job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::Unix:
            createChannelUnixPath(channel, job.serverOpts.content(), job.serverOpts.mode, job.chOpts);
            break;

        case Channel::ConnectorType::Socket:
            createChannelTcpAddress(channel, Channel::Connector::parseAddrPort(job.serverOpts.content()), job.serverOpts.mode, job.chOpts);
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
Channel::ConnectorBase::ConnectorBase(CID cid, const ConnectorMode & mod, const Opts & opts, ChannelBase & srv)
    : owner_(& srv), mode_(mod), flags_(opts.flags), speed_{opts.speed}, cid_(cid) {
}

Channel::ConnectorBase::~ConnectorBase() {
    Application::debug(DebugType::Channels, "channel[{}] write {} bytes, read {} bytes",
        channel(), static_cast<uint64_t>(wstat_), static_cast<uint64_t>(rstat_));
}

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

/// AsyncConnectorBase
void Channel::AsyncConnectorBase::readErrorHandler(std::exception_ptr ptr) {
    if(! ptr) {
        return;
    }

    try {
        std::rethrow_exception(ptr);
    } catch(const system::system_error& err) {
        auto ec = err.code();
        if(ec == asio::error::operation_aborted || ec == asio::error::eof) {
            if(ec == asio::error::eof) {
                Application::warning("{}: channel: {} stream eof", NS_FuncNameV, channel());
            }
            if(streamOpen() && isSendEventClose()) {
                getOwner()->sendSystemChannelClose(channel());
            }
        } else {
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            getOwner()->sendSystemChannelError(channel(), ec.value(), std::string(NS_FuncNameV).append(": ").append(ec.message()));
        }
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        getOwner()->sendSystemChannelError(channel(), -1, std::string(NS_FuncNameV).append(": ").append(err.what()));
    }

    setConnectorStatus(ConnectorStatus::Error);
}

void Channel::AsyncConnectorBase::writeErrorHandler(std::exception_ptr ptr) {
    if(! ptr) {
        return;
    }

    try {
        std::rethrow_exception(ptr);
    } catch(const system::system_error& err) {
        auto ec = err.code();
        if(ec == asio::error::operation_aborted) {
            if(streamOpen() && isSendEventClose()) {
                getOwner()->sendSystemChannelClose(channel());
            }
        } else {
            Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            getOwner()->sendSystemChannelError(channel(), ec.value(), std::string(NS_FuncNameV).append(": ").append(ec.message()));
        }
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        getOwner()->sendSystemChannelError(channel(), -1, std::string(NS_FuncNameV).append(": ").append(err.what()));
    }

    setConnectorStatus(ConnectorStatus::Error);
}

asio::awaitable<void> Channel::AsyncConnectorBase::readStreamHandler(std::vector<uint8_t> buf) {
    if(isZlib()) {
        buf = ZLib::deflate(buf, Z_BEST_SPEED + 2);
    }

    const auto transferred = buf.size();
    // send to remote
    getOwner()->sendLtsmChannelData(channel(), std::move(buf));

    // speed limits apply
    auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - speed_tp;
    const auto info = speedInfo();

    if(info.first <= elapsed) {
        speed_tp = now;
        speed_sz = transferred;
    } else {
        speed_sz += transferred;

        if(speed_sz >= info.second) {
            auto ex = co_await asio::this_coro::executor;
            asio::steady_timer tm_delay{ex};

            tm_delay.expires_after(info.first - elapsed);
            co_await tm_delay.async_wait(asio::use_awaitable);

            speed_tp = std::chrono::steady_clock::now();
            speed_sz = 0;
        }
    }
    
    co_return;
}

void Channel::AsyncConnectorBase::writeStreamHandler(std::vector<uint8_t> & buf) {
    if(isZlib()) {
        buf = ZLib::inflate(buf);
    }
}

void Channel::AsyncConnectorBase::writeJobsOverhead(uint32_t jobs) {
    if(TransferChannelSize < jobs) {
        Application::warning("{}: transfer channel overload: {}", NS_FuncNameV, jobs);
    }
}

Channel::ConnectorBasePtr
Channel::createSocketConnector(CID channel, AsyncGenericSocket&& sd, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    if(! sd.isOpen()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "sock failed", channel);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadOnly) {
        using AsyncChannelStream = AsyncStreamConnectorRO<AsyncGenericSocket>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        using AsyncChannelStream = AsyncStreamConnectorWO<AsyncGenericSocket>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadWrite) {
        using AsyncChannelStream = AsyncStreamConnectorRW<AsyncGenericSocket>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

Channel::ConnectorBasePtr
Channel::createStreamConnector(CID channel, AsyncStreamDescriptor&& sd, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, mode: {}", NS_FuncNameV, channel, Channel::Connector::modeString(mode));

    if(! sd.isOpen()) {
        Application::error("{}: {}, id: {}", NS_FuncNameV, "stream failed", channel);
        throw channel_error(NS_FuncNameS);
    }

    if(mode == ConnectorMode::ReadOnly) {
        using AsyncChannelStream = AsyncStreamConnectorRO<AsyncStreamDescriptor>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly) {
        using AsyncChannelStream = AsyncStreamConnectorWO<AsyncStreamDescriptor>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadWrite) {
        using AsyncChannelStream = AsyncStreamConnectorRW<AsyncStreamDescriptor>;
        return std::make_shared<AsyncChannelStream>(std::move(sd), channel, mode, chOpts, sender);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

/// createFileConnector
Channel::ConnectorBasePtr
Channel::createFileConnector(CID channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {
    Application::info("{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));

    try {
        auto ex = sender.chan_strand();
        asio::stream_file sf{ex};
        bool fix_perms = false;
        std::error_code err;
        Channel::ConnectorBasePtr ret;

        if(mode == ConnectorMode::ReadOnly) {
            sf.open(path.string(), asio::stream_file::read_only);
            using AsyncChannelStream = AsyncStreamConnectorRO<AsyncStreamFileReadable>;
            ret = std::make_shared<AsyncChannelStream>(AsyncStreamFileReadable(std::move(sf)), channel, mode, chOpts, sender);
        }

        if(mode == ConnectorMode::WriteOnly) {
            const bool exists = std::filesystem::exists(path, err);
            fix_perms = !exists;
            sf.open(path.string(), asio::stream_file::write_only | (exists ? asio::stream_file::append : asio::stream_file::create));
            using AsyncChannelStream = AsyncStreamConnectorWO<AsyncStreamFileWritable>;
            ret = std::make_shared<AsyncChannelStream>(AsyncStreamFileWritable(std::move(sf)), channel, mode, chOpts, sender);
        }

        if(fix_perms) {
            std::filesystem::permissions(path, 
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read,
            std::filesystem::perm_options::replace, err);
        }

        if(ret) {
            Application::info("{}: id: {}, path: `{}', mode: {}", NS_FuncNameV, channel, path, Channel::Connector::modeString(mode));
            return ret;
        }

    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        throw channel_error(NS_FuncNameS);
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        throw channel_error(NS_FuncNameS);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}

void BasicProcessExec::stopping(void) noexcept {
    try {
        if(! proc_.running()) {
            return;
        }
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        return;
    }

    auto ex = proc_.get_executor();
    asio::co_spawn(ex, [proc=std::move(proc_)]() mutable -> asio::awaitable<void> {
        const auto pid = proc.id();
        proc.request_exit();

        auto [ec, exit_code] = co_await proc.async_wait(
            asio::cancel_after(300ms, asio::as_tuple(asio::use_awaitable))
        );

        if(ec) {
            if(ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            } else if(proc.running()) {
                proc.terminate();
            }
        } else {
            Application::debug(DebugType::Channels, "{}: pid: {}, exit code: {}", NS_FuncNameV, pid, exit_code);
        }

        co_return;
    }, asio::detached);
}

/// createCommandConnector
Channel::ConnectorBasePtr
Channel::createCommandConnector(CID channel, const std::string & cmd, const std::vector<std::string> & args, const ConnectorMode & mode, const Opts & chOpts, ChannelBase & sender) {

    auto exe = process::v2::environment::find_executable(cmd);
    if(exe.empty()) {
        Application::error("{}: cmd `{}' not found", NS_FuncNameV, cmd);
        throw channel_error(NS_FuncNameS);
    }

    try {
        auto ex = sender.chan_strand();
        Channel::ConnectorBasePtr ret;

        if(mode == ConnectorMode::ReadOnly) {
            using AsyncChannelStream = AsyncStreamConnectorRO<AsyncReadablePipeCmd>;
            ret = std::make_shared<AsyncChannelStream>(AsyncReadablePipeCmd(ex, exe.string(), args), channel, mode, chOpts, sender);
        }

        if(mode == ConnectorMode::WriteOnly) {
            using AsyncChannelStream = AsyncStreamConnectorWO<AsyncWritablePipeCmd>;
            ret = std::make_shared<AsyncChannelStream>(AsyncWritablePipeCmd(ex, exe.string(), args), channel, mode, chOpts, sender);
        }

        if(ret) {
            Application::info("{}: id: {}, run cmd: `{}', mode: {}", NS_FuncNameV, channel, exe.string(), Channel::Connector::modeString(mode));
            return ret;
        }

    } catch(const system::system_error& err) {
        auto ec = err.code();
        Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
        throw channel_error(NS_FuncNameS);
    } catch(const std::exception& err) {
        Application::error("{}: exception: {}", NS_FuncNameV, err.what());
        throw channel_error(NS_FuncNameS);
    }

    Application::error("{}: id: {}, {} failed", NS_FuncNameV, channel, "mode");
    throw channel_error(NS_FuncNameS);
}
