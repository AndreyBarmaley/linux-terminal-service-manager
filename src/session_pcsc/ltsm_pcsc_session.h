/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_PCSC_SESSION_
#define _LTSM_PCSC_SESSION_

#include <tuple>
#include <thread>
#include <atomic>
#include <memory>
#include <future>
#include <string>
#include <vector>
#include <functional>
#include <forward_list>

#include "pcsclite.h"

#include "ltsm_pcsc.h"
#include "ltsm_async_socket.h"
#include "ltsm_application.h"
#include "ltsm_pcsc_adaptor.h"
#include "avast_asio_async_mutex.hpp"

namespace PcscLite {
    // origin READER_STATE: PCSC/src/eventhandler.h
    struct ReaderState {
        char name[MAX_READERNAME];///< reader name
        uint32_t event = 0; ///< number of card events
        uint32_t state = 0; ///< SCARD_* bit field
        int32_t share = 0; ///< PCSCLITE_SHARING_* sharing status

        uint8_t atr[MAX_ATR_SIZE];///< ATR
        uint32_t atrLen = 0; ///< ATR length
        uint32_t protocol = 0; ///< SCARD_PROTOCOL_* value

        void reset(void);
    };
}

namespace LTSM {
    using binary_buf = std::vector<uint8_t>;
    using char_buf = std::vector<char>;

    using RetEstablishedContext = std::tuple<uint64_t, uint32_t>;
    using RetReleaseContext = std::tuple<uint32_t>;
    using RetConnect = std::tuple<uint64_t, uint32_t, uint32_t>;
    using RetReconnect = std::tuple<uint32_t, uint32_t>;
    using RetDisconnect = std::tuple<uint32_t>;
    using RetTransaction = std::tuple<uint32_t>;
    using RetTransmit = std::tuple<uint32_t, uint32_t, uint32_t, binary_buf>;
    using RetStatus = std::tuple<std::string, uint32_t, uint32_t, uint32_t, binary_buf>;
    using RetControl = std::tuple<uint32_t, binary_buf>;
    using RetGetAttrib = std::tuple<uint32_t, binary_buf>;
    using RetSetAttrib = std::tuple<uint32_t>;
    using RetCancel = std::tuple<uint32_t>;
    using ListReaders = std::list<std::string>;
    using RetStatusChanged = std::tuple<bool, uint32_t>;

    struct Transaction {
        avast::asio::async_mutex trans_lock;
        int32_t client_id{0};

        boost::asio::awaitable<void> lock(int32_t id) {
            if(id) {
                co_await trans_lock.async_lock(boost::asio::use_awaitable);
                client_id = id;
            }
            co_return;
        }

        void unlock(int32_t id) {
            if(id && client_id == id) {
                client_id = 0;
                trans_lock.unlock();
            }
        }
    };

    class PcscRemote : protected AsyncSocket<boost::asio::local::stream_protocol::socket> {

        std::unordered_map<uint32_t, uint64_t> map_context_;

        uint64_t timer_context_{0};
        boost::asio::cancellation_signal timer_stop_;
        avast::asio::async_mutex send_lock_;

        Transaction trans_lock_;

        boost::system::error_code ec_;
        bool connected_{false};

      protected:
        [[nodiscard]] boost::asio::awaitable<uint32_t> syncReaderStatus(const int32_t &, const uint64_t &, const std::string &, PcscLite::ReaderState &, bool* changed = nullptr);
        [[nodiscard]] boost::asio::awaitable<void> transactionLock(int32_t id);

      public:
        PcscRemote(boost::asio::local::stream_protocol::socket && sock)
            : AsyncSocket<boost::asio::local::stream_protocol::socket>(std::move(sock)) {
        }
        ~PcscRemote() = default;

        [[nodiscard]] boost::asio::awaitable<bool> handlerWaitConnect(const std::string & path);

        [[nodiscard]] boost::asio::awaitable<RetEstablishedContext> sendEstablishedContext(const int32_t & id, const uint32_t & scope);
        [[nodiscard]] boost::asio::awaitable<RetReleaseContext> sendReleaseContext(const int32_t & id, const uint64_t & context);
        [[nodiscard]] boost::asio::awaitable<RetConnect> sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, std::string_view readerName);
        [[nodiscard]] boost::asio::awaitable<RetReconnect> sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization);
        [[nodiscard]] boost::asio::awaitable<RetDisconnect> sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition);
        [[nodiscard]] boost::asio::awaitable<RetTransaction> sendBeginTransaction(const int32_t & id, const uint64_t & handle);
        [[nodiscard]] boost::asio::awaitable<RetTransaction> sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition);
        [[nodiscard]] boost::asio::awaitable<RetTransmit> sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data);
        [[nodiscard]] boost::asio::awaitable<RetStatus> sendStatus(const int32_t & id, const uint64_t & handle);
        [[nodiscard]] boost::asio::awaitable<RetControl> sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1);
        [[nodiscard]] boost::asio::awaitable<RetGetAttrib> sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId);
        [[nodiscard]] boost::asio::awaitable<RetSetAttrib> sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr);
        [[nodiscard]] boost::asio::awaitable<RetCancel> sendCancel(const int32_t & id, const uint64_t & context);

        [[nodiscard]] boost::asio::awaitable<uint32_t> sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount);
        [[nodiscard]] boost::asio::awaitable<ListReaders> sendListReaders(const int32_t & id, const uint64_t & context);

        bool isError(void) const {
            return !! ec_;
        }

        bool isConnected(void) const {
            return connected_;
        }

        [[nodiscard]] boost::asio::awaitable<uint32_t> syncReaders(const int32_t & id, const uint64_t & context, bool* changed);
        [[nodiscard]] boost::asio::awaitable<void> syncReaderTimerStart(const int32_t & id, const uint64_t & context);

        void syncReaderTimerStop(void);
        void transactionUnlock(int32_t id);

        uint32_t makeContext64(uint64_t remote);
        uint64_t findContext32(uint32_t local) const;
        void removeContext32(uint32_t local);
    };

    class PcscSessionBus;

    class PcscLocal : protected AsyncSocket<boost::asio::local::stream_protocol::socket> {
        uint64_t context64_ = 0; ///< remote context
        uint64_t handle64_ = 0;  ///< remote handle

        uint32_t context32_ = 0; ///< local context
        uint32_t handle32_ = 0;  ///< local handle
        const int cid_ = 0;

        std::weak_ptr<PcscRemote> remote_;
        boost::asio::cancellation_signal stop_;

        PcscLite::ReaderState* reader_ = nullptr;
        PcscSessionBus* session_ = nullptr;

      protected:
        void handlerClientWaitCommand(const boost::system::error_code & ec);

        void handlerClientActionStarted(void);
        [[nodiscard]] boost::asio::awaitable<void> replyError(uint32_t len, uint32_t err);

        [[nodiscard]] boost::asio::awaitable<bool> proxyEstablishContext(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyReleaseContext(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyConnect(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyReconnect(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyDisconnect(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyBeginTransaction(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyEndTransaction(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyTransmit(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyStatus(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyControl(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyGetAttrib(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxySetAttrib(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyCancel(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyGetVersion(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyGetReaderState(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyReaderStateChangeStart(void);
        [[nodiscard]] boost::asio::awaitable<bool> proxyReaderStateChangeStop(void);

        [[nodiscard]] boost::asio::awaitable<bool> clientAction(uint32_t cmd, uint32_t len);
        void statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr);

      public:
        PcscLocal(boost::asio::local::stream_protocol::socket && sock, int cid, std::shared_ptr<PcscRemote> ptr, PcscSessionBus* bus)
            : AsyncSocket(std::move(sock)), cid_{cid}, remote_{ptr}, session_{bus} {
        }
        ~PcscLocal() = default;

        [[nodiscard]] boost::asio::awaitable<bool> handlerClientWaitCommand(void);

        inline int id(void) const {
            return cid_;
        }

        const uint64_t & proxyContext(void) const {
            return context64_;
        }

        const uint64_t & proxyHandle(void) const {
            return handle64_;
        }

        const uint32_t & localContext(void) const {
            return context32_;
        }
        const uint32_t & localHandle(void) const {
            return handle32_;
        }

        void stopSignal(void) {
            stop_.emit(boost::asio::cancellation_type::terminal);
        }

        boost::asio::cancellation_slot stopSlot(void) {
            return stop_.slot();
        }
    };

    using DBusConnectionPtr = std::unique_ptr<sdbus::IConnection>;

    class PcscSessionBus : public ApplicationLog, public sdbus::AdaptorInterfaces<Session::Pcsc_adaptor> {
        boost::asio::io_context ioc_;
        boost::asio::signal_set signals_;

        boost::asio::local::stream_protocol::endpoint pcsc_ep_;

        boost::asio::cancellation_signal listen_stop_;

        std::list<PcscLocal> clients_;
        boost::asio::strand<boost::asio::any_io_executor> clients_guard_{ioc_.get_executor()};

        DBusConnectionPtr dbus_conn_;
        std::shared_ptr<PcscRemote> remote_;

      protected:
        void stop(void);

        [[nodiscard]] boost::asio::awaitable<void> handlerLocalAccept(PcscLocal & client);
        [[nodiscard]] boost::asio::awaitable<void> handlerLocalListener(void);
        void handlerLocalStopped(const PcscLocal* client, std::exception_ptr);

      public:
        PcscSessionBus(DBusConnectionPtr, bool debug = false);
        virtual ~PcscSessionBus();

        int start(void);

        [[nodiscard]] boost::asio::awaitable<void> handlerStopClient(uint64_t);

        int32_t getVersion(void) override;
        void serviceShutdown(void) override;
        void setDebug(const std::string & level) override;

        bool connectChannel(const std::string & sock) override;
        void disconnectChannel(const std::string & sock) override;
    };
}

#endif // _LTSM_PCSC_SESSION_
