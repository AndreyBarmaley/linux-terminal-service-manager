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
#include <optional>
#include <filesystem>
#include <forward_list>

#undef D
#include <boost/container/flat_map.hpp>

#include "ltsm_audio.h"

#ifdef LTSM_PKCS11_AUTH
#include "ltsm_pkcs11_wrapper.h"
#endif

#include "ltsm_tools.h"
#include "ltsm_application.h"

#include "ltsm_streambuf.h"
#include "ltsm_json_wrapper.h"
#include "channel_stream.h"

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
    constexpr CID ChannelTypeSystem = 0;
    constexpr CID ChannelTypeReserved = 0xFF;
    constexpr CID ChannelTypeLast = UINT8_MAX;

    namespace Channel {
        enum class ConnectorType { Unknown, Unix, Socket, File, Command, Fuse, Audio, Pcsc, Pkcs11, AcceptSock };
        enum class ConnectorMode { Unknown, ReadOnly, ReadWrite, WriteOnly };
        enum class ConnectorStatus { Unknown, Openning, Connected, Running, Error };

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
            void updateType(ConnectorType type) {
                first = type;
            }
        };

        struct UrlMode : TypeContent {
            ConnectorMode mode = ConnectorMode::Unknown;
            std::string url;

            UrlMode(const std::string & str, std::string_view mod) : TypeContent(parseUrl(str)), mode(connectorMode(mod)), url(str) {}
            UrlMode(const ConnectorType & typ, const std::string & body, const ConnectorMode & mod) : TypeContent(typ, body), mode(mod), url(createUrl(typ, body)) {}
        };

        enum class OptsFlags : uint32_t { ZLibCompression = 0x0001, AllowLoginSession = 0x0010, SendEventClose = 0x0100, NoFdClose = 0x0200 };

        struct Opts {
            Speed speed = Speed::Medium;
            int flags = 0;
        };

        using generic_socket = boost::asio::generic::stream_protocol::socket;

        struct Planned {
            UrlMode serverOpts;
            UrlMode clientOpts;
            Opts chOpts;
            CID channel = 0;
            std::optional<generic_socket> acceptSock;

            void acceptSockClose(void);

            Planned(const UrlMode& srv, const UrlMode& cli, const Opts& opt);
            Planned(const UrlMode& srv, const UrlMode& cli, const Opts& opt, generic_socket&& sock);

            Planned(Planned&&) noexcept;
            Planned& operator=(Planned&&) noexcept;

            ~Planned();
        };

        /// ConnectorBase
        class ConnectorBase {
          private:
            std::atomic<Channel::ConnectorStatus> status_{ConnectorStatus::Unknown};
            ChannelBase* owner_ = nullptr;

            mutable std::atomic<uint64_t> rstat_ = 0;
            mutable std::atomic<uint64_t> wstat_ = 0;

            ConnectorMode mode_ = ConnectorMode::Unknown;
            std::atomic<uint32_t> flags_{0};

            const Speed speed_;
            const CID cid_;

          protected:
            inline ChannelBase* connectorOwner(void) {
                return owner_;
            }

            inline void setConnectorMode(const ConnectorMode & cm) {
                mode_ = cm;
            }

            inline void setNoFdClose(void) {
                flags_ |= static_cast<uint32_t>(OptsFlags::NoFdClose);
            }

          public:
            ConnectorBase(CID cid, const ConnectorMode & mod, const Opts & opts, ChannelBase & srv);
            virtual ~ConnectorBase();

            std::pair<std::chrono::milliseconds,uint32_t> speedInfo(void) const;

            //
            virtual void cancel(void) { /* empty */ }
            virtual void pushData(std::vector<uint8_t> &&) { /* empty */ }
            virtual void connectorRunningEvent(void) { /* empty */ }

            bool isAllowSessionFor(bool user) const;

            inline bool isZlib(void) const {
                return static_cast<uint32_t>(OptsFlags::ZLibCompression) & flags_;
            }

            inline bool isSendEventClose(void) const {
                return static_cast<uint32_t>(OptsFlags::SendEventClose) & flags_;
            }

            inline bool isNoFdClose(void) const {
                return static_cast<uint32_t>(OptsFlags::NoFdClose) & flags_;
            }

            inline void setSendEventClose(void) {
                flags_ |= static_cast<uint32_t>(OptsFlags::SendEventClose);
            }

            inline void setConnectorStatus(const Channel::ConnectorStatus & st) {
                status_.store(st);
                if(Channel::ConnectorStatus::Running == st) {
                    connectorRunningEvent();
                }
            }

            inline Channel::ConnectorStatus connectorStatus(void) const {
                return status_.load();
            }

            inline bool isConnected(void) const {
                return Channel::ConnectorStatus::Connected == connectorStatus();
            }

            inline bool isRunning(void) const {
                return Channel::ConnectorStatus::Running == connectorStatus();
            }

            inline ChannelBase* getOwner(void) {
                return owner_;
            }

            inline bool connectorMode(const ConnectorMode & cm) const {
                return mode_ == cm;
            }

            inline bool isReadAllow(void) const {
                return connectorMode(ConnectorMode::ReadOnly) ||
                    connectorMode(ConnectorMode::ReadWrite);
            }

            inline bool isWriteAllow(void) const {
                return connectorMode(ConnectorMode::WriteOnly) ||
                    connectorMode(ConnectorMode::ReadWrite);
            }

            inline int connectorFlags(void) const {
                return flags_;
            }

            inline CID channel(void) const {
                return cid_;
            }

            void updateReadStat(uint64_t val) const {
                rstat_ += val;
            }

            void updateWriteStat(uint64_t val) const {
                wstat_ += val;
            }
        };

        using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
        using ConnectorBasePtr = std::shared_ptr<ConnectorBase>;

        class AsyncConnectorBase : public ConnectorBase {
            TimePoint speed_tp{std::chrono::steady_clock::now()};
            uint32_t speed_sz = 0;

          protected:
            void readErrorHandler(std::exception_ptr ptr);
            void writeErrorHandler(std::exception_ptr ptr);

            boost::asio::awaitable<void> readStreamHandler(std::vector<uint8_t>);
            void writeStreamHandler(std::vector<uint8_t> &);
            void writeJobsOverhead(uint32_t jobs);

            virtual bool streamOpen(void) const = 0;

          public:
            using ConnectorBase::ConnectorBase;
        };

        template<typename AsioStream>
        class AsyncStreamConnectorRO : public AsyncConnectorBase, public IReaderStream<AsyncStreamBase> {
            AsioStream stream_;

          protected:
            AsyncStreamBase & stream(void) override {
                return stream_;
            }

            bool streamOpen(void) const override {
                return stream_.isOpen();
            }

            boost::asio::awaitable<void> readStreamHandler(std::vector<uint8_t> buf) override {
                co_await AsyncConnectorBase::readStreamHandler(std::move(buf));
                co_return;
            }

            void readErrorHandler(std::exception_ptr ptr) override {
                AsyncConnectorBase::readErrorHandler(ptr);
            }

          public:
            AsyncStreamConnectorRO(AsioStream&& sd,
                    CID ch, const ConnectorMode & mod, const Opts & opts, ChannelBase & srv)
                : AsyncConnectorBase(ch, mod, opts, srv), stream_{std::move(sd)} {
            }

            void cancel(void) override {
                Application::info("{}: channel: {} is stopping", NS_FuncNameV, channel());
                stopReaderLoop(true);
            }

            void connectorRunningEvent(void) override {
                Application::info("{}: channel: {} is running", NS_FuncNameV, channel());
                startReaderLoop();
            }
        };

        template<typename AsioStream>
        class AsyncStreamConnectorWO : public AsyncConnectorBase, public IWriterStream<AsyncStreamBase> {
            TransferChannel channel_;
            AsioStream stream_;

          protected:
            AsyncStreamBase & stream(void) override {
                return stream_;
            }

            TransferChannel & transferChannel(void) override {
                return channel_;
            }

            bool streamOpen(void) const override {
                return stream_.isOpen();
            }

            boost::asio::awaitable<void> writeStreamHandler(std::vector<uint8_t> buf) override {
                AsyncConnectorBase::writeStreamHandler(buf);
                co_await stream_.async_send_buf(boost::asio::buffer(buf));
                co_return;
            }

            void writeErrorHandler(std::exception_ptr ptr) override {
                AsyncConnectorBase::writeErrorHandler(ptr);
            }

            void writeQueueOverflow(uint32_t jobs) override {
                AsyncConnectorBase::writeJobsOverhead(jobs);
            }

          public:
            AsyncStreamConnectorWO(AsioStream&& sd,
                    CID ch, const ConnectorMode & mod, const Opts & opts, ChannelBase & srv)
                : AsyncConnectorBase(ch, mod, opts, srv), channel_{sd.get_executor(), TransferChannelSize}, stream_{std::move(sd)} {
            }

            void cancel(void) override {
                Application::info("{}: channel: {} is stopping", NS_FuncNameV, channel());
                stopWriterLoop(true);
            }

            void pushData(std::vector<uint8_t>&& buf) override {
                pushWriterData(std::move(buf));
            }

            void connectorRunningEvent(void) override {
                Application::info("{}: channel: {} is running", NS_FuncNameV, channel());
                startWriterLoop();
            }
        };

        template<typename AsioStream>
        class AsyncStreamConnectorRW : public AsyncStreamConnectorWO<AsioStream>, public IReaderStream<AsyncStreamBase> {
          protected:
            boost::asio::awaitable<void> readStreamHandler(std::vector<uint8_t> buf) override {
                co_await AsyncConnectorBase::readStreamHandler(std::move(buf));
                co_return;
            }

            void readErrorHandler(std::exception_ptr ptr) override {
                AsyncConnectorBase::readErrorHandler(ptr);
            }

          public:
            AsyncStreamConnectorRW(AsioStream&& sd,
                    CID ch, const ConnectorMode & mod, const Opts & opts, ChannelBase & srv)
                : AsyncStreamConnectorWO<AsioStream>(std::move(sd), ch, mod, opts, srv) {
            }

            void pushData(std::vector<uint8_t>&& buf) override {
                if(AsyncConnectorBase::isWriteAllow()) {
                    AsyncStreamConnectorWO<AsioStream>::pushWriterData(std::move(buf));
                }
            }

            void cancel(void) override {
                Application::info("{}: channel: {} is stopping", NS_FuncNameV, AsyncConnectorBase::channel());
                const bool read_allow  = AsyncConnectorBase::isReadAllow();
                const bool write_allow = AsyncConnectorBase::isWriteAllow();
                if(read_allow) {
                    IReaderStream<AsyncStreamBase>::stopReaderLoop(!write_allow);
                }
                if(write_allow) {
                    IWriterStream<AsyncStreamBase>::stopWriterLoop(true);
                }
            }

            void connectorRunningEvent(void) override {
                Application::info("{}: channel: {} is running", NS_FuncNameV, AsyncConnectorBase::channel());
                if(AsyncConnectorBase::isReadAllow()) {
                    IReaderStream<AsyncStreamBase>::startReaderLoop();
                }
                if(AsyncConnectorBase::isWriteAllow()) {
                    IWriterStream<AsyncStreamBase>::startWriterLoop();
                }
            }
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

            void pushData(std::vector<uint8_t> &&) override;
        };

        /// ConnectorClientAudio
        class ConnectorClientAudio : public ConnectorBase {
            std::forward_list<AudioFormat> formats;
            const AudioFormat* format = nullptr;

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
            virtual ~ConnectorClientAudio() = default;

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
            virtual ~ConnectorClientPcsc() = default;

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
            virtual ~ConnectorClientPkcs11() = default;

            void pushData(std::vector<uint8_t> &&) override;
        };
#endif // LTSM_PKCS11_AUTH
#endif // LTSM_CLIENT

        namespace Connector {
            const char* typeString(const ConnectorType &);
            const char* modeString(const ConnectorMode &);
            const char* speedString(const Speed &);
        }

        ConnectorBasePtr createSocketConnector(CID, AsyncGenericSocket&&, const ConnectorMode &, const Opts &, ChannelBase &);
        ConnectorBasePtr createStreamConnector(CID, AsyncStreamDescriptor&& sd, const ConnectorMode &, const Opts &, ChannelBase &);
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
        ConnectorBasePtr createCommandConnector(CID, const std::string &, const std::vector<std::string> &, const ConnectorMode &, const Opts &, ChannelBase &);
    } // namespace Channel

    class ChannelBase {
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        std::array<Channel::ConnectorBasePtr, ChannelTypeLast + 1> channels_;

        int channel_debug_ = -1;

      protected:
        bool isFreeSlot(CID);
        size_t countValidSlots(void) const;

        void setChannelStatus(CID, const Channel::ConnectorStatus &);
        Channel::ConnectorStatus channelStatus(CID) const;

        inline bool channelConnected(CID ch) const {
            return Channel::ConnectorStatus::Connected == channelStatus(ch);
        }

        inline void setChannelConnected(CID ch) {
            setChannelStatus(ch, Channel::ConnectorStatus::Connected);
        }

        inline void setChannelRunning(CID ch) {
            setChannelStatus(ch, Channel::ConnectorStatus::Running);
        }

        void recvLtsmChannelEvent(CID, std::vector<uint8_t> &&);
        void recvChannelData(CID, std::vector<uint8_t> &&);
        void recvChannelSystem(const JsonContent &);

        inline bool isChannelDebug(CID channel) const {
            return channel_debug_ == channel;
        }

        void emplaceChannel(CID channel, Channel::ConnectorBasePtr &&);

        // recv system events
        virtual void recvChannelSystemEvent(const std::string &, const JsonObject &) = 0;
        virtual void systemChannelConnectedEvent(const JsonObject &) = 0;
        virtual bool isAllowChannel(const Channel::ConnectorBase*) const = 0;
        virtual void channelErrorNotify(CID, int err, const std::string &) { /* empty */ }

        void systemChannelCloseEvent(const JsonObject &);
        void systemChannelErrorEvent(const JsonObject &);

        void destroyChannel(CID, bool sendEvent);

#ifdef __UNIX__
        void createChannelUnixPath(CID, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelTcpAddress(CID, std::pair<std::string, uint16_t>, const Channel::ConnectorMode &, const Channel::Opts &);
#endif
        void createChannelSocket(CID, AsyncGenericSocket&&, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelFile(CID, const std::filesystem::path &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelCommand(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelFuse(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelAudio(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelPcsc(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);
        void createChannelPkcs11(CID, const std::string &, const Channel::ConnectorMode &, const Channel::Opts &);

        void setChannelDebug(CID, bool);
        void shutdownChannels(void);

      public:
        explicit ChannelBase(const boost::asio::any_io_executor& ctx) : strand_{ctx} {
        }

        virtual ~ChannelBase() = default;

        inline boost::asio::strand<boost::asio::any_io_executor> chan_strand(void) const {
            return strand_;
        }

        bool isValidChannelId(int id) const noexcept {
            return id > ChannelTypeSystem && id < ChannelTypeReserved;
        }

        bool sendSystemTransferFiles(std::forward_list<std::string>&&) const;

        void sendSystemChannelOpen(CID, const Channel::UrlMode &, const Channel::Opts &);
        void sendSystemChannelClose(CID);
        void sendSystemChannelConnected(CID, int flags, int error);
        void sendSystemChannelError(CID, int code, const std::string &);

        virtual void sendLtsmChannelDataPriv(CID, std::vector<uint8_t>&&) const = 0;

        void sendLtsmChannelData(CID cid, std::vector<uint8_t>&& buf) const {
            return sendLtsmChannelDataPriv(cid, std::move(buf));
        }

        void sendLtsmChannelData(CID cid, std::span<const uint8_t> span) const {
            return sendLtsmChannelDataPriv(cid, std::vector<uint8_t>{std::begin(span), std::end(span)});
        }

        void sendLtsmChannelData(CID cid, const std::string& str) const {
            return sendLtsmChannelDataPriv(cid, std::vector<uint8_t>{std::begin(str), std::end(str)});
        }

        void sendChannelDataSuccess(CID channel, size_t val) const;

        virtual bool serverSide(void) const {
            return false;
        }

        virtual bool allowCreateChannel(const Channel::ConnectorType &, const std::string &, const Channel::ConnectorMode &) const {
            return false;
        }
    };

    class ChannelClient : public ChannelBase {
      protected:
        void recvChannelSystemEvent(const std::string &, const JsonObject &) override;
        void systemChannelConnectedEvent(const JsonObject &) override;

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

        bool isAllowChannel(const Channel::ConnectorBase*) const override {
            return true;
        }
    };

#ifdef __UNIX__
    using CancelationSignalPtr = std::unique_ptr<boost::asio::cancellation_signal>;

    class ChannelListener : public ChannelBase {
        boost::container::flat_map<std::string, CancelationSignalPtr> listeners_;

        std::list<Channel::Planned> channels_planned_;
        std::atomic<uint32_t> planned_counts_{0};

      protected:
        template<typename Acceptor>
        boost::asio::awaitable<void> acceptorAcceptAwait(Acceptor acceptor,
                    const Channel::UrlMode& serverOpts, const Channel::UrlMode& clientOpts, const Channel::Opts& channelOpts) {
            auto ex = co_await boost::asio::this_coro::executor;

            for(;;) {
                auto sock = co_await acceptor.async_accept(boost::asio::use_awaitable);
                auto job = Channel::Planned(serverOpts, clientOpts, channelOpts, std::move(sock));
                plannedEmplaceSpawn(std::move(job));
            }
        }

        void plannedEmplaceSpawn(Channel::Planned &&);
        void exceptionHandler(std::exception_ptr ptr);

        void recvChannelSystemEvent(const std::string &, const JsonObject &) override;
        void systemChannelConnectedEvent(const JsonObject &) override;

        virtual void systemClientVariablesEvent(const JsonObject &) = 0;
        virtual void systemKeyboardChangeEvent(const JsonObject &) = 0;
        virtual void systemTransferFilesEvent(const JsonObject &) = 0;
        virtual void systemCursorFailedEvent(const JsonObject &) = 0;

        bool createChannel(const Channel::UrlMode & curlMod, const Channel::UrlMode & surlMod, const Channel::Opts &);
        uint32_t countFreeChannels(void) const;

        void systemChannelConnected(CID channel, int flags, int error);
        void plannedEmplace(Channel::Planned);
        void createChannelPlanned(CID, Channel::Planned &&);

        boost::asio::awaitable<void> createListenerAwait(Channel::UrlMode clientOpts, Channel::UrlMode serverOpts, Channel::Opts channelOpts, int listenLimit);
        void destroyListener(std::string clientUrl);

        boost::asio::ip::tcp::endpoint createTcpEndpoint(const Channel::UrlMode & serverOpts) const;
        boost::asio::local::stream_protocol::endpoint createUnixEndpoint(const Channel::UrlMode & serverOpts) const;

      public:
        explicit ChannelListener(const boost::asio::any_io_executor& ctx) : ChannelBase(ctx) {
            listeners_.reserve(8);
        }

        bool serverSide(void) const override {
            return true;
        }

        bool isAllowChannel(const Channel::ConnectorBase*) const override;

        virtual bool isUserSession(void) const {
            return false;
        }

        bool allowCreateChannel(const Channel::ConnectorType &, const std::string &, const Channel::ConnectorMode &) const override {
            return true;
        }
    };
#endif
}

#endif
