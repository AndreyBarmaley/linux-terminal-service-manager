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
#include <forward_list>

#ifdef LTSM_CLIENT
 #include "ltsm_audio.h"
 #include "ltsm_audio_decoder.h"
#endif

#ifdef LTSM_PKCS11_AUTH
 #include "ltsm_pkcs11_wrapper.h"
#endif

#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    static const int LtsmProtocolVersion = 0x03;

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
        static const std::string_view LoginSuccess{"LoginSuccess"};
    }

    class ChannelClient;
    class ChannelListener;

    /// channel_error execption
    struct channel_error : public std::runtime_error
    {
        explicit channel_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    enum class ChannelType : uint8_t { System = 0, Reserved = 0xFF };

    namespace Channel
    {
        enum class ConnectorType { Unknown, Unix, Socket, File, Command, Fuse, Audio, Pcsc, Pkcs11 };
        enum class ConnectorMode { Unknown, ReadOnly, ReadWrite, WriteOnly };

        // UltraSlow: ~10k/sec, ~40k/sec, ~80k/sec, ~800k/sec, ~1600k/sec
        enum class Speed { VerySlow, Slow, Medium, Fast, UltraFast };

        ConnectorType connectorType(std::string_view);
        ConnectorMode connectorMode(std::string_view);
        Speed connectorSpeed(std::string_view);

        std::pair<ConnectorType, std::string> parseUrl(const std::string &);
        std::string createUrl(const ConnectorType &, std::string_view);

        struct TypeContent : std::pair<ConnectorType, std::string>
        {
            explicit TypeContent(const std::pair<ConnectorType, std::string> & pair) : std::pair<ConnectorType, std::string>(pair) {}
            explicit TypeContent(std::pair<ConnectorType, std::string> && pair) noexcept : std::pair<ConnectorType, std::string>(std::move(pair)) {}

            TypeContent() : std::pair<ConnectorType, std::string>{ConnectorType::Unknown, ""} {}
            TypeContent(const ConnectorType & type, const std::string & cont) : std::pair<ConnectorType, std::string>{type, cont} {}

            const ConnectorType & type(void) const { return first; }
            const std::string & content(void) const { return second; }
        };

        struct UrlMode : TypeContent
        {
            ConnectorMode mode = ConnectorMode::Unknown;
            std::string url;

            UrlMode(const std::string & str, std::string_view mod) : TypeContent(parseUrl(str)), mode(connectorMode(mod)), url(str) {}
            UrlMode(const ConnectorType & typ, const std::string & body, const ConnectorMode & mod) : TypeContent(typ, body), mode(mod), url(createUrl(typ, body)) {}
        };

        enum class OptsFlags : uint32_t { ZLibCompression = 1, AllowLoginSession = 2 };

        struct Opts
        {
            Speed speed = Speed::Medium;
            int flags = 0;
        };

        struct Planned
        {
            UrlMode serverOpts;
            UrlMode clientOpts;
            Opts chOpts;
            int serverFd = -1;
            uint8_t channel = 0;
        };

        // Local2Remote
        class Local2Remote
        {
        protected:
            std::vector<uint8_t> buf;

            std::chrono::milliseconds delay{100};

            std::unique_ptr<ZLib::DeflateBase> zlib;

            size_t transfer1 = 0;
            size_t transfer2 = 0;
            size_t blocksz = 4096;

            int error = 0;
            uint8_t id = 255;

            bool sendData(void);

        public:
            Local2Remote(uint8_t cid, int flags);
            virtual ~Local2Remote();

            virtual bool hasInput(void) const = 0;
            virtual size_t hasData(void) const = 0;
            virtual ssize_t readDataTo(void* buf, size_t len) = 0;

            bool readData(void);
            void setSpeed(const Channel::Speed &);

            uint8_t cid(void) const { return id; }

            int getError(void) const { return error; }

            std::chrono::milliseconds getDelay(void) const { return delay; }

            const std::vector<uint8_t> & getBuf(void) const { return buf; }
        };

        /// Local2Remote_FD
        class Local2Remote_FD : public Local2Remote
        {
            int fd = -1;
            bool needClose = true;

        public:
            Local2Remote_FD(uint8_t cid, int fd0, bool close, int flags);
            ~Local2Remote_FD();

            bool hasInput(void) const override;
            size_t hasData(void) const override;
            ssize_t readDataTo(void* buf, size_t len) override;
        };

        // Remote2Local
        class Remote2Local
        {
        protected:
            std::list<std::vector<uint8_t>> queueBufs;
            std::mutex lockQueue;

            std::chrono::milliseconds delay{100};

            std::unique_ptr<ZLib::InflateBase> zlib;

            size_t transfer1 = 0;
            size_t transfer2 = 0;

            int error = 0;
            uint8_t id = 255;

        protected:
            std::vector<uint8_t> popData(void);

        public:
            Remote2Local(uint8_t cid, int flags);
            virtual ~Remote2Local();

            virtual ssize_t writeDataFrom(const void* buf, size_t len) = 0;

            void pushData(std::vector<uint8_t> &&);
            bool writeData(void);
            void setSpeed(const Channel::Speed &);

            uint8_t cid(void) const { return id; }

            int getError(void) const { return error; }

            bool isEmpty(void) const { return queueBufs.empty(); }

            std::chrono::milliseconds getDelay(void) const { return delay; }
        };

        /// Remote2Local_FD
        class Remote2Local_FD : public Remote2Local
        {
            int fd = -1;
            bool needClose = true;

        public:
            Remote2Local_FD(uint8_t cid, int fd0, bool close, int flags);
            ~Remote2Local_FD();

            ssize_t writeDataFrom(const void* buf, size_t len) override;
        };

        /// ConnectorBase
        class ConnectorBase
        {
        private:
            std::atomic<bool> loopRunning{false};
            std::atomic<bool> remoteConnected{false};

        protected:
            ChannelClient* owner = nullptr;
            ConnectorMode mode = ConnectorMode::Unknown;
        public:
            int flags = 0;

        public:
            ConnectorBase(uint8_t ch, const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv);
            virtual ~ConnectorBase() = default;

            virtual uint8_t channel(void) const = 0;
            virtual int error(void) const = 0;

            virtual void setSpeed(const Channel::Speed &) = 0;
            virtual void pushData(std::vector<uint8_t> &&) = 0;

            bool isAllowSessionFor(bool user) const;
            bool isRunning(void) const;
            void setRunning(bool);

            bool isRemoteConnected(void) const;
            void setRemoteConnected(bool);

            ChannelClient* getOwner(void) { return owner; }

            bool isMode(ConnectorMode cm) const { return mode == cm; }
        };

        typedef std::unique_ptr<ConnectorBase> ConnectorBasePtr;

        // ConnectorFD_R
        class ConnectorFD_R : public ConnectorBase
        {
            std::unique_ptr<Local2Remote> localRemote;
            std::thread thr;

        public:
            ConnectorFD_R(uint8_t channel, int fd, bool close, const Opts &, ChannelClient &);
            virtual ~ConnectorFD_R();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override { /* skipped */ }
        };

        // ConnectorFD_W
        class ConnectorFD_W : public ConnectorBase
        {
            std::unique_ptr<Remote2Local> remoteLocal;
            std::thread thw;

        public:
            ConnectorFD_W(uint8_t channel, int fd, bool close, const Opts &, ChannelClient &);
            virtual ~ConnectorFD_W();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        // ConnectorFD_RW
        class ConnectorFD_RW : public ConnectorBase
        {
            std::unique_ptr<Remote2Local> remoteLocal;
            std::unique_ptr<Local2Remote> localRemote;

            std::thread thr;
            std::thread thw;

        public:
            ConnectorFD_RW(uint8_t channel, int fd, const Opts &, ChannelClient &);
            virtual ~ConnectorFD_RW();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        // ConnectorCMD_R
        class ConnectorCMD_R : public ConnectorFD_R
        {
            FILE* fcmd = nullptr;

        public:
            ConnectorCMD_R(uint8_t channel, FILE*, const Opts &, ChannelClient &);
            virtual ~ConnectorCMD_R();
        };

        // ConnectorCMD_W
        class ConnectorCMD_W : public ConnectorFD_W
        {
            FILE* fcmd = nullptr;

        public:
            ConnectorCMD_W(uint8_t channel, FILE*, const Opts &, ChannelClient &);
            virtual ~ConnectorCMD_W();
        };

#ifdef LTSM_CLIENT
        /// ConnectorClientFuse
        class ConnectorClientFuse : public ConnectorBase
        {
            StreamBuf reply;
            std::forward_list<int> opens;
            std::string shareRoot;

            bool fuseInit = false;
            uint16_t fuseVer = 0;
            uint8_t cid = 255;

            std::vector<uint8_t> last;

        protected:
            bool fuseOpInit(const StreamBufRef &);
            bool fuseOpQuit(const StreamBufRef &);
            bool fuseOpGetAttr(const StreamBufRef &);
            bool fuseOpReadDir(const StreamBufRef &);
            bool fuseOpOpen(const StreamBufRef &);
            bool fuseOpRelease(const StreamBufRef &);
            bool fuseOpRead(const StreamBufRef &);
            bool fuseOpLookup(const StreamBufRef &);

        public:
            ConnectorClientFuse(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
            virtual ~ConnectorClientFuse();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        /// ConnectorClientAudio
        class ConnectorClientAudio : public ConnectorBase
        {
            std::forward_list<AudioFormat> formats;
            const AudioFormat* format = nullptr;

            uint16_t audioVer = 0;
            uint8_t cid = 255;

            std::unique_ptr<AudioPlayer> player;
            std::unique_ptr<AudioDecoder::BaseDecoder> decoder;
            std::vector<uint8_t> last;

        protected:
            bool audioOpInit(const StreamBufRef &);
            void audioOpData(const StreamBufRef &);
            void audioOpSilent(const StreamBufRef &);

        public:
            ConnectorClientAudio(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
            virtual ~ConnectorClientAudio();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        /// ConnectorClientPcsc
        class ConnectorClientPcsc : public ConnectorBase
        {
            //uint16_t    pcscVer = 0;
            uint8_t cid = 255;

            std::vector<uint8_t> last;

        protected:
            void pcscEstablishContext(const StreamBufRef &);
            void pcscReleaseContext(const StreamBufRef &);
            void pcscListReaders(const StreamBufRef &);
            void pcscConnect(const StreamBufRef &);
            void pcscReconnect(const StreamBufRef &);
            void pcscDisconnect(const StreamBufRef &);
            void pcscBeginTransaction(const StreamBufRef &);
            void pcscEndTransaction(const StreamBufRef &);
            void pcscTransmit(const StreamBufRef &);
            void pcscStatus(const StreamBufRef &);
            void pcscGetStatusChange(const StreamBufRef &);
            void pcscControl(const StreamBufRef &);
            void pcscCancel(const StreamBufRef &);
            void pcscGetAttrib(const StreamBufRef &);
            void pcscSetAttrib(const StreamBufRef &);

        public:
            ConnectorClientPcsc(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
            virtual ~ConnectorClientPcsc();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

#ifdef __UNIX__
        /// ConnectorClientPkcs11
        class ConnectorClientPkcs11 : public ConnectorBase
        {
            StreamBuf reply;

#ifdef LTSM_PKCS11_AUTH
            PKCS11::LibraryPtr pkcs11;
#endif
            std::vector<uint8_t> last;

            uint16_t protoVer = 0;
            uint8_t cid = 255;

        protected:
            bool pkcs11Init(const StreamBufRef &);
            bool pkcs11GetSlots(const StreamBufRef &);
            bool pkcs11GetSlotMechanisms(const StreamBufRef &);
            bool pkcs11GetSlotCertificates(const StreamBufRef &);
            bool pkcs11SignData(const StreamBufRef &);
            bool pkcs11DecryptData(const StreamBufRef &);

        public:
            ConnectorClientPkcs11(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
            virtual ~ConnectorClientPkcs11();

            uint8_t channel(void) const override;
            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };
#endif // LINUX

#endif // LTSM_CLIENT

        namespace Connector
        {
            const char* typeString(const ConnectorType &);
            const char* modeString(const ConnectorMode &);
            const char* speedString(const Speed &);
        }

#ifdef __UNIX__
        ConnectorBasePtr createUnixConnector(uint8_t channel, int fd, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createUnixConnector(uint8_t channel, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelClient &);

        ConnectorBasePtr createTcpConnector(uint8_t channel, int fd, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createTcpConnector(uint8_t channel, const std::string & ipaddr, int port, const ConnectorMode &, const Opts &, ChannelClient &);

        ConnectorBasePtr createClientPkcs11Connector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
#endif
        ConnectorBasePtr createClientPcscConnector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createClientFuseConnector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createClientAudioConnector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createFileConnector(uint8_t channel, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelClient &);
        ConnectorBasePtr createCommandConnector(uint8_t channel, const std::string &, const ConnectorMode &, const Opts &, ChannelClient &);

#ifdef __UNIX__
        /// Listener
        class Listener
        {
            std::thread th;
            std::atomic<bool> loopRunning{false};

            UrlMode sopts;
            UrlMode copts;

            ChannelListener* owner = nullptr;

            Opts chopts;
            int srvfd = -1;

        public:
            Listener(int fd, const UrlMode & srvOpts, const UrlMode & cliOpts, const Channel::Opts &, ChannelListener &);
            virtual ~Listener();

            static void loopAccept(Listener*);

            bool isRunning(void) const;
            void setRunning(bool);

            const std::string & getClientUrl(void) const { return copts.url; }

            const std::string & getServerUrl(void) const { return sopts.url; }

            bool isUnix(void) const { return sopts.type() == ConnectorType::Unix; }
        };

        std::unique_ptr<Listener> createUnixListener(const Channel::UrlMode & serverOpts, size_t listen,
                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelListener &);
        std::unique_ptr<Listener> createTcpListener(const Channel::UrlMode & serverOpts, size_t listen,
                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelListener &);
#endif
    }

    class ChannelClient
    {
    protected:
        std::list<std::unique_ptr<Channel::ConnectorBase>> channels;
        std::list<Channel::Planned> channelsPlanned;

        mutable std::mutex lockch, lockpl;

        int channelDebug = -1;

    protected:
        Channel::ConnectorBase* findChannel(uint8_t);
        Channel::Planned* findPlanned(uint8_t);

        void recvLtsmProto(const NetworkStream &);
        void sendLtsmProto(NetworkStream &, std::mutex &, uint8_t channel, const uint8_t*, size_t);

        virtual void recvChannelSystem(const std::vector<uint8_t> &) = 0;
        void recvChannelData(uint8_t channel, std::vector<uint8_t> &&);

        virtual bool isUserSession(void) const { return false; }

        virtual void systemClientVariables(const JsonObject &) { /* empty */ }

        virtual void systemKeyboardChange(const JsonObject &) { /* empty */ }

        void systemChannelOpen(const JsonObject &);
        void systemChannelListen(const JsonObject &) { /* empty */ }

        bool systemChannelConnected(const JsonObject &);
        void systemChannelClose(const JsonObject &);
        virtual void systemChannelError(const JsonObject &) { /* empty */ }

        virtual void systemTransferFiles(const JsonObject &) { /* empty */ }

        virtual void systemLoginSuccess(const JsonObject &) { /* empty */ }

        bool createChannel(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, const Channel::Opts &);
        void destroyChannel(uint8_t channel);

#ifdef __UNIX__
        bool createChannelUnix(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelUnixFd(uint8_t channel, int, const Channel::ConnectorMode &, const Channel::Opts &);

        bool createChannelSocket(uint8_t channel, std::pair<std::string, int>, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelSocketFd(uint8_t channel, int, const Channel::ConnectorMode &, const Channel::Opts &);
#endif
        bool createChannelFile(uint8_t channel, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelCommand(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelClientFuse(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelClientAudio(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelClientPcsc(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelClientPkcs11(uint8_t channel, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);

        size_t countFreeChannels(void) const;

        void setChannelDebug(const uint8_t & channel, const bool & debug);
        void channelsShutdown(void);

    public:
        ChannelClient() = default;
        virtual ~ChannelClient() = default;

        void sendLtsmChannelData(uint8_t channel, std::string_view);
        void sendLtsmChannelData(uint8_t channel, const std::vector<uint8_t> &);

        void sendSystemKeyboardChange(const std::vector<std::string> &, int);
        void sendSystemClientVariables(const json_plain &, const json_plain &, const std::vector<std::string> &, const std::string &);
        bool sendSystemTransferFiles(std::forward_list<std::string>);
        void sendSystemChannelOpen(uint8_t channel, const Channel::UrlMode &, const Channel::Opts &);
        void sendSystemChannelClose(uint8_t channel);
        void sendSystemChannelConnected(uint8_t channel, int flags, bool noerror);
        void sendSystemChannelError(uint8_t channel, int code, const std::string &);

        void recvLtsmEvent(uint8_t channel, std::vector<uint8_t> &&);

        virtual void sendLtsmChannelData(uint8_t channel, const uint8_t*, size_t) = 0;
        virtual bool serverSide(void) const { return false; }

        virtual bool createChannelAllow(const Channel::ConnectorType &, const std::string &, const Channel::ConnectorMode &) const { return false; }

        virtual const char* pkcs11Library(void) const { return nullptr; }
    };

#ifdef __UNIX__
    class ChannelListener : public ChannelClient
    {
        std::list<std::unique_ptr<Channel::Listener>> listeners;
        mutable std::mutex lockls;

    protected:
        bool createListener(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, size_t listen, const Channel::Opts &);
        void destroyListener(const std::string & clientUrl, const std::string & serverUrl);

    public:
        ChannelListener() = default;
        virtual ~ChannelListener() = default;

        bool createChannelAcceptFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts &);
    };
#endif
}

#endif
