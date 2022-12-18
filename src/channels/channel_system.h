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

#ifndef _CHANNEL_SYSTEM_
#define _CHANNEL_SYSTEM_

#include <list>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <utility>
#include <filesystem>

#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    static const int LtsmProtocolVersion = 0x01;

    namespace SystemCommand
    {
        static const std::string_view ChannelOpen{"ChannelOpen"};
        static const std::string_view ChannelListen{"ChannelListen"};
        static const std::string_view ChannelConnected{"ChannelConnected"};
        static const std::string_view ChannelClose{"ChannelClose"};
        static const std::string_view ChannelError{"ChannelError"};
        static const std::string_view ClientVariables{"ClientVariables"};
        static const std::string_view TransferFiles{"TransferFiles"};
        static const std::string_view KeyboardChange{"KeyboardChange"};
        static const std::string_view FuseProxy{"FuseProxy"};
        static const std::string_view TokenAuth{"TokenAuth"};
        static const std::string_view LoginSuccess{"LoginSuccess"};
    }

    class ChannelClient;

    /// channel_error execption
    struct channel_error : public std::runtime_error
    {   
        explicit channel_error(const std::string &  what) : std::runtime_error(what){}
        explicit channel_error(const char* what) : std::runtime_error(what){}
    };

    namespace Channel
    {
        enum { System = 0, Reserved = 0xFF };

        enum class ConnectorType { Unknown, Unix, Socket, File, Command };
        enum class ConnectorMode { Unknown, ReadOnly, ReadWrite, WriteOnly };

        // UltraSlow: ~10k/sec, ~40k/sec, ~80k/sec, ~800k/sec, ~1600k/sec
        enum class Speed { VerySlow, Slow, Medium, Fast, UltraFast };

        ConnectorType connectorType(std::string_view);
        ConnectorMode connectorMode(std::string_view);
        Speed connectorSpeed(std::string_view);

        std::pair<ConnectorType, std::string> parseUrl(std::string_view);
        std::string createUrl(const ConnectorType &, std::string_view);

        struct TypeContent
        {
            ConnectorType type = ConnectorType::Unknown;
            std::string content;

            TypeContent(const std::pair<ConnectorType, std::string> & pair) : type(pair.first), content(pair.second) {}
            TypeContent(const ConnectorType & typ, const std::string & body) : type(typ), content(body) {}
        };

        struct UrlMode : TypeContent
        {
            ConnectorMode mode = ConnectorMode::Unknown;
            std::string url;

            UrlMode(std::string_view str, std::string_view mod) : TypeContent(parseUrl(str)), mode(connectorMode(mod)), url(str) {}
            UrlMode(const ConnectorType & typ, const std::string & body, const ConnectorMode & mod) : TypeContent(typ, body), mode(mod), url(createUrl(typ, body)) {}
        };

        struct Opts
        {
            Speed speed = Speed::Medium;
            bool zlib = false;
        };

        struct Planned
        {
            UrlMode serverOpts;
            UrlMode clientOpts;
            Opts chOpts;
            int serverFd = -1;
            uint8_t channel = 0;
        };

        class Connector
        {
            std::vector<uint8_t> bufr;
            std::list<std::vector<uint8_t>> bufw;
            std::mutex  lockw;

            std::thread thr;
            std::thread thw;

            std::chrono::milliseconds delay{100};

            std::atomic<bool> loopRunning{false};
            std::atomic<bool> remoteConnected{false};

            ChannelClient* owner = nullptr;
            ConnectorMode mode = ConnectorMode::Unknown;

            int         err = 0;
            int         fd = -1;
            uint16_t    blocksz = 4096;
            uint8_t     id = 255;

#ifdef LTSM_SOCKET_ZLIB
            std::unique_ptr<ZLib::DeflateInflate> zlib;
#endif

        public:
            static const char* typeString(const ConnectorType &);
            static const char* modeString(const ConnectorMode &);
            static const char* speedString(const Speed &);

        protected:
            bool        local2remote(void);
            bool        remote2local(void);

            static void loopWriter(Connector*);
            static void loopReader(Connector*);

            void        startThreads(const ConnectorMode &);

        public:
            Connector(uint8_t channel, int fd, const ConnectorMode &, const Opts &, ChannelClient &);
            virtual ~Connector();

            uint8_t     channel(void) const { return id; }
            int         error(void) const { return err; }

            void        setSpeed(const Channel::Speed &);

            bool        isRunning(void) const;
            void        setRunning(bool);

            bool        isRemoteConnected(void) const;
            void        setRemoteConnected(bool);

            void        pushData(std::vector<uint8_t> &&);
        };

        namespace UnixConnector
        {
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, int fd, const ConnectorMode &, const Opts &, ChannelClient &);
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelClient &);
        };

        namespace TcpConnector
        {
            std::pair<std::string, int> parseAddrPort(std::string_view);

            std::unique_ptr<Connector>
                createConnector(uint8_t channel, int fd, const ConnectorMode &, const Opts &, ChannelClient &);
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, std::string_view ipaddr, int port, const ConnectorMode &, const Opts &, ChannelClient &);
        };

        namespace FileConnector
        {
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelClient &);
        };

        class CommandConnector : public Connector
        {
            std::unique_ptr<FILE, int(*)(FILE*)> fcmd;

        public:
            CommandConnector(uint8_t channel, FILE*, const ConnectorMode &, const Opts &, ChannelClient &);

            static std::unique_ptr<Connector>
                createConnector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
        };

        class Listener
        {
            std::thread th;
            std::atomic<bool> loopRunning{false};

            UrlMode        sopts;
            UrlMode        copts;

            ChannelClient* owner = nullptr;

            Opts           chopts;
            int            srvfd = -1;

        public:
            Listener(int fd, const UrlMode & srvOpts, const UrlMode & cliOpts, const Channel::Opts &, ChannelClient &);
            virtual ~Listener();

            static void loopAccept(Listener*);

            bool        isRunning(void) const;
            void        setRunning(bool);

            const std::string & getClientUrl(void) const { return copts.url; }
            bool        isUnix(void) const { return sopts.type == ConnectorType::Unix; }
        };

        namespace UnixListener
        {
            std::unique_ptr<Listener>
                createListener(const Channel::UrlMode & serverOpts, size_t listen,
                                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelClient &);
        }

        namespace TcpListener
        {
            std::unique_ptr<Listener>
                createListener(const Channel::UrlMode & serverOpts, size_t listen,
                                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelClient &);
        }
    }

    class ChannelClient
    {
        std::list<std::unique_ptr<Channel::Connector>> channels;
        std::list<std::unique_ptr<Channel::Listener>> listenners;
        std::list<Channel::Planned> channelsPlanned;

        mutable std::mutex lockch, lockpl, lockls;

        Channel::Connector* findChannel(uint8_t);
        Channel::Planned*   findPlanned(uint8_t);

        int                 channelDebug = -1;

    protected:
        friend void Channel::Listener::loopAccept(Listener*);

        void            recvLtsm(const NetworkStream &);
        void            sendLtsm(NetworkStream &, std::mutex &, uint8_t channel, const uint8_t*, size_t);

        virtual void    recvChannelSystem(const std::vector<uint8_t> &) = 0;
        void            recvChannelData(uint8_t channel, std::vector<uint8_t> &&);

        virtual bool    isUserSession(void) const { return false; }

        virtual void    systemClientVariables(const JsonObject &) {}
        virtual void    systemKeyboardChange(const JsonObject &) {}

        void            systemChannelOpen(const JsonObject &);
        void            systemChannelListen(const JsonObject &) {}
        bool            systemChannelConnected(const JsonObject &);
        void            systemChannelClose(const JsonObject &);
        virtual void    systemChannelError(const JsonObject &) {}

        virtual void    systemTransferFiles(const JsonObject &) {}
        virtual void    systemFuseProxy(const JsonObject &) {}
        virtual void    systemTokenAuth(const JsonObject &) {}
        virtual void    systemLoginSuccess(const JsonObject &) {}

        bool            createListener(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, size_t listen, const Channel::Opts &);
        bool            createChannel(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, const Channel::Opts &);

        bool            createChannelUnix(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool            createChannelUnixFd(uint8_t channel, int, const Channel::ConnectorMode &, const Channel::Opts &);
        bool            createChannelFile(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool            createChannelSocket(uint8_t channel, std::pair<std::string, int>, const Channel::ConnectorMode &, const Channel::Opts &);
        bool            createChannelSocketFd(uint8_t channel, int, const Channel::ConnectorMode &, const Channel::Opts &);
        bool            createChannelCommand(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);

        bool            createChannelFromListenerFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts &);
        void            destroyChannel(uint8_t channel);
        void            destroyListener(const std::string & clientUrl, const std::string & serverUrl);

        size_t          countFreeChannels(void) const;

        void            setChannelDebug(const uint8_t& channel, const bool& debug);
        void            channelsShutdown(void);

    public:
        void            sendLtsmEvent(uint8_t channel, std::string_view);
        void            sendLtsmEvent(uint8_t channel, const std::vector<uint8_t> &);

        void            sendSystemKeyboardChange(const std::vector<std::string> &, int);
        void            sendSystemClientVariables(const json_plain &, const json_plain &, const std::vector<std::string> &, const std::string &);
        bool            sendSystemTransferFiles(std::list<std::string>);
        void            sendSystemChannelOpen(uint8_t channel, const Channel::UrlMode &, const Channel::Opts &);
        void            sendSystemChannelClose(uint8_t channel);
        void            sendSystemChannelConnected(uint8_t channel, bool zlib, bool noerror);
        void            sendSystemChannelError(uint8_t channel, int code, const std::string &);

        void            recvLtsmEvent(uint8_t channel, std::vector<uint8_t> &&);

        virtual void    sendLtsmEvent(uint8_t channel, const uint8_t*, size_t) = 0;
        virtual bool    serverSide(void) const { return false; }
    };
}

#endif
