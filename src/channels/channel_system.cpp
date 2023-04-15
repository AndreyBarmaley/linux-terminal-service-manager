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

#include <sys/socket.h>

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

///
std::string LTSM::Channel::createUrl(const ConnectorType & type, std::string_view body)
{
    return std::string(Connector::typeString(type)).append("://").append(body);
}

LTSM::Channel::ConnectorType LTSM::Channel::connectorType(std::string_view str)
{

    for(auto type : { ConnectorType::Unix, ConnectorType::Socket, ConnectorType::File, ConnectorType::Command })
        if(str == Connector::typeString(type)) return type;

    return ConnectorType::Unknown;
}

LTSM::Channel::ConnectorMode LTSM::Channel::connectorMode(std::string_view str)
{
    for(auto mode : { ConnectorMode::ReadOnly, ConnectorMode::ReadWrite, ConnectorMode::WriteOnly })
        if(str == Connector::modeString(mode)) return mode;

    return ConnectorMode::Unknown;
}

LTSM::Channel::Speed LTSM::Channel::connectorSpeed(std::string_view str)
{
    for(auto speed : { Speed::VerySlow, Speed::Slow, Speed::Medium, Speed::Fast, Speed::UltraFast })
        if(str == Connector::speedString(speed)) return speed;

    return Speed::VerySlow;
}

LTSM::Channel::DataPack LTSM::Channel::dataPack(int type)
{
    switch(type)
    {
	case Channel::DataPack::ZLib: return Channel::DataPack::ZLib;
	default: break;
    }

    return Channel::DataPack::Raw;
}

const char* LTSM::Channel::Connector::typeString(const ConnectorType & type)
{
    switch(type)
    {
        case ConnectorType::Unix:       return "unix";
        case ConnectorType::File:       return "file";
        case ConnectorType::Socket:     return "socket";
        case ConnectorType::Command:    return "command";
        default: break;
    }

    return "unknown";
}

const char* LTSM::Channel::Connector::modeString(const ConnectorMode & mode)
{
    switch(mode)
    {
        // default mode - unix: rw, socket: rw, file(present): ro, file(not found): wo
        case ConnectorMode::ReadWrite:  return "rw";
        case ConnectorMode::ReadOnly:   return "ro";
        case ConnectorMode::WriteOnly:  return "wo";
        default: break;
    }

    return "unknown";
}

const char* LTSM::Channel::Connector::speedString(const Speed & speed)
{
    switch(speed)
    {
        case Speed::Slow:       return "slow";
        case Speed::Medium:     return "medium";
        case Speed::Fast:       return "fast";
        case Speed::UltraFast:  return "ultra";
        default: break;
    }

    return "very";
}

std::pair<LTSM::Channel::ConnectorType, std::string>
    LTSM::Channel::parseUrl(std::string_view url)
{
    if(0 == url.compare(0, 7, "file://"))
        return std::make_pair(Channel::ConnectorType::File, std::string(url.substr(7)));

    if(0 == url.compare(0, 7, "unix://"))
        return std::make_pair(Channel::ConnectorType::Unix, std::string(url.substr(7)));

    if(0 == url.compare(0, 7, "sock://"))
        return std::make_pair(Channel::ConnectorType::Socket, std::string(url.substr(7)));

    if(0 == url.compare(0, 9, "socket://"))
        return std::make_pair(Channel::ConnectorType::Socket, std::string(url.substr(9)));

    if(0 == url.compare(0, 6, "cmd://"))
        return std::make_pair(Channel::ConnectorType::Command, std::string(url.substr(6)));

    if(0 == url.compare(0, 10, "command://"))
        return std::make_pair(Channel::ConnectorType::Command, std::string(url.substr(10)));

    return std::make_pair(Channel::ConnectorType::Unknown, std::string(url.begin(), url.end()));
}

std::pair<std::string, int>
    LTSM::Channel::TcpConnector::parseAddrPort(std::string_view addrPort)
{
    Application::debug("%s: addr: `%s'", __FUNCTION__, addrPort.data());

    // format url
    // url1: hostname:port
    // url2: xx.xx.xx.xx:port
    auto list = Tools::split(addrPort, ':');

    int port = -1;
    std::string addr = "127.0.0.1";

    if(2 != list.size())
	return std::make_pair(addr, port);

    // check addr
    auto octets = Tools::split(list.front(), '.');
    if(4 == octets.size())
    {
        bool error = false;

        try
        {
    	    // check numbers
	    if(std::any_of(octets.begin(), octets.end(), [](auto & val){ return 255 < std::stoi(val); }))
                error = true;
        }
    	catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            error = true;
        }

        if(error)
            Application::error("%s: %s, addr: `%s'", __FUNCTION__, "incorrect ipaddr", addrPort.data());
    }
    else
    // resolv hostname
    {
        std::string addr2 = TCPSocket::resolvHostname(list.front());

        if(addr2.empty())
            Application::error("%s: %s, addr: `%s'", __FUNCTION__, "incorrect hostname", addrPort.data());
        else
            addr = addr2;
    }

    // check port
    try
    {
        port = std::stoi(list.back());
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
    }

    return std::make_pair(addr, port);
}

/// ChannelClient
LTSM::Channel::Connector* LTSM::ChannelClient::findChannel(uint8_t channel)
{
    const std::scoped_lock guard{lockch};
    auto it = std::find_if(channels.begin(), channels.end(), [=](auto & ptr){ return ptr && ptr->channel() == channel; });
    return it != channels.end() ? (*it).get() : nullptr;
}

LTSM::Channel::Planned* LTSM::ChannelClient::findPlanned(uint8_t channel)
{
    const std::scoped_lock guard{lockpl};
    auto it = std::find_if(channelsPlanned.begin(), channelsPlanned.end(), [=](auto & st){ return st.channel == channel; });
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

void LTSM::ChannelClient::sendLtsmEvent(uint8_t channel, std::string_view str)
{
    sendLtsmEvent(channel, reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

void LTSM::ChannelClient::sendLtsmEvent(uint8_t channel, const std::vector<uint8_t> & vec)
{
    sendLtsmEvent(channel, vec.data(), vec.size());
}

void LTSM::ChannelClient::recvLtsmEvent(uint8_t channel, std::vector<uint8_t> && buf)
{
    if(channel == Channel::Reserved)
    {
        Application::error("%s: reserved channel blocked", __FUNCTION__);
        throw std::invalid_argument(NS_FuncName);
    }

    if(channel == Channel::System)
        recvChannelSystem(buf);
    else
        recvChannelData(channel, std::move(buf));
}

void LTSM::ChannelClient::recvChannelData(uint8_t channel, std::vector<uint8_t> && buf)
{
    Application::debug("%s: id: %" PRId8 ", data size: %u", __FUNCTION__, channel, buf.size());

    if(! isUserSession())
    {
        Application::error("%s: ltsm channel disable for login session", __FUNCTION__);
        throw std::invalid_argument(NS_FuncName);
    }

    auto channelConn = findChannel(channel);
    if(! channelConn)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel not found", channel);
        throw std::invalid_argument(NS_FuncName);
    }

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
    auto pack = Channel::dataPack(jo.getInteger("packed", 0));
    bool replyError = false;

    // deprectated
    if(jo.hasKey("zlib"))
    {
	pack = jo.getBoolean("zlib", false) ? Channel::DataPack::ZLib : Channel::DataPack::Raw;
    }

    Application::info("%s: id: %" PRId8 ", type: %s, mode: %s, speed: %s, pack: %d", __FUNCTION__, channel, stype.c_str(), smode.c_str(), sspeed.c_str(), (int) pack);

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
        Channel::Opts chopts{ Channel::connectorSpeed(sspeed), pack};

        if(type == Channel::ConnectorType::Unix)
            replyError = ! createChannelUnix(channel, jo.getString("path"), mode, chopts);
        else
        if(type == Channel::ConnectorType::File)
            replyError = ! createChannelFile(channel, jo.getString("path"), mode, chopts);
        else
        if(type == Channel::ConnectorType::Socket)
            replyError = ! createChannelSocket(channel, std::make_pair(jo.getString("ipaddr"), jo.getInteger("port")), mode, chopts);
        else
        if(type == Channel::ConnectorType::Command)
            replyError = ! createChannelCommand(channel, jo.getString("runcmd"), mode, chopts);
        else
        {
            Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "unknown channel type", channel);
            replyError = true;
        }
    }

    if(replyError)
        sendSystemChannelConnected(channel, pack, false);
}

bool LTSM::ChannelClient::systemChannelConnected(const JsonObject & jo)
{
    int channel = jo.getInteger("id");
    bool error = jo.getBoolean("error");
    auto pack = Channel::dataPack(jo.getInteger("packed", 0));

    // deprectated
    if(jo.hasKey("zlib"))
    {
	pack = jo.getBoolean("zlib", false) ? Channel::DataPack::ZLib : Channel::DataPack::Raw;
    }

    // move planed to running
    const std::scoped_lock guard{lockpl};
    auto it = std::find_if(channelsPlanned.begin(), channelsPlanned.end(), [=](auto & st){ return st.channel == channel; });

    if(it != channelsPlanned.end())
    {
        auto job = std::move(*it);
        channelsPlanned.erase(it);

        job.chOpts.pack = pack;

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
            Application::info("%s: %s, id: %" PRId8 ", client url: `%s', server url: `%s'", __FUNCTION__, "found planned job", channel, job.clientOpts.url.c_str(), "listenner");

            switch(job.serverOpts.type)
            {
                case Channel::ConnectorType::Unix:
                    createChannelUnixFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                    break;

                case Channel::ConnectorType::Socket:
                    createChannelSocketFd(job.channel, job.serverFd, job.serverOpts.mode, job.chOpts);
                    break;

                default:
                    Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "channel type not implemented", channel);
                    throw channel_error(NS_FuncName);
            }
        }
        else
        if(! job.serverOpts.content.empty())
        {
            Application::info("%s: %s, id: %" PRId8 ", client url: `%s', server url: `%s'", __FUNCTION__, "found planned job", channel, job.clientOpts.url.c_str(), job.serverOpts.url.c_str());

            switch(job.serverOpts.type)
            {
                case Channel::ConnectorType::Unix:
                    createChannelUnix(job.channel, job.serverOpts.content, job.serverOpts.mode, job.chOpts);
                    break;

                case Channel::ConnectorType::File:
                    createChannelFile(job.channel, job.serverOpts.content, job.serverOpts.mode, job.chOpts);
                    break;

                case Channel::ConnectorType::Socket:
                    createChannelSocket(job.channel, Channel::TcpConnector::parseAddrPort(job.serverOpts.content), job.serverOpts.mode, job.chOpts);
                    break;

                case Channel::ConnectorType::Command:
                    createChannelCommand(job.channel, job.serverOpts.content, job.serverOpts.mode, job.chOpts);
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

    sendLtsmEvent(Channel::System, jo.flush());
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

        sendLtsmEvent(Channel::System, jo.flush());
    }
}

bool LTSM::ChannelClient::sendSystemTransferFiles(std::list<std::string> files)
{
    Application::info("%s: files: %u", __FUNCTION__, files.size());

    files.remove_if([](auto & file){
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
            ja.push(JsonObjectStream().push("file", fname).push("size", fsize).flush());
    }
    jo.push("files", ja.flush());

    sendLtsmEvent(Channel::System, jo.flush());
    return true;
}

bool LTSM::ChannelClient::createListener(const Channel::UrlMode & clientOpts, const Channel::UrlMode & serverOpts, size_t listen, const Channel::Opts & chOpts)
{
    Application::debug("%s: client: %s, server: %s", __FUNCTION__, clientOpts.url.c_str(), serverOpts.url.c_str());

    if(serverOpts.type == Channel::ConnectorType::Unix)
    {
        try
        {
            const std::scoped_lock guard{lockls};
            listenners.emplace_back(Channel::UnixListener::createListener(serverOpts, listen, clientOpts, chOpts, *this));
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            return false;
        }

        return true;
    }
    else
    if(serverOpts.type == Channel::ConnectorType::Socket)
    {
        try
        {
            const std::scoped_lock guard{lockls};
            listenners.emplace_back(Channel::TcpListener::createListener(serverOpts, listen, clientOpts, chOpts, *this));
        }
        catch(const std::exception & err)
        {
            Application::error("%s: exception: %s", __FUNCTION__, err.what());
            return false;
        }

        return true;
    }

    Application::error("%s: allow unix or socket format only, url: `%s'", __FUNCTION__, serverOpts.url.c_str());
    return false;
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

    Application::debug("%s: server url: `%s', client url: `%s'", serverOpts.url.c_str(), clientOpts.url.c_str());

    if(clientOpts.type == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown client url: `%s'", __FUNCTION__, clientOpts.url.c_str());
        return false;
    }

    if(serverOpts.type == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown server url: `%s'", __FUNCTION__, serverOpts.url.c_str());
        return false;
    }

    // find free channel
    uint8_t channel = 1;
    for(; channel < Channel::Reserved; ++channel)
    {
        if(! findChannel(channel) && ! findPlanned(channel))
            break;
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

bool LTSM::ChannelClient::createChannelFromListenerFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts & chOpts)
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
    Application::debug("client url: `%s', mode: %s", clientOpts.url.c_str(), Channel::Connector::modeString(clientOpts.mode));

    if(clientOpts.type == Channel::ConnectorType::Unknown)
    {
        Application::error("%s: unknown client url: `%s'", __FUNCTION__, clientOpts.url.c_str());
        return false;
    }

    // find free channel
    uint8_t channel = 1;
    for(; channel < Channel::Reserved; ++channel)
    {
        if(! findChannel(channel) && ! findPlanned(channel))
            break;
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

bool LTSM::ChannelClient::createChannelUnix(uint8_t channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::UnixConnector::createConnector(channel, path, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelUnixFd(uint8_t channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::UnixConnector::createConnector(channel, sock, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelFile(uint8_t channel, const std::filesystem::path & path, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", path: `%s', mode: %s", __FUNCTION__, channel, path.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::FileConnector::createConnector(channel, path, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelCommand(uint8_t channel, const std::string & runcmd, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", run cmd: `%s', mode: %s", __FUNCTION__, channel, runcmd.c_str(), Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::CommandConnector::createConnector(channel, runcmd, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelSocket(uint8_t channel, std::pair<std::string, int> ipAddrPort, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", addr: %s, port: %d, mode: %s", __FUNCTION__, channel, ipAddrPort.first.c_str(), ipAddrPort.second, Channel::Connector::modeString(mode));

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
        channels.emplace_back(Channel::TcpConnector::createConnector(channel, ipAddrPort.first, ipAddrPort.second, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

bool LTSM::ChannelClient::createChannelSocketFd(uint8_t channel, int sock, const Channel::ConnectorMode & mode, const Channel::Opts & chOpts)
{
    Application::debug("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    try
    {
        const std::scoped_lock guard{lockch};
        channels.emplace_back(Channel::TcpConnector::createConnector(channel, sock, mode, chOpts, *this));
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    return true;
}

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

void LTSM::ChannelClient::destroyListener(const std::string & clientUrl, const std::string & serverUrl)
{
    const std::scoped_lock guard{lockls};
    auto it = std::find_if(listenners.begin(), listenners.end(), [&](auto & ptr) { return ptr && ptr->getClientUrl() == clientUrl; });

    if(it != listenners.end())
    {
        (*it)->setRunning(false);

        std::this_thread::sleep_for(100ms);
        listenners.erase(it);
        Application::info("%s: client url: `%s'", __FUNCTION__, clientUrl.c_str());
    }
}

void LTSM::ChannelClient::sendSystemChannelOpen(uint8_t channel, const Channel::UrlMode & clientOpts, const Channel::Opts & chOpts)
{
    Application::info("%s: id: %" PRId8 ", path: `%s'", __FUNCTION__, channel, clientOpts.content.c_str());
    JsonObjectStream jo;

    jo.push("cmd", SystemCommand::ChannelOpen);
    jo.push("id", channel);
    jo.push("type", Channel::Connector::typeString(clientOpts.type));
    jo.push("mode", Channel::Connector::modeString(clientOpts.mode));
    jo.push("speed", Channel::Connector::speedString(chOpts.speed));
    jo.push("packed", (int) chOpts.pack);

    if(clientOpts.type == Channel::ConnectorType::Socket)
    {
        auto [ ipaddr, port ] = Channel::TcpConnector::parseAddrPort(clientOpts.content);
        jo.push("port", port);
        jo.push("ipaddr", ipaddr);
    }
    else
    if(clientOpts.type == Channel::ConnectorType::Command)
    {
        jo.push("runcmd", clientOpts.content);
    }
    else
    {
        jo.push("path", clientOpts.content);
    }

    sendLtsmEvent(Channel::System, jo.flush());
}

void LTSM::ChannelClient::sendSystemChannelError(uint8_t channel, int code, const std::string & err)
{
    sendLtsmEvent(Channel::System, JsonObjectStream().push("cmd", SystemCommand::ChannelError).push("id", channel).push("code", code).push("error", err).flush());
}

void LTSM::ChannelClient::sendSystemChannelClose(uint8_t channel)
{
    sendLtsmEvent(Channel::System, JsonObjectStream().push("cmd", SystemCommand::ChannelClose).push("id", channel).flush());
}

void LTSM::ChannelClient::sendSystemChannelConnected(uint8_t channel, int pack, bool noerror)
{
    sendLtsmEvent(Channel::System, JsonObjectStream().
        push("cmd", SystemCommand::ChannelConnected).
        push("packed", pack).
        push("error", ! noerror).
        push("id", channel).flush());
}

void LTSM::ChannelClient::recvLtsm(const NetworkStream & ns)
{
    int version = ns.recvInt8();
    if(version != LtsmProtocolVersion)
    {
        Application::error("%s: unknown version: 0x%02x", __FUNCTION__, version);
        throw std::runtime_error(NS_FuncName);
    }

    auto channel = ns.recvInt8();
    auto length = ns.recvIntBE16();
    Application::debug("%s: id: %" PRId8 ", data size: %" PRIu16, __FUNCTION__, channel, length);

    auto buf = ns.recvData(length);

    if(channelDebug == channel)
    {
        auto str = Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2);
        Application::info("%s: id: %" PRId8 ", size: %" PRIu16 ", content: [%s]", __FUNCTION__, channel, length, str.c_str());
    }

    recvLtsmEvent(channel, std::move(buf));
}

void LTSM::ChannelClient::sendLtsm(NetworkStream & ns, std::mutex & sendLock,
                                    uint8_t channel, const uint8_t* buf, size_t len)
{
    Application::debug("%s: id: %" PRId8 ", data size: %u", __FUNCTION__, channel, len);

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
        auto str = Tools::buffer2hexstring<uint8_t>(buf, len, 2);
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
    else
    if(channelDebug == channel)
    {
        channelDebug = -1;
    }
}

void LTSM::ChannelClient::channelsShutdown(void)
{
    const std::scoped_lock guard{lockch};
    for(auto & ptr : channels)
        ptr->setRunning(false);
}

// Remote2Local
LTSM::Channel::Remote2Local::Remote2Local(uint8_t cid, int fd0, const DataPack & pack) : fd(fd0), id(cid)
{
    if(DataPack::ZLib == pack)
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
        return true;

    transfer1 += buf.size();

    if(zlib)
    {
        auto buf2 = zlib->inflateData(buf.data(), buf.size(), Z_SYNC_FLUSH);
        buf.swap(buf2);
        // Application::debug("%s: inflate, size1: %d, size2: %d", __FUNCTION__, buf.size(), buf2.size());
    }

    size_t writesz = 0;
    while(writesz < buf.size())
    {
        ssize_t real = write(fd, buf.data() + writesz, buf.size() - writesz);

        if(0 < real)
        {
            writesz += real;
            transfer2 += real;
            continue;
        }

        if(EAGAIN == errno || EINTR == errno)
            continue;

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

// Local2Remote
LTSM::Channel::Local2Remote::Local2Remote(uint8_t cid, int fd0, const DataPack & pack) : fd(fd0), id(cid)
{
    if(DataPack::ZLib == pack)
    {
        zlib = std::make_unique<ZLib::DeflateBase>(Z_BEST_SPEED + 2);
    }

    buf.reserve(0xFFFF);
}

LTSM::Channel::Local2Remote::~Local2Remote()
{
    Application::info("%s: channel: %" PRIu8 ", receive: %u byte, transfer: %u byte, error: %d", "Local2Remote", id, transfer1, transfer2, error);
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

bool LTSM::Channel::Local2Remote::readData(void)
{
    size_t dtsz = 0;

    try
    {
        if(NetworkStream::hasInput(fd))
    	    dtsz = NetworkStream::hasData(fd);
    }
    catch(const std::exception & err)
    {
        error = errno;
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        return false;
    }

    if(0 == dtsz)
    {
        buf.clear();
        return true;
    }

    buf.resize(std::min(dtsz, blocksz));
    ssize_t real = read(fd, buf.data(), buf.size());

    if(0 < real)
    {
        buf.resize(real);
        transfer1 += real;

        if(zlib)
        {
            auto buf2 = zlib->deflateData(buf.data(), buf.size(), Z_SYNC_FLUSH);
            transfer2 += buf2.size();
	    buf.swap(buf2);
            // Application::debug("%s: deflate, size1: %d, size2: %d", __FUNCTION__, buf2.size(), buf.size());
        }

        return true;
    }

    // eof
    if(0 == real)
        return false;

    if(EAGAIN == errno || EINTR == errno)
    {
        buf.clear();
        return true;
    }

    error = errno;
    return false;
}

/// Connector
LTSM::Channel::Connector::Connector(uint8_t ch, int fd0, const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : owner(& srv), mode(mod), fd(fd0)
{
    owner->sendSystemChannelConnected(ch, chOpts.pack, true);

    // start threads
    loopRunning = true;

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::ReadOnly)
    {
        localRemote = std::make_unique<Local2Remote>(ch, fd0, chOpts.pack);
        localRemote->setSpeed(chOpts.speed);
    }

    if(mode == ConnectorMode::ReadWrite || mode == ConnectorMode::WriteOnly)
    {
        remoteLocal = std::make_unique<Remote2Local>(ch, fd0, chOpts.pack);
        remoteLocal->setSpeed(chOpts.speed);
    }

    if(localRemote)
        thr = std::thread(loopReader, this);

    if(remoteLocal)
        thw = std::thread(loopWriter, this);
}

LTSM::Channel::Connector::~Connector()
{
    if(loopRunning)
        loopRunning = false;

    if(thr.joinable())
        thr.join();

    if(thw.joinable())
        thw.join();

    if(0 <= fd)
        close(fd);
}

int LTSM::Channel::Connector::error(void) const
{
    int err1 = remoteLocal ? remoteLocal->getError() : 0;
    int err2 = localRemote ? localRemote->getError() : 0;

    return err1 ? err1 : err2;
}

uint8_t LTSM::Channel::Connector::channel(void) const
{
    if(remoteLocal)
        return remoteLocal->cid();

    if(localRemote)
        return localRemote->cid();

    return 0;
}

bool LTSM::Channel::Connector::isRunning(void) const
{
    return loopRunning;
}

bool LTSM::Channel::Connector::isRemoteConnected(void) const
{
    return remoteConnected;
}

void LTSM::Channel::Connector::setRunning(bool f)
{
    loopRunning = f;
}

void LTSM::Channel::Connector::setRemoteConnected(bool f)
{
    remoteConnected = f;
}

void LTSM::Channel::Connector::setSpeed(const Channel::Speed & speed)
{
    if(localRemote)
        localRemote->setSpeed(speed);

    if(remoteLocal)
        remoteLocal->setSpeed(speed);
}

void LTSM::Channel::Connector::pushData(std::vector<uint8_t> && buf)
{
    if(! buf.empty() && remoteLocal)
        remoteLocal->pushData(std::move(buf));
}

void LTSM::Channel::Connector::loopWriter(Connector* cn)
{
    bool error = false;
    auto st = cn->remoteLocal.get();

    while(cn->loopRunning)
    {
        if(st->isEmpty())
        {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        }

        if(! st->writeData())
        {
            error = true;
            cn->loopRunning = false;
        }
    }

    if(error)
    {
        if(cn->owner)
            cn->owner->sendSystemChannelError(st->cid(), st->getError(), std::string(__FUNCTION__).append(": ").append(strerror(st->getError())));

        Application::error("%s: id: %" PRId8 ", error: %s", __FUNCTION__, st->cid(), strerror(st->getError()));
    }
    else
    {
        // all data write
        while(! st->isEmpty())
        {
            if(! st->writeData()) break;
        }
    }

    // read/write priority send
    if(cn->mode != ConnectorMode::ReadWrite || cn->mode == ConnectorMode::WriteOnly)
        cn->owner->sendSystemChannelClose(st->cid());
}

void LTSM::Channel::Connector::loopReader(Connector* cn)
{
    bool error = false;
    auto st = cn->localRemote.get();

    while(cn->loopRunning)
    {
        if(! st->readData())
        {
            error = true;
            cn->loopRunning = false;
        }

        if(st->getBuf().empty())
        {
            std::this_thread::sleep_for(st->getDelay());
            continue;
        }
        else
        if(cn->owner)
        {
            auto & buf = st->getBuf();
            cn->owner->sendLtsmEvent(st->cid(), buf.data(), buf.size());
        }
    }

    if(error)
    {
        if(cn->owner)
            cn->owner->sendSystemChannelError(st->cid(), st->getError(), std::string(__FUNCTION__).append(": ").append(strerror(st->getError())));

        Application::error("%s: id: %" PRId8 ", error: %s", __FUNCTION__, st->cid(), strerror(st->getError()));
    }

    if(cn->owner)
    {
	// read/write priority send
        if(cn->mode == ConnectorMode::ReadWrite || cn->mode == ConnectorMode::ReadOnly)
    	    cn->owner->sendSystemChannelClose(st->cid());
    }
}

/// UnixConnector
std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::UnixConnector::createConnector(uint8_t channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
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

    return std::make_unique<Connector>(channel, fd, mode, chOpts, sender);
}

std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::UnixConnector::createConnector(uint8_t channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "unix failed", channel);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Connector>(channel, sock, mode, chOpts, sender);
}

/// TcpConnector
std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::TcpConnector::createConnector(uint8_t channel, std::string_view ipaddr, int port, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", addr: %s, port: %d, mode: %s", __FUNCTION__, channel, ipaddr.data(), port, Channel::Connector::modeString(mode));

    int fd = TCPSocket::connect(ipaddr, port);
    if(0 > fd)
    {
        Application::error("%s: %s, id: %" PRId8 ", addr: %s, port: %d", __FUNCTION__, "socket failed", channel, ipaddr.data(), port);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Connector>(channel, fd, mode, chOpts, sender);
}

std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::TcpConnector::createConnector(uint8_t channel, int sock, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", sock: %d, mode: %s", __FUNCTION__, channel, sock, Channel::Connector::modeString(mode));

    if(0 > sock)
    {
        Application::error("%s: %s, id: %" PRId8, __FUNCTION__, "socket failed", channel);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Connector>(channel, sock, mode, chOpts, sender);
}

/// FileConnector
std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::FileConnector::createConnector(uint8_t channel, const std::filesystem::path & path, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
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
        fd = open(path.c_str(), O_RDONLY);
    }
    else
    if(mode == ConnectorMode::WriteOnly)
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

        fd = open(path.c_str(), flags, S_IRUSR|S_IWUSR|S_IRGRP);
    }

    if(0 > fd)
    {
        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open file", strerror(errno), errno);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Connector>(channel, fd, mode, chOpts, sender);
}

/// CommandConnector
LTSM::Channel::CommandConnector::CommandConnector(uint8_t channel, FILE* ptr, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & owner)
    : Connector(channel, fileno(ptr), mode, chOpts, owner), fcmd{ptr, pclose}
{
}

std::unique_ptr<LTSM::Channel::Connector>
    LTSM::Channel::CommandConnector::createConnector(uint8_t channel, const std::string & runcmd, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
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
        list.push_front(cmd.native());
        auto runcmd2 = Tools::join(list, " ");

        fcmd = popen(runcmd2.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    }
    else
    if(std::filesystem::is_regular_file(list.front(), err))
    {
        fcmd = popen(runcmd.c_str(), (mode == ConnectorMode::ReadOnly ? "r" : "w"));
    }

    if(! fcmd)
    {
        Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "open cmd", strerror(errno), errno);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<CommandConnector>(channel, fcmd, mode, chOpts, sender);
}

/// Listener
LTSM::Channel::Listener::Listener(int fd, const UrlMode & serverOpts, const UrlMode & clientOpts, const Channel::Opts & ch, ChannelClient & sender)
    : sopts(serverOpts), copts(clientOpts), owner(& sender), chopts(ch), srvfd(fd)
{
    loopRunning = true;
    th = std::thread(loopAccept, this);
}

LTSM::Channel::Listener::~Listener()
{
    loopRunning = false;

    if(th.joinable())
        th.join();

    if(0 <= srvfd)
        close(srvfd);

    if(isUnix())
    {
        try
        {
            if(std::filesystem::exists(sopts.content) && std::filesystem::is_socket(sopts.content))
                std::filesystem::remove(sopts.content);
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

            Application::error("%s: exception: %s", __FUNCTION__, err.what());
        }

        if(input)
        {
            auto sock = st->isUnix() ?
                UnixSocket::accept(st->srvfd) : TCPSocket::accept(st->srvfd);

            if(sock < 0)
            {
                st->loopRunning = false;
            }
            else
            if(! st->owner->createChannelFromListenerFd(st->copts, sock, st->sopts, st->chopts))
                close(sock);
        }
        else
        {
            std::this_thread::sleep_for(250ms);
        }
    }
}

std::unique_ptr<LTSM::Channel::Listener>
    LTSM::Channel::UnixListener::createListener(const UrlMode & serverOpts, size_t listen,
        const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelClient & sender)
{
    auto & path = serverOpts.content;
    std::error_code err;

    if(std::filesystem::exists(path, err))
    {
        if(std::filesystem::is_socket(path, err))
        {
            Application::warning("%s: %s, path: `%s'", __FUNCTION__, "socket present", path.c_str());
            std::filesystem::remove(path);
        }
        else
        {
            Application::error("%s: %s, path: `%s'", __FUNCTION__, "file present", path.c_str());
            throw channel_error(NS_FuncName);
        }
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
    LTSM::Channel::TcpListener::createListener(const UrlMode & serverOpts, size_t listen,
        const UrlMode & clientOpts, const Channel::Opts & chOpts, ChannelClient & sender)
{
    auto [ ipaddr, port ] = Channel::TcpConnector::parseAddrPort(serverOpts.content);
    if(0 >= port)
    {
        Application::error("%s: %s, url: `%s'", __FUNCTION__, "socket format", serverOpts.content.data());
        throw channel_error(NS_FuncName);
    }

    // hardcore server listen
    if(sender.serverSide())
        ipaddr = "127.0.0.1";

    int srvfd = TCPSocket::listen(ipaddr, port, listen);
    if(0 > srvfd)
    {
        Application::error("%s: %s, ipaddr: %s, port: %d", __FUNCTION__, "socket failed", ipaddr.c_str(), port);
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<Listener>(srvfd, serverOpts, clientOpts, chOpts, sender);
}
