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

#include <span>
#include <list>
#include <mutex>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <utility>
#include <filesystem>
#include <forward_list>

#include <boost/asio.hpp>

#include "ltsm_audio.h"

#ifdef LTSM_PKCS11_AUTH
#include "ltsm_pkcs11_wrapper.h"
#endif

#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_json_wrapper.h"

namespace LTSM {
    static const int LtsmProtocolVersion = 0x03;

    struct AudioFormat;
    class AudioPlayer;

    namespace AudioDecoder {
        class BaseDecoder;
    }

    namespace SystemCommand {
        static const std::string_view ChannelOpen{"ChannelOpen"};
        static const std::string_view ChannelListen{"ChannelListen"};
        static const std::string_view ChannelConnected{"ChannelConnected"};
        static const std::string_view ChannelClose{"ChannelClose"};
        static const std::string_view ChannelError{"ChannelError"};
        static const std::string_view ClientVariables{"ClientVariables"};
        static const std::string_view TransferFiles{"TransferFiles"};
        static const std::string_view KeyboardChange{"KeyboardChange"};
        static const std::string_view CursorFailed{"CursorFailed"};
        static const std::string_view LoginSuccess{"LoginSuccess"};
    }

    class ChannelBase;
    class ChannelListener;

    /// channel_error execption
    struct channel_error : public std::runtime_error {
        explicit channel_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    using CID = uint8_t;
    const CID ChannelTypeSystem = 0;
    const CID ChannelTypeReserved = 0xFF;
    const CID ChannelLimit = UINT8_MAX;

    namespace Channel {
        enum class ConnectorType { Unknown, Unix, Socket, File, Command, Fuse, Audio, Pcsc, Pkcs11 };
        enum class ConnectorMode { Unknown, ReadOnly, ReadWrite, WriteOnly };

        // UltraSlow: ~10k/sec, ~40k/sec, ~80k/sec, ~800k/sec, ~1600k/sec
        enum class Speed { VerySlow, Slow, Medium, Fast, UltraFast, Ultra5 };

        ConnectorType connectorType(std::string_view);
        ConnectorMode connectorMode(std::string_view);
        Speed connectorSpeed(std::string_view);

        std::pair<ConnectorType, std::string> parseUrl(std::string_view);
        std::string createUrl(const ConnectorType &, std::string_view);

        struct TypeContent : std::pair<ConnectorType, std::string> {
            explicit TypeContent(const std::pair<ConnectorType, std::string> & pair) : std::pair<ConnectorType, std::string>(pair) {}
            explicit TypeContent(std::pair<ConnectorType, std::string> && pair) noexcept : std::pair<ConnectorType, std::string>(std::move(pair)) {}

            TypeContent() : std::pair<ConnectorType, std::string> {
                ConnectorType::Unknown, ""
            } {}
            TypeContent(const ConnectorType & type, const std::string & cont) : std::pair<ConnectorType, std::string> {
                type, cont
            } {}

            const ConnectorType & type(void) const {
                return first;
            }
            const std::string & content(void) const {
                return second;
            }
        };

        struct UrlMode : TypeContent {
            ConnectorMode mode = ConnectorMode::Unknown;
            std::string url;

            UrlMode(const std::string & str, std::string_view mod) : TypeContent(parseUrl(str)), mode(connectorMode(mod)), url(str) {}
            UrlMode(const ConnectorType & typ, const std::string & body, const ConnectorMode & mod) : TypeContent(typ, body), mode(mod), url(createUrl(typ, body)) {}
        };

        enum class OptsFlags : uint32_t { ZLibCompression = 1, AllowLoginSession = 2 };

        struct Opts {
            Speed speed = Speed::Medium;
            int flags = 0;
        };

        struct Planned {
            UrlMode serverOpts;
            UrlMode clientOpts;
            Opts chOpts;
            int serverFd = -1;
            CID channel = 0;
        };

        // Local2Remote
        class Local2Remote {
          protected:
            std::chrono::milliseconds delay{100};

            std::vector<uint8_t> buf;

            size_t transfer1 = 0;
            size_t transfer2 = 0;
            size_t blocksz = 4096;

            int error = 0;
            CID id = 255;
            bool zlib = false;

            bool sendData(void);

          public:
            Local2Remote(CID, int flags);
            virtual ~Local2Remote();

            Local2Remote(const Local2Remote&) = delete;
            Local2Remote& operator=(const Local2Remote&) = delete;

            virtual bool hasInput(void) const = 0;
            virtual size_t hasData(void) const = 0;
            virtual ssize_t readDataTo(void* buf, size_t len) = 0;

            bool readData(void);
            void setSpeed(const Channel::Speed &);

            const CID& cid(void) const {
                return id;
            }

            int getError(void) const {
                return error;
            }

            std::chrono::milliseconds getDelay(void) const {
                return delay;
            }

            std::vector<uint8_t> & getBuf(void) {
                return buf;
            }

            const std::vector<uint8_t> & getBuf(void) const {
                return buf;
            }
        };

        /// Local2Remote_FD
        class Local2Remote_FD : public Local2Remote {
            int fd = -1;
            bool needClose = true;

          public:
            Local2Remote_FD(CID, int fd0, bool close, int flags);
            ~Local2Remote_FD();

            bool hasInput(void) const override;
            size_t hasData(void) const override;
            ssize_t readDataTo(void* buf, size_t len) override;
        };

        // Remote2Local
        class Remote2Local {
            mutable std::mutex lockQueue;
          protected:
            std::list<std::vector<uint8_t>> queueBufs;

            std::chrono::milliseconds delay{100};

            size_t transfer1 = 0;
            size_t transfer2 = 0;

            int error = 0;
            CID id = 255;
            bool zlib = false;

          protected:
            std::vector<uint8_t> popData(void);

          public:
            Remote2Local(CID, int flags);
            virtual ~Remote2Local();

            Remote2Local(const Remote2Local&) = delete;
            Remote2Local& operator=(const Remote2Local&) = delete;

            virtual ssize_t writeDataFrom(const void* buf, size_t len) = 0;

            void pushData(std::vector<uint8_t> &&);
            bool writeData(void);
            void setSpeed(const Channel::Speed &);
            bool isEmpty(void) const;

            const CID& cid(void) const {
                return id;
            }

            int getError(void) const {
                return error;
            }

            std::chrono::milliseconds getDelay(void) const {
                return delay;
            }
        };

        /// Remote2Local_FD
        class Remote2Local_FD : public Remote2Local {
            int fd = -1;
            bool needClose = true;

          public:
            Remote2Local_FD(CID, int fd0, bool close, int flags);
            ~Remote2Local_FD();

            ssize_t writeDataFrom(const void* buf, size_t len) override;
        };

        /// ConnectorBase
        class ConnectorBase {
          private:
            std::atomic<bool> loopRunning{false};
            std::atomic<bool> remoteConnected{false};

            CID cid = 255;

          protected:
            ChannelBase* owner = nullptr;
            ConnectorMode mode = ConnectorMode::Unknown;

          public:
            int flags = 0;

          public:
            ConnectorBase(CID, const ConnectorMode & mod, const Opts & chOpts, ChannelBase & srv);
            virtual ~ConnectorBase() = default;

            virtual int error(void) const = 0;

            virtual void setSpeed(const Channel::Speed &) = 0;
            virtual void pushData(std::vector<uint8_t> &&) = 0;

            bool isAllowSessionFor(bool user) const;
            bool isRunning(void) const;
            void setRunning(bool);

            bool isRemoteConnected(void) const;
            void setRemoteConnected(bool);

            ChannelBase* getOwner(void) {
                return owner;
            }

            bool isMode(ConnectorMode cm) const {
                return mode == cm;
            }

            CID channel(void) const {
                return cid;
            }
        };

        using ConnectorBasePtr = std::unique_ptr<ConnectorBase>;

        // ConnectorFD_R
        class ConnectorFD_R : public ConnectorBase {
            std::unique_ptr<Local2Remote> localRemote;
            std::thread thr;

          public:
            ConnectorFD_R(CID, int fd, bool close, const Opts &, ChannelBase &);
            virtual ~ConnectorFD_R();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override { /* skipped */ }
        };

        // ConnectorFD_W
        class ConnectorFD_W : public ConnectorBase {
            std::unique_ptr<Remote2Local> remoteLocal;
            std::thread thw;

          public:
            ConnectorFD_W(CID, int fd, bool close, const Opts &, ChannelBase &);
            virtual ~ConnectorFD_W();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        // ConnectorFD_RW
        class ConnectorFD_RW : public ConnectorBase {
            std::unique_ptr<Remote2Local> remoteLocal;
            std::unique_ptr<Local2Remote> localRemote;

            std::thread thr;
            std::thread thw;

          public:
            ConnectorFD_RW(CID, int fd, const Opts &, ChannelBase &);
            virtual ~ConnectorFD_RW();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        // ConnectorCMD_R
        class ConnectorCMD_R : public ConnectorFD_R {
            FILE* fcmd = nullptr;

          public:
            ConnectorCMD_R(CID, FILE*, const Opts &, ChannelBase &);
            virtual ~ConnectorCMD_R();
        };

        // ConnectorCMD_W
        class ConnectorCMD_W : public ConnectorFD_W {
            FILE* fcmd = nullptr;

          public:
            ConnectorCMD_W(CID, FILE*, const Opts &, ChannelBase &);
            virtual ~ConnectorCMD_W();
        };

#ifdef LTSM_CLIENT
        /// ConnectorClientFuse
        class ConnectorClientFuse : public ConnectorBase {
            StreamBuf reply;
            std::forward_list<int> opens;
            std::string shareRoot;

            bool fuseInit = false;
            uint16_t fuseVer = 0;

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
            ConnectorClientFuse(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
            virtual ~ConnectorClientFuse();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        /// ConnectorClientAudio
        class ConnectorClientAudio : public ConnectorBase {
            std::forward_list<AudioFormat> formats;
            const AudioFormat* format = nullptr;

            using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
            std::unique_ptr<TimePoint> silent;

            std::unique_ptr<AudioPlayer> player;
            std::unique_ptr<AudioDecoder::BaseDecoder> decoder;
            std::vector<uint8_t> last;

          protected:
            bool audioOpInit(const StreamBufRef &);
            void audioOpData(const StreamBufRef &);
            void audioOpSilent(const StreamBufRef &);

          public:
            ConnectorClientAudio(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
            virtual ~ConnectorClientAudio();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

        /// ConnectorClientPcsc
        class ConnectorClientPcsc : public ConnectorBase {
            //uint16_t    pcscVer = 0;
            std::vector<uint8_t> last;

          protected:
            bool pcscOpInit(const StreamBufRef &);
            void pcscLiteCommand(uint16_t cmd, const StreamBufRef & sb);

            void pcscLiteEstablishContext(const StreamBufRef &);
            void pcscLiteReleaseContext(const StreamBufRef &);
            void pcscLiteListReaders(const StreamBufRef &);
            void pcscLiteConnect(const StreamBufRef &);
            void pcscLiteReconnect(const StreamBufRef &);
            void pcscLiteDisconnect(const StreamBufRef &);
            void pcscLiteBeginTransaction(const StreamBufRef &);
            void pcscLiteEndTransaction(const StreamBufRef &);
            void pcscLiteTransmit(const StreamBufRef &);
            void pcscLiteStatus(const StreamBufRef &);
            void pcscLiteGetStatusChange(const StreamBufRef &);
            void pcscLiteControl(const StreamBufRef &);
            void pcscLiteCancel(const StreamBufRef &);
            void pcscLiteGetAttrib(const StreamBufRef &);
            void pcscLiteSetAttrib(const StreamBufRef &);

          public:
            ConnectorClientPcsc(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
            virtual ~ConnectorClientPcsc();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };

#ifdef LTSM_PKCS11_AUTH
        /// ConnectorClientPkcs11
        class ConnectorClientPkcs11 : public ConnectorBase {
            StreamBuf reply;

            PKCS11::LibraryPtr pkcs11;
            std::vector<uint8_t> last;

          protected:
            bool pkcs11Init(const StreamBufRef &);
            bool pkcs11GetSlots(const StreamBufRef &);
            bool pkcs11GetSlotMechanisms(const StreamBufRef &);
            bool pkcs11GetSlotCertificates(const StreamBufRef &);
            bool pkcs11SignData(const StreamBufRef &);
            bool pkcs11DecryptData(const StreamBufRef &);

          public:
            ConnectorClientPkcs11(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
            virtual ~ConnectorClientPkcs11();

            int error(void) const override;
            void setSpeed(const Channel::Speed &) override;
            void pushData(std::vector<uint8_t> &&) override;
        };
#endif // LTSM_PKCS11_AUTH
#endif // LTSM_CLIENT

        namespace Connector {
            const char* typeString(const ConnectorType &);
            const char* modeString(const ConnectorMode &);
            const char* speedString(const Speed &);
        }

#ifdef __UNIX__
        ConnectorBasePtr createUnixConnector(CID, int fd, const ConnectorMode &, const Opts &, ChannelBase &);
        ConnectorBasePtr createUnixConnector(CID, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelBase &);

        ConnectorBasePtr createTcpConnector(CID, int fd, const ConnectorMode &, const Opts &, ChannelBase &);
        ConnectorBasePtr createTcpConnector(CID, const std::string & ipaddr, int port, const ConnectorMode &, const Opts &, ChannelBase &);
#endif
#ifdef LTSM_PKCS11_AUTH
        ConnectorBasePtr createClientPkcs11Connector(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
#endif
#ifdef LTSM_WITH_PCSC
        ConnectorBasePtr createClientPcscConnector(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
#endif
#ifdef LTSM_WITH_FUSE
        ConnectorBasePtr createClientFuseConnector(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
#endif
#ifdef LTSM_WITH_AUDIO
        ConnectorBasePtr createClientAudioConnector(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);
#endif
        ConnectorBasePtr createFileConnector(CID, const std::filesystem::path &, const ConnectorMode &, const Opts &, ChannelBase &);
        ConnectorBasePtr createCommandConnector(CID, const std::string &, const ConnectorMode &, const Opts &, ChannelBase &);

#ifdef __UNIX__
        /// Listener
        class Listener {
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

            const std::string & getClientUrl(void) const {
                return copts.url;
            }

            const std::string & getServerUrl(void) const {
                return sopts.url;
            }

            bool isUnix(void) const {
                return sopts.type() == ConnectorType::Unix;
            }
        };

        std::unique_ptr<Listener> createUnixListener(const Channel::UrlMode & serverOpts, size_t listen,
                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelListener &);
        std::unique_ptr<Listener> createTcpListener(const Channel::UrlMode & serverOpts, size_t listen,
                const Channel::UrlMode & clientOpts, const Channel::Opts &, ChannelListener &);
#endif
    } // namespace Channel

    class ChannelBase {
        boost::asio::strand<boost::asio::any_io_executor> strand_;

        mutable std::mutex lockch;
        mutable std::mutex lockpl;

        std::array<Channel::ConnectorBasePtr, 256> channels_;
        std::list<Channel::Planned> channelsPlanned;

      protected:
        int channelDebug = -1;

      protected:
        void plannedEmplace(Channel::Planned &&);

        Channel::ConnectorBase* findChannel(CID);
        Channel::Planned* findPlanned(CID);

        bool channelPlannedCreate(CID, const Channel::Planned &);

        void recvLtsmProto(CID, std::vector<uint8_t> &&);
        void recvChannelData(CID, std::vector<uint8_t> &&);
        void recvChannelSystem(const JsonContent &);

        virtual bool isUserSession(void) const {
            return false;
        }

        // recv system events
        virtual void recvChannelSystemEvent(const std::string&, const JsonObject &) = 0;
        virtual void systemChannelErrorEvent(const JsonObject &) { /* empty */ }

        void systemChannelConnectedEvent(const JsonObject &);
        void systemChannelCloseEvent(const JsonObject &);

        bool createChannel(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, const Channel::Opts &);
        void destroyChannel(CID);

#ifdef __UNIX__
        bool createChannelUnix(CID, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelUnixFd(CID, int, const Channel::ConnectorMode &, const Channel::Opts &);

        bool createChannelSocket(CID, std::pair<std::string, int>, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelSocketFd(CID, int, const Channel::ConnectorMode &, const Channel::Opts &);
#endif
        bool createChannelFile(CID, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelCommand(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelBaseFuse(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelBaseAudio(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelBasePcsc(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        bool createChannelBasePkcs11(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);

        size_t countFreeChannels(void) const;

        void setChannelDebug(CID, bool);
        void channelsShutdown(void);

      public:
        explicit ChannelBase(const boost::asio::any_io_executor& ctx) : strand_{ctx} {
        }

        virtual ~ChannelBase() = default;

        boost::asio::awaitable<bool> sendSystemTransferFiles(std::forward_list<std::string>);
        void sendSystemChannelOpen(CID, const Channel::UrlMode &, const Channel::Opts &);
        void sendSystemChannelClose(CID);
        void sendSystemChannelConnected(CID, int flags, bool noerror);
        void sendSystemChannelError(CID, int code, const std::string &);

        void recvLtsmEvent(CID, std::vector<uint8_t> &&);

        virtual void sendLtsmChannelData(CID, std::vector<uint8_t>&&) = 0;
        virtual void sendLtsmChannelData(CID, std::string&&) = 0;
        virtual bool serverSide(void) const = 0;
        virtual bool allowCreateChannel(const Channel::ConnectorType &, const std::string &, const Channel::ConnectorMode &) const {
            return false;
        }
    };

    class ChannelClient : public ChannelBase {
      protected:
        void recvChannelSystemEvent(const std::string&, const JsonObject &) override;

        void systemChannelOpenEvent(const JsonObject &);
        void systemChannelListenEvent(const JsonObject &);

        virtual void systemLoginSuccessEvent(const JsonObject &) = 0;

      public:
        explicit ChannelClient(const boost::asio::any_io_executor& ctx) : ChannelBase(ctx) {
        }

        void sendSystemCursorFailed(int cursorId);
        void sendSystemKeyboardChange(const std::vector<std::string> &, int);
        void sendSystemClientVariables(const json_plain &, const json_plain &, const std::vector<std::string> &, const std::string &);

        virtual const char* pkcs11Library(void) const {
            return nullptr;
        }

        bool serverSide(void) const override {
            return false;
        }
    };

#ifdef __UNIX__
    class ChannelListener : public ChannelBase {
        std::list<std::unique_ptr<Channel::Listener>> listeners;
        mutable std::mutex lockls;

      protected:
        bool createListener(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, size_t listen, const Channel::Opts &);
        void destroyListener(const std::string & clientUrl, const std::string & serverUrl);

        void recvChannelSystemEvent(const std::string&, const JsonObject &) override;

        virtual void systemClientVariablesEvent(const JsonObject &) = 0;
        virtual void systemKeyboardChangeEvent(const JsonObject &) = 0;
        virtual void systemTransferFilesEvent(const JsonObject &) = 0;
        virtual void systemCursorFailedEvent(const JsonObject &) = 0;

      public:
        explicit ChannelListener(const boost::asio::any_io_executor& ctx) : ChannelBase(ctx) {
        }

        bool serverSide(void) const override {
            return true;
        }

        bool createChannelAcceptFd(const Channel::UrlMode & clientOpts, int sock, const Channel::UrlMode & serverOpts, const Channel::Opts &);
    };
#endif
}

#endif
