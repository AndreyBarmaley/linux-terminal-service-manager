/***************************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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
 ***************************************************************************/

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
        static const std::string_view ClientVariables{"ClientVariables"};
        static const std::string_view TransferFiles{"TransferFiles"};
        static const std::string_view KeyboardChange{"KeyboardChange"};
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

        enum class ConnectorType { Unknown, Unix, Socket, File };
        enum class ConnectorMode { Unknown, ReadOnly, ReadWrite, WriteOnly, Default };

        // UltraSlow: ~4k/sec, ~16k/sec, ~40k/sec, ~200k/sec, ~800k/sec
        enum class Speed { UltraSlow, Slow, Medium, Fast, UltraFast };

        ConnectorType connectorType(std::string_view);
        ConnectorMode connectorMode(std::string_view);

        std::pair<ConnectorType, std::string> parseUrl(std::string_view);
        std::string createUrl(const ConnectorType &, std::string_view);

        struct Planned
        {
            ConnectorType serverType = ConnectorType::Unknown;
            ConnectorType clientType = ConnectorType::Unknown;
            ConnectorMode serverMode = ConnectorMode::Default;
            ConnectorMode clientMode = ConnectorMode::Default;
            std::string serverUrl;
            std::string clientUrl;
            int serverFd = -1;
            bool serverUnix = false;
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

        public:
            static const char* typeString(const ConnectorType &);
            static const char* modeString(const ConnectorMode &);

        protected:
            bool        local2remote(void);
            bool        remote2local(void);

            static void loopWriter(Connector*);
            static void loopReader(Connector*);

            void        startThreads(const ConnectorMode &);

        public:
            Connector(uint8_t channel, int fd, const ConnectorMode &, ChannelClient &);
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
                createConnector(uint8_t channel, int fd, ChannelClient &, ConnectorMode mode = ConnectorMode::ReadWrite);
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, const std::filesystem::path &, ChannelClient &, ConnectorMode mode = ConnectorMode::ReadWrite);
        };

        namespace TcpConnector
        {
            std::pair<std::string, int> parseUrl(std::string_view);

            std::unique_ptr<Connector>
                createConnector(uint8_t channel, int fd, ChannelClient &, ConnectorMode mode = ConnectorMode::ReadWrite);
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, std::string_view ipaddr, int port, ChannelClient &, ConnectorMode mode = ConnectorMode::ReadWrite);
        };

        namespace FileConnector
        {
            std::unique_ptr<Connector>
                createConnector(uint8_t channel, const std::filesystem::path &, ChannelClient &, ConnectorMode mode = ConnectorMode::ReadOnly);
        };

        class Listener
        {
            std::thread th;
            std::atomic<bool> loopRunning{false};

            std::string    clientUrl;
            Channel::ConnectorMode clientMode = ConnectorMode::Default;
            Channel::ConnectorMode serverMode = ConnectorMode::Default;

            ChannelClient* owner = nullptr;

            int            srvfd = -1;
            bool           isUnix = false;

        public:
            Listener(int fd, bool isunix, ChannelClient &, const Channel::ConnectorMode & smode, const std::string & curl, const Channel::ConnectorMode & cmode);
            virtual ~Listener();

            static void loopAccept(Listener*);

            bool        isRunning(void) const;
            void        setRunning(bool);

            const std::string & getClientUrl(void) const { return clientUrl; }
        };

        namespace UnixListener
        {
            std::unique_ptr<Listener>
                createListener(const std::filesystem::path &, int listen, ChannelClient &,
                                const Channel::ConnectorMode & smode, const std::string & curl, const Channel::ConnectorMode & cmode);
        } 

        namespace TcpListener
        {
            std::unique_ptr<Listener>
                createListener(std::string_view url, int listen, ChannelClient &,
                                const Channel::ConnectorMode & smode, const std::string & curl, const Channel::ConnectorMode & cmode);
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
        void            systemChannelListen(const JsonObject &);
        bool            systemChannelConnected(const JsonObject &);
        void            systemChannelClose(const JsonObject &);

        virtual void    systemTransferFiles(const JsonObject &) {}

        bool            createListener(const std::string & client, const Channel::ConnectorMode & cmode, const std::string& server, const Channel::ConnectorMode & smode);
        bool            createChannel(const std::string& client, const Channel::ConnectorMode & cmode, const std::string& server, const Channel::ConnectorMode & smode);
        bool            createChannelUnix(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &);
        bool            createChannelUnix(uint8_t channel, int, const Channel::ConnectorMode &);
        bool            createChannelFile(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &);
        bool            createChannelSocket(uint8_t channel, std::pair<std::string, int>, const Channel::ConnectorMode &);
        bool            createChannelSocket(uint8_t channel, int, const Channel::ConnectorMode &);

        bool            createChannelFromListener(const std::string& client, const Channel::ConnectorMode & cmode, bool isunix, int sock, const Channel::ConnectorMode & smode);
        void            destroyChannel(uint8_t channel);
        void            destroyListener(const std::string & clientUrl, const std::string & serverUrl);

        size_t          countFreeChannels(void) const;

    public:
        void            sendLtsmEvent(uint8_t channel, std::string_view);
        void            sendLtsmEvent(uint8_t channel, const std::vector<uint8_t> &);

        void            sendSystemKeyboardChange(const std::vector<std::string> &, int);
        void            sendSystemClientVariables(const json_plain &, const json_plain &, const std::vector<std::string> &);
        bool            sendSystemTransferFiles(std::list<std::string>);
        void            sendSystemChannelOpen(uint8_t channel, const Channel::ConnectorType &, std::string_view path, const Channel::ConnectorMode &);
        void            sendSystemChannelClose(uint8_t channel);
        void            sendSystemChannelConnected(uint8_t channel, bool noerror);

        void            recvLtsmEvent(uint8_t channel, std::vector<uint8_t> &&);

        virtual void    sendLtsmEvent(uint8_t channel, const uint8_t*, size_t) = 0;
        virtual bool    serverSide(void) const { return false; }
    };
}

#endif
