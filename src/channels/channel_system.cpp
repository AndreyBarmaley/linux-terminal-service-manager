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

#ifdef __LINUX__
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
#include "ltsm_sockets.h"
#include "ltsm_tools.h"
#include "ltsm_librfb.h"
#include "ltsm_json_wrapper.h"

using namespace std::chrono_literals;

namespace LTSM
{
    namespace Channel
    {
        namespace Connector
        {
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);

            std::pair<std::string, int> parseAddrPort(const std::string &);
        }
    }
}

///
std::string LTSM::Channel::createUrl(const ConnectorType & type, std::string_view body)
{
    return std::string(Connector::typeString(type)).append("://").append(body);
}

LTSM::Channel::ConnectorType LTSM::Channel::connectorType(std::string_view str)
{

    for(auto type :
            {
                ConnectorType::Unix, ConnectorType::Socket, ConnectorType::File, ConnectorType::Command,
                ConnectorType::Fuse, ConnectorType::Audio, ConnectorType::Pcsc, ConnectorType::Pkcs11
            })
    {
        if(str == Connector::typeString(type)) { return type; }
    }

    return ConnectorType::Unknown;
}

LTSM::Channel::ConnectorMode LTSM::Channel::connectorMode(std::string_view str)
{
    for(auto mode : { ConnectorMode::ReadOnly, ConnectorMode::ReadWrite, ConnectorMode::WriteOnly })
    {
        if(str == Connector::modeString(mode)) { return mode; }
    }

    return ConnectorMode::Unknown;
}

LTSM::Channel::Speed LTSM::Channel::connectorSpeed(std::string_view str)
{
    for(auto speed : { Speed::VerySlow, Speed::Slow, Speed::Medium, Speed::Fast, Speed::UltraFast })
    {
        if(str == Connector::speedString(speed)) { return speed; }
    }

    return Speed::VerySlow;
}

const char* LTSM::Channel::Connector::typeString(const ConnectorType & type)
{
    switch(type)
    {
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

const char* LTSM::Channel::Connector::modeString(const ConnectorMode & mode)
{
    switch(mode)
    {
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

const char* LTSM::Channel::Connector::speedString(const Speed & speed)
{
    switch(speed)
    {
        case Speed::Slow:
            return "slow";

        case Speed::Medium:
            return "medium";

        case Speed::Fast:
            return "fast";

        case Speed::UltraFast:
            return "ultra";

        default:
            break;
    }

    return "very";
}

std::pair<LTSM::Channel::ConnectorType, std::string>
LTSM::Channel::parseUrl(const std::string & url)
{
    if(0 == url.compare(0, 7, "file://"))
    {
        return std::make_pair(Channel::ConnectorType::File, url.substr(7));
    }

    if(0 == url.compare(0, 7, "unix://"))
    {
        return std::make_pair(Channel::ConnectorType::Unix, url.substr(7));
    }

    if(0 == url.compare(0, 7, "sock://"))
    {
        return std::make_pair(Channel::ConnectorType::Socket, url.substr(7));
    }

    if(0 == url.compare(0, 9, "socket://"))
    {
        return std::make_pair(Channel::ConnectorType::Socket, url.substr(9));
    }

    if(0 == url.compare(0, 6, "cmd://"))
    {
        return std::make_pair(Channel::ConnectorType::Command, url.substr(6));
    }

    if(0 == url.compare(0, 10, "command://"))
    {
        return std::make_pair(Channel::ConnectorType::Command, url.substr(10));
    }

    if(0 == url.compare(0, 7, "fuse://"))
    {
        return std::make_pair(Channel::ConnectorType::Fuse, url.substr(7));
    }

    if(0 == url.compare(0, 8, "audio://"))
    {
        return std::make_pair(Channel::ConnectorType::Audio, url.substr(8));
    }

    if(0 == url.compare(0, 7, "pcsc://"))
    {
        return std::make_pair(Channel::ConnectorType::Pcsc, url.substr(7));
    }

    if(0 == url.compare(0, 9, "pkcs11://"))
    {
        return std::make_pair(Channel::ConnectorType::Pkcs11, url.substr(9));
    }

    return std::make_pair(Channel::ConnectorType::Unknown, url);
}

std::pair<std::string, int>
LTSM::Channel::Connector::parseAddrPort(const std::string & addrPort)
{
    Application::debug(DebugType::Channels, "%s: addr: `%s'", __FUNCTION__, addrPort.c_str());

    // format url
    // url1: hostname:port
    // url2: xx.xx.xx.xx:port
    auto list = Tools::split(addrPort, ':');

    int port = -1;
    std::string addr = "127.0.0.1";

    if(2 != list.size())
    {
        return std::make_pair(addr, port);
    }

    // check addr
    if(auto octets = Tools::split(list.front(), '.'); 4 == octets.size())
    {
        bool error = false;

        try
        {
            // check numbers
            if(std::any_of(octets.begin(), octets.end(), [](auto & val) { return 255 < std::stoi(val); }))
            {
                error = true;
            }
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
            error = true;
        }

        if(error)
        {
            Application::error("%s: %s, addr: `%s'", __FUNCTION__, "incorrect ipaddr", addrPort.c_str());
        }
    }
    else
        // resolv hostname
    {
        std::string addr2 = TCPSocket::resolvHostname(list.front());

        if(addr2.empty())
        {
            Application::error("%s: %s, addr: `%s'", __FUNCTION__, "incorrect hostname", addrPort.c_str());
        }
        else
        {
            addr = addr2;
        }
    }

    // check port
    try
    {
        port = std::stoi(list.back());
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }

    return std::make_pair(addr, port);
}

/// ChannelClient
LTSM::Channel::ConnectorBase* LTSM::ChannelClient::findChannel(uint8_t channel)
{
    const std::scoped_lock guard{lockch};
    auto it = std::find_if(channels.begin(), channels.end(), [=](auto & ptr) { return ptr && ptr->channel() == channel; });

    return it != channels.end() ? (*it).get() : nullptr;
}

LTSM::Channel::Planned* LTSM::ChannelClient::findPlanned(uint8_t channel)
{
    const std::scoped_lock guard{lockpl};
    auto it = std::find_if(channelsPlanned.begin(), channelsPlanned.end(), [=](auto & st) { return st.channel == channel; });

    return it != channelsPlanned.end() ? & (*it) : nullptr;
}

size_t LTSM::ChannelClient::countFreeChannels(void) const
{
    const std::scoped_lock guard{lockch, lockpl};

    size_t used = (2 + channels.size() + channelsPlanned.size());

    if(used > 0xFF)
    {
        Application::error("%s: used channel count is large, count: %u", __FUNCTION__, used);
        throw channel_error(NS_FuncName);
    }

    return 0xFF - used;
}

void LTSM::ChannelClient::sendLtsmChannelData(uint8_t channel, std::string_view str)
{
    sendLtsmChannelData(channel, reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

void LTSM::ChannelClient::sendLtsmChannelData(uint8_t channel, const std::vector<uint8_t> & vec)
{
    sendLtsmChannelData(channel, vec.data(), vec.size());
}

void LTSM::ChannelClient::recvLtsmEvent(uint8_t channel, std::vector<uint8_t> && buf)
{
    if(channel == Channel::Reserved)
    {
        Application::error("%s: reserved channel blocked", __FUNCTION__);
        throw std::invalid_argument(NS_FuncName);
    }

    if(channel == Channel::System)
    {
        recvChannelSystem(buf);
    }
    else
    {
        recvChannelData(channel, std::move(buf));
    }
}

void LTSM::ChannelClient::recvChannelData(uint8_t channel, std::vector<uint8_t> && buf)
{
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", data size: %u", __FUNCTION__, channel, buf.size());

    auto channelConn = findChannel(channel);

    if(! channelConn)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel not found", channel);
        throw std::invalid_argument(NS_FuncName);
    }

#ifndef LTSM_CLIENT

    if((channelConn->isAllowSessionFor(true) && ! isUserSession()) ||
            (channelConn->isAllowSessionFor(false) && isUserSession()))
    {
        Application::error("%s: ltsm channel disable for session: `%s'", __FUNCTION__, (isUserSession() ? "user" : "login"));
        throw std::invalid_argument(NS_FuncName);
    }

#endif

    if(! channelConn->isRemoteConnected())
    {
        Application::error("%s: %s, id: %" PRId8 ", error: %d", __FUNCTION__, "channel not connected", channel, channelConn->error());
        throw std::invalid_argument(NS_FuncName);
    }

    if(! channelConn->isRunning())
    {
        Application::error("%s: %s, id: %" PRId8 ", error: %d", __FUNCTION__, "channel not running", channel, channelConn->error());
        throw std::invalid_argument(NS_FuncName);
    }

    channelConn->pushData(std::move(buf));
}

void LTSM::ChannelClient::systemChannelOpen(const JsonObject & jo)
{
    int channel = jo.getInteger("id");
    auto stype = jo.getString("type");
    auto smode = jo.getString("mode");
    auto sspeed = jo.getString("speed");
    int flags = jo.getInteger("flags", 0);
    bool replyError = false;

    Application::info("%s: id: %" PRId8 ", type: %s, mode: %s, speed: %s, flags: 0x%08" PRIx32, __FUNCTION__, channel, stype.c_str(), smode.c_str(), sspeed.c_str(), flags);

    if(! isUserSession())
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "not user session", channel);
        replyError = true;
    }

    if(channel <= Channel::System || channel >= Channel::Reserved)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel incorrect", channel);
        replyError = true;
    }

    Channel::ConnectorMode mode = Channel::connectorMode(smode);

    if(mode == Channel::ConnectorMode::Unknown)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "unknown channel mode", channel);
        replyError = true;
    }

    if(findChannel(channel))
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel busy", channel);
        replyError = true;
    }

    if(! replyError)
    {
        Channel::ConnectorType type = Channel::connectorType(stype);
        Channel::Opts chopts{ Channel::connectorSpeed(sspeed), flags};

        if(type == Channel::ConnectorType::File)
        {
            replyError = ! createChannelFile(channel, jo.getString("path"), mode, chopts);
        }
        else if(type == Channel::ConnectorType::Audio)
        {
            replyError = ! createChannelClientAudio(channel, jo.getString("audio"), mode, chopts);
        }
        else if(type == Channel::ConnectorType::Fuse)
        {
            replyError = ! createChannelClientFuse(channel, jo.getString("fuse"), mode, chopts);
        }
        else if(type == Channel::ConnectorType::Pcsc)
        {
            replyError = ! createChannelClientPcsc(channel, jo.getString("pcsc"), mode, chopts);
        }
#ifdef __LINUX__
        else if(type == Channel::ConnectorType::Unix)
        {
            replyError = ! createChannelUnix(channel, jo.getString("path"), mode, chopts);
        }
        else if(type == Channel::ConnectorType::Socket)
        {
            replyError = ! createChannelSocket(channel, std::make_pair(jo.getString("ipaddr"), jo.getInteger("port")), mode, chopts);
        }
        else if(type == Channel::ConnectorType::Pkcs11)
        {
            replyError = ! createChannelClientPkcs11(channel, jo.getString("pkcs11"), mode, chopts);
        }
#endif
        else if(type == Channel::ConnectorType::Command)
        {
            replyError = ! createChannelCommand(channel, jo.getString("runcmd"), mode, chopts);
        }
        else
        {
            Application::error("%s: %s `%s', id: %" PRId8, __FUNCTION__, "unknown channel type", stype.c_str(), channel);
            replyError = true;
        }
    }

    if(replyError)
    {
        sendSystemChannelConnected(channel, flags, false);
    }
}

bool LTSM::ChannelClient::systemChannelConnected(const JsonObject & jo)
{
    int channel = jo.getInteger("id");
    bool error = jo.getBoolean("error");
    int flags = jo.getInteger("flags", 0);

    // move planed to running
    const std::scoped_lock guard{lockpl};
    auto it = std::find_if(channelsPlanned.begin(), channelsPlanned.end(), [=](auto & st) { return st.channel == channel; });

    if(it != channelsPlanned.end())
    {
        auto job = std::move(*it);
        channelsPlanned.erase(it);

        job.chOpts.flags = flags;

        if(error)
        {
            Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "client connect error", channel);

            if(0 <= job.serverFd)
            {
                close(job.serverFd);
                job.serverFd = -1;
            }

            return false;
        }

        if(job.channel <= Channel::System || job.channel >= Channel::Reserved)
        {
            Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel incorrect", job.channel);

            if(0 <= job.serverFd)
            {
                close(job.serverFd);
                job.serverFd = -1;
            }

            return false;
        }

        if(findChannel(job.channel))
        {
            Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel busy", channel);

            if(0 <= job.serverFd)
            {
                close(job.serverFd);
                job.serverFd = -1;
            }

            return false;
        }

        if(0 <= job.serverFd)
        {
            Application::info("%s: %s, id: %" PRId8 ", client url: `%s', server url: `%s'", __FUNCTION__, "found planned job", channel, job.clientOpts.url.c_str(), "listener");

            switch(job.serverOpts.type())
            {
#ifdef __LINUX__
                case Channel::ConnectorType::Unix:
                    createChannelUnixFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                    break;

                case Channel::ConnectorType::Socket:
                    createChannelSocketFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                    break;
#endif

                default:
                    Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel type not implemented", channel);
                    throw channel_error(NS_FuncName);
            }
        }
        else if(! job.serverOpts.content().empty())
        {
            Application::info("%s: %s, id: %" PRId8 ", client url: `%s', server url: `%s'", __FUNCTION__, "found planned job", channel, job.clientOpts.url.c_str(), job.serverOpts.url.c_str());

            switch(job.serverOpts.type())
            {
#ifdef __LINUX__
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
                    Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel type not implemented", channel);
                    throw channel_error(NS_FuncName);
            }
        }
    }

    // set connected flag
    if(auto channelConn = findChannel(channel))
    {
        channelConn->setRemoteConnected(true);
    }
    else
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel not running", channel);
    }

    return true;
}

void LTSM::ChannelClient::systemChannelClose(const JsonObject & jo)
{
    int channel = jo.getInteger("id");
    destroyChannel(channel);
}

void LTSM::ChannelClient::sendSystemClientVariables(const json_plain & vars, const json_plain & env, const std::vector<std::string> & layouts, const std::string & group)
{
    JsonObjectStream jo;
    jo.push("cmd", SystemCommand::ClientVariables);
    jo.push("options", vars);
    jo.push("environments", env);

    JsonObjectStream jo2;
    jo2.push("layouts", JsonArrayStream(layouts.begin(), layouts.end()).flush());
    jo2.push("current", group);

    jo.push("keyboard", jo2.flush());

    sendLtsmChannelData(Channel::System, jo.flush());
}

void LTSM::ChannelClient::sendSystemKeyboardChange(const std::vector<std::string> & names, int group)
{
    if(0 <= group && group < names.size())
    {
        JsonObjectStream jo;
        jo.push("cmd", SystemCommand::KeyboardChange);
        jo.push("layout", names[group]);
        jo.push("group", group);
        jo.push("names", JsonArrayStream(names.begin(), names.end()).flush());

        sendLtsmChannelData(Channel::System, jo.flush());
    }
}

bool LTSM::ChannelClient::sendSystemTransferFiles(std::forward_list<std::string> files)
{
    Application::info("%s", __FUNCTION__);

    files.remove_if([](auto & file)
    {
        std::error_code err;

        if(! std::filesystem::is_regular_file(file, err))
        {
            Application::warning("%s: %s, path: `%s', uid: %d", "sendSystemTransferFiles", err.message().c_str(), file.c_str(), getuid());
            return true;
        }

        if(0 != access(file.c_str(), R_OK))
        {
            Application::warning("%s: skip not readable, file: %s", "sendSystemTransferFiles", file.c_str());
            return true;
        }

        return false;
    });

    if(files.empty())
    {
        Application::error("%s: failed,  empty list", __FUNCTION__);
        return false;
    }

    JsonObjectStream jo;
    jo.push("cmd", SystemCommand::TransferFiles);

    JsonArrayStream ja;

    for(auto & fname : files)
    {
        std::error_code err;

        if(auto fsize = std::filesystem::file_size(fname, err))
        {
            ja.push(JsonObjectStream().push("file", fname).push("size", static_cast<size_t>(fsize)).flush());
        }
    }

    jo.push("files", ja.flush());

    sendLtsmChannelData(Channel::System, jo.flush());
    return true;
}

bool LTSM::ChannelClient::createChannel(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts)
{
    if(clientOpts.mode == Channel::ConnectorMode::Unknown)
    {
        Application::error("%s: unknown %s mode", __FUNCTION__, "client");
        return false;
    }

    if(serverOpts.mode == Channel::ConnectorMode::Unknown)
    {
        Application::error("%s: unknown %s mode", __FUNCTION__, "server");
        return false;
    }

    if(serverOpts.mode == clientOpts.mode &&
            (serverOpts.mode == Channel::ConnectorMode::ReadOnly || serverOpts.mode == Channel::ConnectorMode::WriteOnly))
    {
        Application::error("%s: incorrect modes pair (wo,wo) or (ro,ro)", __FUNCTION__);
        return false;
    }

    Application::debug(DebugType::Channels, "%s: server url: `%s', client url: `%s'", serverOpts.url.c_str(), clientOpts.url.c_str());

    if(clientOpts.type() == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown client url: `%s'", __FUNCTION__, clientOpts.url.c_str());
        return false;
    }

    if(serverOpts.type() == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown server url: `%s'", __FUNCTION__, serverOpts.url.c_str());
        return false;
    }

    // find free channel
    uint8_t channel = 1;

    for(; channel < Channel::Reserved; ++channel)
    {
        if(! findChannel(channel) && ! findPlanned(channel))
        {
            break;
        }
    }

    if(channel == Channel::Reserved)
    {
        Application::error("%s: all channels busy", __FUNCTION__);
        return false;
    }
    else
    {
        const std::scoped_lock guard{lockpl};

        channelsPlanned.emplace_back(
                           Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = chOpts, .channel = channel });
    }

    // send channel open to client
    sendSystemChannelOpen(channel, clientOpts, chOpts);

    // next part: ChannelClient::systemChannelConnected

    return true;
}

bool LTSM::ChannelClient::createChannelClientAudio(uint8_t channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
#ifdef LTSM_CLIENT
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createClientAudioConnector(channel, url, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
#else
    Application::error("%s: %s, url: `%s'", __FUNCTION__, "unsupported audio", url.c_str());
    return false;
#endif
}

bool LTSM::ChannelClient::createChannelClientFuse(uint8_t channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
#ifdef LTSM_CLIENT
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createClientFuseConnector(channel, url, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
#else
    Application::error("%s: %s, url: `%s'", __FUNCTION__, "unsupported fuse", url.c_str());
    return false;
#endif
}

bool LTSM::ChannelClient::createChannelClientPcsc(uint8_t channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
#ifdef LTSM_CLIENT
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createClientPcscConnector(channel, url, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
#else
    Application::error("%s: %s, url: `%s'", __FUNCTION__, "unsupported pcsc", url.c_str());
    return false;
#endif
}

#ifdef __LINUX__
bool LTSM::ChannelClient::createChannelUnix(uint8_t channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    if(! createChannelAllow(Channel::ConnectorType::Unix, path.native(), mode))
    {
        Application::error("%s: %s, content: `%s'", __FUNCTION__, "blocked", path.c_str());
        return false;
    }

    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createUnixConnector(channel, path, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelUnixFd(uint8_t channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createUnixConnector(channel, sock, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelClientPkcs11(uint8_t channel, const std::string & url, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
#if defined(LTSM_CLIENT) && defined(LTSM_PKCS11_AUTH)
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createClientPkcs11Connector(channel, url, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
#else
    Application::error("%s: %s, url: `%s'", __FUNCTION__, "unsupported pkcs11", url.c_str());
    return false;
#endif
}

#endif // __LINUX__

bool LTSM::ChannelClient::createChannelFile(uint8_t channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
#if defined(__MINGW64__) || defined(__MINGW32__)
    if(! createChannelAllow(Channel::ConnectorType::File, Tools::wstring2string(path.native()), mode))
#else
    if(! createChannelAllow(Channel::ConnectorType::File, path.native(), mode))
#endif
    {
        Application::error("%s: %s, content: `%s'", __FUNCTION__, "blocked", path.c_str());
        return false;
    }

    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createFileConnector(channel, path, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelCommand(uint8_t channel, const std::string & runcmd, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    if(! createChannelAllow(Channel::ConnectorType::Command, runcmd, mode))
    {
        Application::error("%s: %s, content: `%s'", __FUNCTION__, "blocked", runcmd.c_str());
        return false;
    }

    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", run cmd: `%s', mode: %s", __FUNCTION__, channel, runcmd.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createCommandConnector(channel, runcmd, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}

#ifdef __LINUX__
bool LTSM::ChannelClient::createChannelSocket(uint8_t channel, std::pair<std::string, int> ipAddrPort, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    if(! createChannelAllow(Channel::ConnectorType::Socket, ipAddrPort.first, mode))
    {
        Application::error("%s: %s, content: `%s'", __FUNCTION__, "blocked", ipAddrPort.first.c_str());
        return false;
    }

    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", addr: %s, port: %d, mode: %s", __FUNCTION__, channel, ipAddrPort.first.c_str(), ipAddrPort.second, Channel::Connector::modeString(mode));

    if(serverSide() && 0 != ipAddrPort.first.compare(0, 4, "127."))
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "server side allow socket only for localhost", channel);
        return false;
    }

    if(0 > ipAddrPort.second)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "incorrect connection info", channel);
        return false;
    }

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createTcpConnector(channel, ipAddrPort.first, ipAddrPort.second, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelSocketFd(uint8_t channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::createTcpConnector(channel, sock, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    return true;
}
#endif

void LTSM::ChannelClient::destroyChannel(uint8_t channel)
{
    std::thread([this, channel]
    {
        const std::scoped_lock guard{this->lockch};

        auto it = std::find_if(this->channels.begin(), this->channels.end(), [=](auto & ptr) { return ptr && ptr->channel() == channel; });

        if(it != this->channels.end())
        {
            (*it)->setRunning(false);
            std::this_thread::sleep_for(100ms);
            this->channels.erase(it);
            Application::info("%s: %s, id: %" PRId8, "destroyChannel", "channel removed", channel);
        }
        else
        {
            Application::error("%s: %s, id: %" PRId8, "destroyChannel", "channel not found", channel);
        }
    }).detach();
}

void LTSM::ChannelClient::sendSystemChannelOpen(uint8_t channel, const Channel::UrlMode & clientOpts, const Channel::Opts & chOpts)
{
    Application::info("%s: id: %" PRId8 ", content: `%s'", __FUNCTION__, channel, clientOpts.content().c_str());
    JsonObjectStream jo;

    jo.push("cmd", SystemCommand::ChannelOpen);
    jo.push("id", channel);
    jo.push("type", Channel::Connector::typeString(clientOpts.type()));
    jo.push("mode", Channel::Connector::modeString(clientOpts.mode));
    jo.push("speed", Channel::Connector::speedString(chOpts.speed));
    jo.push("flags", chOpts.flags);

    if(clientOpts.type() == Channel::ConnectorType::Socket)
    {
        auto [ ipaddr, port ] = Channel::Connector::parseAddrPort(clientOpts.content());
        jo.push("port", port);
        jo.push("ipaddr", ipaddr);
    }
    else if(clientOpts.type() == Channel::ConnectorType::Command)
    {
        jo.push("runcmd", clientOpts.content());
    }
    else if(clientOpts.type() == Channel::ConnectorType::Fuse)
    {
        jo.push("fuse", clientOpts.content());
    }
    else if(clientOpts.type() == Channel::ConnectorType::Audio)
    {
        jo.push("audio", clientOpts.content());
    }
    else if(clientOpts.type() == Channel::ConnectorType::Pcsc)
    {
        jo.push("pcsc", clientOpts.content());
    }
    else if(clientOpts.type() == Channel::ConnectorType::Pkcs11)
    {
        jo.push("pkcs11", clientOpts.content());
    }
    else
    {
        jo.push("path", clientOpts.content());
    }

    sendLtsmChannelData(Channel::System, jo.flush());
}

void LTSM::ChannelClient::sendSystemChannelError(uint8_t channel, int code, const std::string & err)
{
    sendLtsmChannelData(Channel::System, JsonObjectStream().push("cmd", SystemCommand::ChannelError).push("id", channel).push("code", code).push("error", err).flush());
}

void LTSM::ChannelClient::sendSystemChannelClose(uint8_t channel)
{
    sendLtsmChannelData(Channel::System, JsonObjectStream().push("cmd", SystemCommand::ChannelClose).push("id", channel).flush());
}

void LTSM::ChannelClient::sendSystemChannelConnected(uint8_t channel, int flags, bool noerror)
{
    sendLtsmChannelData(Channel::System, JsonObjectStream().
                  push("cmd", SystemCommand::ChannelConnected).
                  push("flags", flags).
                  push("error", ! noerror).
                  push("id", channel).flush());
}

void LTSM::ChannelClient::recvLtsmProto(const NetworkStream & ns)
{
    int version = ns.recvInt8();

    if(version != LtsmProtocolVersion)
    {
        Application::error("%s: unknown version: 0x%02x", __FUNCTION__, version);
        throw std::runtime_error(NS_FuncName);
    }

    auto channel = ns.recvInt8();
    auto length = ns.recvIntBE16();
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", data size: %" PRIu16, __FUNCTION__, channel, length);

    auto buf = ns.recvData(length);

    if(channelDebug == channel)
    {
        auto str = Tools::buffer2hexstring(buf.begin(), buf.end(), 2);
        Application::info("%s: id: %" PRId8 ", size: %" PRIu16 ", content: [%s]", __FUNCTION__, channel, length, str.c_str());
    }

    recvLtsmEvent(channel, std::move(buf));
}

void LTSM::ChannelClient::sendLtsmProto(NetworkStream & ns, std::mutex & sendLock,
                                   uint8_t channel, const uint8_t* buf, size_t len)
{
    Application::debug(DebugType::Channels, "%s: id: %" PRId8 ", data size: %u", __FUNCTION__, channel, len);

    const std::scoped_lock guard{sendLock};

    ns.sendInt8(RFB::PROTOCOL_LTSM);

    // version
    ns.sendInt8(LtsmProtocolVersion);
    //channel
    ns.sendInt8(channel);

    if(len == 0 || !buf)
    {
        Application::error("%s: empty data", __FUNCTION__);
        throw std::invalid_argument(NS_FuncName);
    }

    if(0xFFFF < len)
    {
        Application::error("%s: data size large", __FUNCTION__);
        throw std::invalid_argument(NS_FuncName);
    }

    // data
    ns.sendIntBE16(len);

    if(channelDebug == channel)
    {
        auto str = Tools::buffer2hexstring(buf, buf + len, 2);
        Application::info("%s: id: %" PRId8 ", size: %u, content: [%s]", __FUNCTION__, channel, len, str.c_str());
    }

    ns.sendRaw(buf, len);
    ns.sendFlush();
}

void LTSM::ChannelClient::setChannelDebug(const uint8_t & channel, const bool & debug)
{
    if(debug)
    {
        channelDebug = channel;
    }
    else if(channelDebug == channel)
    {
        channelDebug = -1;
    }
}

void LTSM::ChannelClient::channelsShutdown(void)
{
    const std::scoped_lock guard{lockch};

    for(auto & ptr : channels)
    {
        ptr->setRunning(false);
    }
}

#ifdef __LINUX__
// ChannelListener
bool LTSM::ChannelListener::createListener(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, size_t listen, const Channel::Opts & chOpts)
{
    Application::debug(DebugType::Channels, "%s: client: %s, server: %s", __FUNCTION__, clientOpts.url.c_str(), serverOpts.url.c_str());

    try
    {
        const std::scoped_lock guard{lockls};

        if(std::any_of(listeners.begin(), listeners.end(), [&](auto & ptr) { return ptr->getServerUrl() == serverOpts.url; }))
        {
            Application::debug(DebugType::Channels, "%s: listen present, url: %s", __FUNCTION__, serverOpts.url.c_str());
            return true;
        }

        if(serverOpts.type() == Channel::ConnectorType::Socket)
        {
            listeners.emplace_back(Channel::createTcpListener(serverOpts, listen, clientOpts, chOpts, *this));
            return true;
        }
        else if(serverOpts.type() == Channel::ConnectorType::Unix)
        {
            listeners.emplace_back(Channel::createUnixListener(serverOpts, listen, clientOpts, chOpts, *this));
            return true;
        }
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    Application::error("%s: allow unix or socket format only, url: `%s'", __FUNCTION__, serverOpts.url.c_str());
    return false;
}

void LTSM::ChannelListener::destroyListener(const std::string & clientUrl, const std::string & serverUrl)
{
    const std::scoped_lock guard{lockls};
    auto it = std::find_if(listeners.begin(), listeners.end(), [&](auto & ptr) { return ptr && ptr->getClientUrl() == clientUrl; });

    if(it != listeners.end())
    {
        (*it)->setRunning(false);

        std::this_thread::sleep_for(100ms);
        listeners.erase(it);
        Application::info("%s: client url: `%s'", __FUNCTION__, clientUrl.c_str());
    }
}

bool LTSM::ChannelListener::createChannelAcceptFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts)
{
    if(clientOpts.mode == Channel::ConnectorMode::Unknown)
    {
        Application::error("%s: unknown %s mode", __FUNCTION__, "client");
        return false;
    }

    if(serverOpts.mode == Channel::ConnectorMode::Unknown)
    {
        Application::error("%s: unknown %s mode", __FUNCTION__, "server");
        return false;
    }

    if(serverOpts.mode == clientOpts.mode &&
            (serverOpts.mode == Channel::ConnectorMode::ReadOnly || serverOpts.mode == Channel::ConnectorMode::WriteOnly))
    {
        Application::error("%s: incorrect modes pair (wo,wo) or (ro,ro)", __FUNCTION__);
        return false;
    }

    // parse url client
    Application::debug(DebugType::Channels, "client url: `%s', mode: %s", clientOpts.url.c_str(), Channel::Connector::modeString(clientOpts.mode));

    if(clientOpts.type() == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown client url: `%s'", __FUNCTION__, clientOpts.url.c_str());
        return false;
    }

    // find free channel
    uint8_t channel = 1;

    for(; channel < Channel::Reserved; ++channel)
    {
        if(! findChannel(channel) && ! findPlanned(channel))
        {
            break;
        }
    }

    if(channel == Channel::Reserved)
    {
        Application::error("%s: all channels busy", __FUNCTION__);
        return false;
    }
    else
    {
        const std::scoped_lock guard{lockpl};

        channelsPlanned.emplace_back(
                           Channel::Planned{ .serverOpts = serverOpts, .clientOpts = clientOpts, .chOpts = chOpts, .serverFd = sock, .channel = channel });
    }

    // send channel open to client
    sendSystemChannelOpen(channel, clientOpts, chOpts);

    // next part: ChannelClient::systemChannelConnected

    return true;
}
#endif

// Remote2Local
LTSM::Channel::Remote2Local::Remote2Local(uint8_t cid, int flags) : id(cid)
{
    if(Channel::OptsFlags::ZLibCompression & flags)
    {
        zlib = std::make_unique<ZLib::InflateBase>();
    }
}

LTSM::Channel::Remote2Local::~Remote2Local()
{
    Application::info("%s: channel: %" PRIu8 ", receive: %u byte, transfer: %u byte, error: %d", "Remote2Local", id, transfer1, transfer2, error);
}

void LTSM::Channel::Remote2Local::pushData(std::vector<uint8_t> && buf)
{
    const std::scoped_lock guard{lockQueue};
    queueBufs.emplace_back(std::move(buf));
}

std::vector<uint8_t> LTSM::Channel::Remote2Local::popData(void)
{
    const std::scoped_lock guard{lockQueue};

    if(queueBufs.empty())
        return {};

    auto queueSz = queueBufs.size();

    if(queueSz > 10)
    {
        // descrease delay
        if(delay > std::chrono::milliseconds{10})
        {
            Application::warning("%s: id: %" PRId8 ", queue large: %u, change delay to %ums", __FUNCTION__, id, queueSz, delay.count());
            delay -= std::chrono::milliseconds{10};
        }
        else
        {
            Application::warning("%s: id: %" PRId8 ", queue large: %u, fixme: `%s'", __FUNCTION__, id, queueSz, "fixme: remote decrease speed");
        }
    }

    auto buf = std::move(queueBufs.front());
    queueBufs.pop_front();

    return buf;
}

bool LTSM::Channel::Remote2Local::writeData(void)
{
    auto buf = popData();

    if(buf.empty())
    {
        return true;
    }

    transfer1 += buf.size();

    if(zlib)
    {
        auto buf2 = zlib->inflateData(buf.data(), buf.size(), Z_SYNC_FLUSH);
        buf.swap(buf2);
        // Application::debug(DebugType::Channels, "%s: inflate, size1: %d, size2: %d", __FUNCTION__, buf.size(), buf2.size());
    }

    size_t writesz = 0;

    while(writesz < buf.size())
    {
        ssize_t real = writeDataFrom(buf.data() + writesz, buf.size() - writesz);

        if(0 < real)
        {
            writesz += real;
            transfer2 += real;
            continue;
        }

        if(EAGAIN == errno || EINTR == errno)
        {
            continue;
        }

        error = errno;
        return false;
    }

    return true;
}

void LTSM::Channel::Remote2Local::setSpeed(const Channel::Speed & speed)
{
    switch(speed)
    {
        case Speed::VerySlow:
            delay = std::chrono::milliseconds(200);
            break;

        case Speed::Slow:
            delay = std::chrono::milliseconds(100);
            break;

        case Speed::Medium:
            delay = std::chrono::milliseconds(100);
            break;

        case Speed::Fast:
            delay = std::chrono::milliseconds(60);
            break;

        case Speed::UltraFast:
            delay = std::chrono::milliseconds(20);
            break;
    }
}

/// Remote2Local_FD
LTSM::Channel::Remote2Local_FD::Remote2Local_FD(uint8_t cid, int fd0, bool close, int flags)
    : Remote2Local(cid, flags), fd(fd0), needClose(close)
{
}

LTSM::Channel::Remote2Local_FD::~Remote2Local_FD()
{
    if(needClose && 0 <= fd)
    {
        close(fd);
    }
}

ssize_t LTSM::Channel::Remote2Local_FD::writeDataFrom(const void* buf, size_t len)
{
    return ::write(fd, buf, len);
}

// Local2Remote
LTSM::Channel::Local2Remote::Local2Remote(uint8_t cid, int flags) : id(cid)
{
    if(Channel::OptsFlags::ZLibCompression & flags)
    {
        zlib = std::make_unique<ZLib::DeflateBase>(Z_BEST_SPEED + 2);
    }

    buf.reserve(0xFFFF);
}

LTSM::Channel::Local2Remote::~Local2Remote()
{
    Application::info("%s: channel: %" PRIu8 ", receive: %u byte, transfer: %u byte, error: %d", "Local2Remote", id, transfer1, transfer2, error);
}

bool LTSM::Channel::Local2Remote::readData(void)
{
    size_t dtsz = 0;

    try
    {
        if(hasInput())
        {
            dtsz = hasData();
        }
    }
    catch(const std::exception & err)
    {
        error = errno;
        Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        return false;
    }

    if(0 == dtsz)
    {
        buf.clear();
        return true;
    }

    buf.resize(std::min(dtsz, blocksz));
    ssize_t real = readDataTo(buf.data(), buf.size());

    if(0 < real)
    {
        buf.resize(real);
        transfer1 += real;

        if(zlib)
        {
            auto buf2 = zlib->deflateData(buf.data(), buf.size(), Z_SYNC_FLUSH);
            transfer2 += buf2.size();
            buf.swap(buf2);
            // Application::debug(DebugType::Channels, "%s: deflate, size1: %d, size2: %d", __FUNCTION__, buf2.size(), buf.size());
        }
        else
        {
            transfer2 += real;
        }

        return true;
    }

    // eof
    if(0 == real)
    {
        return false;
    }

    if(EAGAIN == errno || EINTR == errno)
    {
        buf.clear();
        return true;
    }

    error = errno;
    return false;
}

void LTSM::Channel::Local2Remote::setSpeed(const Channel::Speed & speed)
{
    switch(speed)
    {
        // ~10k/sec
        case Speed::VerySlow:
            blocksz = 4096;
            delay = std::chrono::milliseconds(200);
            break;

        // ~40k/sec
        case Speed::Slow:
            blocksz = 8192;
            delay = std::chrono::milliseconds(100);
            break;

        // ~80k/sec
        case Speed::Medium:
            blocksz = 16384;
            delay = std::chrono::milliseconds(100);
            break;

        // ~800k/sec
        case Speed::Fast:
            blocksz = 32768;
            delay = std::chrono::milliseconds(60);
            break;

        // ~1600k/sec
        case Speed::UltraFast:
            delay = std::chrono::milliseconds(20);
            blocksz = 49152;
            break;
    }
}

/// Local2Remote_FD
LTSM::Channel::Local2Remote_FD::Local2Remote_FD(uint8_t cid, int fd0, bool close, int flags)
    : Local2Remote(cid, flags), fd(fd0), needClose(close)
{
}

LTSM::Channel::Local2Remote_FD::~Local2Remote_FD()
{
    if(needClose && 0 <= fd)
    {
        close(fd);
    }
}

bool LTSM::Channel::Local2Remote_FD::hasInput(void) const
{
    return NetworkStream::hasInput(fd);
}

size_t LTSM::Channel::Local2Remote_FD::hasData(void) const
{
    return NetworkStream::hasData(fd);
}

ssize_t LTSM::Channel::Local2Remote_FD::readDataTo(void* buf, size_t len)
{
    return ::read(fd, buf, len);
}

/// ConnectorBase
LTSM::Channel::ConnectorBase::ConnectorBase(uint8_t ch, const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : owner(& srv), mode(mod), flags(chOpts.flags)
{
    owner->sendSystemChannelConnected(ch, chOpts.flags, true);
}

bool LTSM::Channel::ConnectorBase::isAllowSessionFor(bool user) const
{
    return (flags & OptsFlags::AllowLoginSession) ? ! user : user;
}

bool LTSM::Channel::ConnectorBase::isRunning(void) const
{
    return loopRunning;
}

bool LTSM::Channel::ConnectorBase::isRemoteConnected(void) const
{
    return remoteConnected;
}

void LTSM::Channel::ConnectorBase::setRunning(bool f)
{
    loopRunning = f;
}

void LTSM::Channel::ConnectorBase::setRemoteConnected(bool f)
{
    remoteConnected = f;
}

void LTSM::Channel::Connector::loopWriter(ConnectorBase* cn, Remote2Local* st)
{
    bool error = false;
    auto owner = cn->getOwner();

    if(! owner)
    {
        Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, st->cid(), "owner");
        return;
    }

    while(cn->isRunning())
    {
        if(st->isEmpty())
        {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        }

        if(! st->writeData())
        {
            error = true;
            cn->setRunning(false);
        }
    }

    if(error)
    {
        owner->sendSystemChannelError(st->cid(), st->getError(), std::string(__FUNCTION__).append(": ").append(strerror(st->getError())));

        Application::error("%s: id: %" PRId8 ", error: %s", __FUNCTION__, st->cid(), strerror(st->getError()));
    }
    else
    {
        // all data write
        while(! st->isEmpty())
        {
            if(! st->writeData()) { break; }
        }
    }

    // read/write priority send
    if(! cn->isMode(ConnectorMode::ReadWrite) || cn->isMode(ConnectorMode::WriteOnly))
    {
        owner->sendSystemChannelClose(st->cid());
    }
}

void LTSM::Channel::Connector::loopReader(ConnectorBase* cn, Local2Remote* st)
{
    bool error = false;
    auto owner = cn->getOwner();

    if(! owner)
    {
        Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, st->cid(), "owner");
        return;
    }

    while(cn->isRunning())
    {
        if(! st->readData())
        {
            error = true;
            cn->setRunning(false);
        }

        if(st->getBuf().empty())
        {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        }
        else
        {
            auto & buf = st->getBuf();
            owner->sendLtsmChannelData(st->cid(), buf.data(), buf.size());
        }
    }

    if(error)
    {
        owner->sendSystemChannelError(st->cid(), st->getError(), std::string(__FUNCTION__).append(": ").append(strerror(st->getError())));
        Application::error("%s: id: %" PRId8 ", error: %s", __FUNCTION__, st->cid(), strerror(st->getError()));
    }

    // read/write priority send
    if(cn->isMode(ConnectorMode::ReadWrite) || cn->isMode(ConnectorMode::ReadOnly))
    {
        owner->sendSystemChannelClose(st->cid());
    }
}

/// ConnectorFD_R
LTSM::Channel::ConnectorFD_R::ConnectorFD_R(uint8_t ch, int fd0, bool close, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, ConnectorMode::ReadOnly, chOpts, srv)
{
    // start threads
    setRunning(true);

    localRemote = std::make_unique<Local2Remote_FD>(ch, fd0, close, chOpts.flags);
    localRemote->setSpeed(chOpts.speed);

    if(localRemote)
    {
        thr = std::thread(Connector::loopReader, this, localRemote.get());
    }
}

LTSM::Channel::ConnectorFD_R::~ConnectorFD_R()
{
    setRunning(false);

    if(thr.joinable())
    {
        thr.join();
    }
}

int LTSM::Channel::ConnectorFD_R::error(void) const
{
    return localRemote ? localRemote->getError() : 0;
}

uint8_t LTSM::Channel::ConnectorFD_R::channel(void) const
{
    return localRemote ? localRemote->cid() : 0;
}

void LTSM::Channel::ConnectorFD_R::setSpeed(const Channel::Speed & speed)
{
    if(localRemote)
    {
        localRemote->setSpeed(speed);
    }
}

/// ConnectorFD_W
LTSM::Channel::ConnectorFD_W::ConnectorFD_W(uint8_t ch, int fd0, bool close, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, ConnectorMode::WriteOnly, chOpts, srv)
{
    // start threads
    setRunning(true);

    remoteLocal = std::make_unique<Remote2Local_FD>(ch, fd0, close, chOpts.flags);
    remoteLocal->setSpeed(chOpts.speed);

    if(remoteLocal)
    {
        thw = std::thread(Connector::loopWriter, this, remoteLocal.get());
    }
}

LTSM::Channel::ConnectorFD_W::~ConnectorFD_W()
{
    setRunning(false);

    if(thw.joinable())
    {
        thw.join();
    }
}

int LTSM::Channel::ConnectorFD_W::error(void) const
{
    return remoteLocal ? remoteLocal->getError() : 0;
}

uint8_t LTSM::Channel::ConnectorFD_W::channel(void) const
{
    return remoteLocal ? remoteLocal->cid() : 0;
}

void LTSM::Channel::ConnectorFD_W::setSpeed(const Channel::Speed & speed)
{
    if(remoteLocal)
    {
        remoteLocal->setSpeed(speed);
    }
}

void LTSM::Channel::ConnectorFD_W::pushData(std::vector<uint8_t> && buf)
{
    if(! buf.empty() && remoteLocal)
    {
        remoteLocal->pushData(std::move(buf));
    }
}

/// ConnectorFD_RW
LTSM::Channel::ConnectorFD_RW::ConnectorFD_RW(uint8_t ch, int fd0, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, ConnectorMode::ReadWrite, chOpts, srv)
{
    // start threads
    setRunning(true);

    localRemote = std::make_unique<Local2Remote_FD>(ch, fd0, true, chOpts.flags);
    localRemote->setSpeed(chOpts.speed);

    remoteLocal = std::make_unique<Remote2Local_FD>(ch, fd0, true, chOpts.flags);
    remoteLocal->setSpeed(chOpts.speed);

    if(localRemote)
    {
        thr = std::thread(Connector::loopReader, this, localRemote.get());
    }

    if(remoteLocal)
    {
        thw = std::thread(Connector::loopWriter, this, remoteLocal.get());
    }
}

LTSM::Channel::ConnectorFD_RW::~ConnectorFD_RW()
{
    setRunning(false);

    if(thr.joinable())
    {
        thr.join();
    }

    if(thw.joinable())
    {
        thw.join();
    }
}

int LTSM::Channel::ConnectorFD_RW::error(void) const
{
    int err1 = remoteLocal ? remoteLocal->getError() : 0;
    int err2 = localRemote ? localRemote->getError() : 0;

    return err1 ? err1 : err2;
}

uint8_t LTSM::Channel::ConnectorFD_RW::channel(void) const
{
    if(remoteLocal)
    {
        return remoteLocal->cid();
    }

    if(localRemote)
    {
        return localRemote->cid();
    }

    return 0;
}

void LTSM::Channel::ConnectorFD_RW::setSpeed(const Channel::Speed & speed)
{
    if(localRemote)
    {
        localRemote->setSpeed(speed);
    }

    if(remoteLocal)
    {
        remoteLocal->setSpeed(speed);
    }
}

void LTSM::Channel::ConnectorFD_RW::pushData(std::vector<uint8_t> && buf)
{
    if(! buf.empty() && remoteLocal)
    {
        remoteLocal->pushData(std::move(buf));
    }
}

// ConnectorCMD_W
LTSM::Channel::ConnectorCMD_W::ConnectorCMD_W(uint8_t channel, FILE* ptr, const Opts & chOpts, ChannelClient & owner)
    : ConnectorFD_W(channel, fileno(ptr), false, chOpts, owner), fcmd(ptr)
{
}

LTSM::Channel::ConnectorCMD_W::~ConnectorCMD_W()
{
    if(fcmd)
    {
        pclose(fcmd);
    }
}

// ConnectorCMD_R
LTSM::Channel::ConnectorCMD_R::ConnectorCMD_R(uint8_t channel, FILE* ptr, const Opts & chOpts, ChannelClient & owner)
    : ConnectorFD_R(channel, fileno(ptr), false, chOpts, owner), fcmd(ptr)
{
}

LTSM::Channel::ConnectorCMD_R::~ConnectorCMD_R()
{
    if(fcmd)
    {
        pclose(fcmd);
    }
}

#ifdef __LINUX__
/// createUnixConnector
LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createUnixConnector(uint8_t channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    std::error_code err;

    if(! std::filesystem::is_socket(path, err))
    {
        Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), path.c_str(), getuid());
        throw channel_error(NS_FuncName);
    }

    Application::info("%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    int fd = UnixSocket::connect(path);

    if(0 > fd)
    {
        Application::error("%s: %s, id: %" PRId8 ", path: `%s'", __FUNCTION__, "unix failed", channel, path.c_str());
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadWrite)
    {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}

LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createUnixConnector(uint8_t channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "unix failed", channel);
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadWrite)
    {
        return std::make_unique<ConnectorFD_RW>(channel, sock, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorFD_R>(channel, sock, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorFD_W>(channel, sock, true, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}

/// createTcpConnector
LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createTcpConnector(uint8_t channel, const std::string & ipaddr, int port, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", addr: `%s', port: %d, mode: %s", __FUNCTION__, channel, ipaddr.c_str(), port, Channel::Connector::modeString(mode));

    int fd = TCPSocket::connect(ipaddr, port);

    if(0 > fd)
    {
        Application::error("%s: %s, id: %" PRId8 ", addr: `%s', port: %d", __FUNCTION__, "socket failed", channel, ipaddr.c_str(), port);
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadWrite)
    {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}

LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createTcpConnector(uint8_t channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "socket failed", channel);
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadWrite)
    {
        return std::make_unique<ConnectorFD_RW>(channel, sock, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorFD_R>(channel, sock, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorFD_W>(channel, sock, true, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}
#endif

/// createFileConnector
LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createFileConnector(uint8_t channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "file mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    std::error_code err;

    if(mode == ConnectorMode::ReadOnly &&
            ! std::filesystem::exists(path, err))
    {
        Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), path.c_str(), getuid());
        throw channel_error(NS_FuncName);
    }

    int fd = 0;

    if(mode == ConnectorMode::ReadOnly)
    {
#if defined(__MINGW64__) || defined(__MINGW32__)
        auto cpath = Tools::wstring2string(path);
        fd = open(cpath.c_str(), O_RDONLY);
#else
        fd = open(path.c_str(), O_RDONLY);
#endif
    }
    else if(mode == ConnectorMode::WriteOnly)
    {
        int flags = O_WRONLY;

        if(std::filesystem::exists(path, err))
        {
            flags |= O_APPEND;
            Application::warning("%s: %s, path: `%s'", __FUNCTION__, "file exists switch mode to append", path.c_str());
        }
        else
        {
            flags |= O_CREAT | O_EXCL;
        }

#if defined(__MINGW64__) || defined(__MINGW32__)
        auto cpath = Tools::wstring2string(path);
        fd = open(cpath.c_str(), flags, S_IRUSR|S_IWUSR|S_IRGRP);
#else
        fd = open(path.c_str(), flags, S_IRUSR|S_IWUSR|S_IRGRP);
#endif
    }

    if(0 > fd)
    {
        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open file", strerror(errno), errno);
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadWrite)
    {
        return std::make_unique<ConnectorFD_RW>(channel, fd, chOpts, sender);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorFD_R>(channel, fd, true, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorFD_W>(channel, fd, true, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}

/// createCommandConnector
LTSM::Channel::ConnectorBasePtr
LTSM::Channel::createCommandConnector(uint8_t channel, const std::string & runcmd, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", run cmd: `%s', mode: %s", __FUNCTION__, channel, runcmd.c_str(), Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "cmd mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    auto list = Tools::split(runcmd, 0x20);

    if(list.empty())
    {
        Application::error("%s: %s", __FUNCTION__, "cmd empty");
        throw channel_error(NS_FuncName);
    }

    std::error_code err;

    if(! std::filesystem::exists(list.front(), err))
    {
        Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, err.message().c_str(), list.front().c_str(), getuid());
        throw channel_error(NS_FuncName);
    }

    FILE* fcmd = nullptr;

    if(std::filesystem::is_symlink(list.front(), err))
    {
        auto cmd = Tools::resolveSymLink(list.front());
        list.pop_front();
#if defined(__MINGW64__) || defined(__MINGW32__)
        list.push_front(Tools::wstring2string(cmd.native()));
#else
        list.push_front(cmd.native());
#endif
        auto runcmd2 = Tools::join(list.begin(), list.end(), " ");

        fcmd = popen(runcmd2.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    }
    else if(std::filesystem::is_regular_file(list.front(), err))
    {
        fcmd = popen(runcmd.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    }

    if(! fcmd)
    {
        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open cmd", strerror(errno), errno);
        throw channel_error(NS_FuncName);
    }

    if(mode == ConnectorMode::ReadOnly)
    {
        return std::make_unique<ConnectorCMD_R>(channel, fcmd, chOpts, sender);
    }

    if(mode == ConnectorMode::WriteOnly)
    {
        return std::make_unique<ConnectorCMD_W>(channel, fcmd, chOpts, sender);
    }

    Application::error("%s: id: %" PRId8 ", %s failed", __FUNCTION__, channel, "mode");
    throw channel_error(NS_FuncName);
}

#ifdef __LINUX__
/// Listener
LTSM::Channel::Listener::Listener(int fd, const UrlMode & serverOpts, const UrlMode & clientOpts, const Channel::Opts & ch, ChannelListener & sender)
    : sopts(serverOpts), copts(clientOpts), owner(& sender), chopts(ch), srvfd(fd)
{
    loopRunning = true;
    th = std::thread(loopAccept, this);
}

LTSM::Channel::Listener::~Listener()
{
    loopRunning = false;

    if(th.joinable())
    {
        th.join();
    }

    if(0 <= srvfd)
    {
        close(srvfd);
    }

    if(isUnix())
    {
        try
        {
            if(std::filesystem::exists(sopts.content()) && std::filesystem::is_socket(sopts.content()))
            {
                std::filesystem::remove(sopts.content());
            }
        }
        catch(const std::filesystem::filesystem_error &)
        {
        }
    }
}

bool LTSM::Channel::Listener::isRunning(void) const
{
    return loopRunning;
}

void LTSM::Channel::Listener::setRunning(bool f)
{
    loopRunning = f;
}

void LTSM::Channel::Listener::loopAccept(Listener* st)
{
    while(st->loopRunning)
    {
        bool input = false;

        try
        {
            input = NetworkStream::hasInput(st->srvfd);
        }
        catch(const std::exception & err)
        {
            st->loopRunning = false;

            Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
        }

        if(input)
        {
            auto sock = st->isUnix() ?
                        UnixSocket::accept(st->srvfd) : TCPSocket::accept(st->srvfd);

            if(sock < 0)
            {
                st->loopRunning = false;
            }
            else if(! st->owner->createChannelAcceptFd(st->copts, sock, st->sopts, st->chopts))
            {
                close(sock);
            }
        }
        else
        {
            std::this_thread::sleep_for(250ms);
        }
    }
}

std::unique_ptr<LTSM::Channel::Listener>
LTSM::Channel::createUnixListener(const UrlMode & serverOpts, size_t listen,
                                  const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelListener & sender)
{
    auto & path = serverOpts.content();
    std::error_code err;

    if(std::filesystem::exists(path, err) &&
            ! std::filesystem::is_socket(path, err))
    {
        Application::error("%s: %s, path: `%s'", __FUNCTION__, "not socket", path.c_str());
        throw channel_error(NS_FuncName);
    }

    int srvfd = UnixSocket::listen(path, listen);

    if(0 > srvfd)
    {
        Application::error("%s: %s, path: `%s'", __FUNCTION__, "unix failed", path.c_str());
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Listener>(srvfd, serverOpts, clientOpts, chOpts, sender);
}

std::unique_ptr<LTSM::Channel::Listener>
LTSM::Channel::createTcpListener(const UrlMode & serverOpts, size_t listen,
                                 const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelListener & sender)
{
    auto [ ipaddr, port ] = Connector::parseAddrPort(serverOpts.content());

    if(0 >= port)
    {
        Application::error("%s: %s, url: `%s'", __FUNCTION__, "socket format", serverOpts.content().c_str());
        throw channel_error(NS_FuncName);
    }

    // hardcore server listen
    if(sender.serverSide())
    {
        ipaddr = "127.0.0.1";
    }

    int srvfd = TCPSocket::listen(ipaddr, port, listen);

    if(0 > srvfd)
    {
        Application::error("%s: %s, ipaddr: %s, port: %d", __FUNCTION__, "socket failed", ipaddr.c_str(), port);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Listener>(srvfd, serverOpts, clientOpts, chOpts, sender);
}
#endif
