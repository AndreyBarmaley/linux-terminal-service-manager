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

#include <array>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <filesystem>

#include "pcsclite.h"

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_sockets.h"
#include "ltsm_pcsc_session.h"
#include "ltsm_byte_streambuf.h"

using namespace std::chrono_literals;
using namespace boost;

namespace PcscLite {
    std::array<ReaderState, PCSCLITE_MAX_READERS_CONTEXTS> readers;

    ReaderState* findReaderState(std::string_view name) {
        auto it = std::ranges::find_if(readers, [&](auto & rd) {
            return 0 == name.compare(rd.name);
        });

        return it != readers.end() ? std::addressof(*it) : nullptr;
    }

    bool readersReset(void) {
        bool changed = false;

        // reset all readers
        for(auto & rd : readers) {
            // empty
            if(0 == rd.name[0]) {
                continue;
            }

            rd.reset();
            changed = true;
        }

        return changed;
    }

    bool readersSyncNotFound(const std::list<std::string> & names) {
        bool changed = false;

        for(auto & rd : readers) {
            if(0 == rd.name[0]) {
                continue;
            }

            auto it = std::ranges::find_if(names, [&rd](auto & name) {
                return 0 == name.compare(rd.name);
            });

            // not found, mark absent
            if(it == names.end()) {
                // reset reader
                rd.reset();
                changed = true;
            }
        }

        return changed;
    }

    enum ScardState {
        StateUnknown = 0x0001,
        StateAbsent = 0x0002,
        StatePresent = 0x0004,
        StateSwallowed = 0x0008,
        StatePowered = 0x0010,
        StateNegotiable = 0x0020,
        StateSpecific = 0x0040
    };

    void ReaderState::reset(void) {
        event = 0;
        state = 0;
        share = 0;
        atrLen = MAX_ATR_SIZE;
        protocol = 0;
        std::fill(std::begin(name), std::end(name), 0);
        std::fill(std::begin(atr), std::end(atr), 0);
    }

    inline const char* err2str(uint32_t err) {
        return pcsc_stringify_error(err);
    }

    uint32_t apiCmdLength(uint32_t cmd) {
        uint32_t zero = 0;

        switch(cmd) {
            case EstablishContext:
                return 12;

            case ReleaseContext:
                return 8;

            case Connect:
                return 24 + MAX_READERNAME;

            case Reconnect:
                return 24;

            case Disconnect:
                return 12;

            case BeginTransaction:
                return 8;

            case EndTransaction:
                return 12;

            case Transmit:
                return 32;

            case Status:
                return 8;

            case Control:
                return 24;

            case GetAttrib:
                return 16 + MAX_BUFFER_SIZE;

            case SetAttrib:
                return 16 + MAX_BUFFER_SIZE;

            case Cancel:
                return 8;

            case GetVersion:
                return 12;

            case GetReaderState:
            case WaitReaderStateChangeStart:
            case WaitReaderStateChangeStop:
                return 0;

            default:
                LTSM::Application::warning("{}: unknown cmd: {:#010x}", NS_FuncNameV, cmd);
                break;
        }

        return 0;
    }
}

namespace LTSM {
    /// PcscRemote
    uint32_t PcscRemote::makeContext64(uint64_t remote) {
        uint32_t context = Tools::crc32b({(const uint8_t*) & remote, sizeof(remote)});
        context &= 0x7FFFFFFF;
        map_context_.emplace(context, remote);
        return context;
    }

    uint64_t PcscRemote::findContext32(uint32_t local) const {
        auto it = map_context_.find(local);
        return it != map_context_.end() ? it->second : 0;
    }

    void PcscRemote::removeContext32(uint32_t local) {
        if(auto it = map_context_.find(local); it != map_context_.end()) {
            map_context_.erase(it);
        }
    }

    asio::awaitable<void> PcscRemote::transactionLock(int32_t id) {
        co_await trans_lock_.lock(id);
    }

    void PcscRemote::transactionUnlock(int32_t id) {
        trans_lock_.unlock(id);
    }

    asio::awaitable<void> PcscRemote::retryConnect(const std::string & path, int attempts) {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer{ex};

        for(int it = 1; it <= attempts; it++) {
            try {
                co_await socket().async_connect(path, asio::use_awaitable);
                Application::debug(DebugType::Pcsc, "{}: connected, path: {}", NS_FuncNameV, path);
                co_return;
            } catch(const system::system_error& err) {
                if(it == attempts) {
                    Application::warning("{}: {} failed, path: {}, attempts: {}", NS_FuncNameV, "connect", path, attempts);
                    throw system::system_error(asio::error::operation_aborted);
                } else {
            	    auto ec = err.code();
                    Application::warning("{}: system error: {}", NS_FuncNameV, ec.message());
                }
            }

            timer.expires_after(300ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    asio::awaitable<void> PcscRemote::remoteHandshake(void) {
        co_await send_lock_.async_lock();

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Init)),
                endian::native_to_little(static_cast<uint16_t>(PcscOp::ProtoVer)));
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        uint16_t cmd, err;

        co_await async_recv_values(cmd, err);
        endian::little_to_native_inplace(cmd);
        endian::little_to_native_inplace(err);

        if(cmd != PcscOp::Init) {
            send_lock_.unlock();
            Application::error("{}: {} failed, cmd: {:#06x}", NS_FuncNameV, "id", cmd);
            throw pcsc_error(NS_FuncNameS);
        }

        if(err) {
            auto str = co_await async_recv_buf<std::string>(err);
            send_lock_.unlock();
            Application::error("{}: recv error: {}", NS_FuncNameV, str);
            throw pcsc_error(NS_FuncNameS);
        }

        auto ver = co_await async_recv_le16();
        send_lock_.unlock();

        if(ver == PcscOp::ProtoVer) {
            Application::info("{}: client proto version: {}", NS_FuncNameV, ver);
            co_return;
        }

        Application::error("{}: unsupported version: {}", NS_FuncNameV, ver);
        throw pcsc_error(NS_FuncNameS);
    }

    asio::awaitable<RetEstablishedContext>
    PcscRemote::sendEstablishedContext(const int32_t & id, const uint32_t & scope) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << scope: {}", NS_FuncNameV, id, scope);

        uint64_t context;
        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::EstablishContext)),
                endian::native_to_little(scope));

            co_await async_recv_values(context, ret);

            endian::little_to_native_inplace(context);
            endian::little_to_native_inplace(ret);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(context, ret);
    }

    asio::awaitable<RetReleaseContext>
    PcscRemote::sendReleaseContext(const int32_t & id, const uint64_t & context) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64: {:#018x}",
                           NS_FuncNameV, id, context);

        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::ReleaseContext)),
                endian::native_to_little(context));

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetConnect>
    PcscRemote::sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, std::string_view readerName) {
        co_await transactionLock(id);
        co_await send_lock_.async_lock();
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64: {:#018x}, shareMode: {}, prefferedProtocols: {}, reader: `{}'",
                           NS_FuncNameV, id, context, shareMode, prefferedProtocols, readerName);

        uint64_t handle;
        uint32_t activeProtocol, ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Connect)),
                endian::native_to_little(context),
                endian::native_to_little(shareMode),
                endian::native_to_little(prefferedProtocols),
                endian::native_to_little(static_cast<uint32_t>(readerName.size())), readerName);

            co_await async_recv_values(handle, activeProtocol, ret);

            endian::little_to_native_inplace(handle);
            endian::little_to_native_inplace(activeProtocol);
            endian::little_to_native_inplace(ret);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        transactionUnlock(id);

        co_return std::make_tuple(handle, activeProtocol, ret);
    }

    asio::awaitable<RetReconnect>
    PcscRemote::sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}",
                           NS_FuncNameV, id, handle, shareMode, prefferedProtocols, initialization);
        uint32_t activeProtocol, ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Reconnect)),
                endian::native_to_little(handle),
                endian::native_to_little(shareMode),
                endian::native_to_little(prefferedProtocols),
                endian::native_to_little(initialization));

            co_await async_recv_values(activeProtocol, ret);

            endian::little_to_native_inplace(activeProtocol);
            endian::little_to_native_inplace(ret);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(activeProtocol, ret);
    }

    asio::awaitable<RetDisconnect>
    PcscRemote::sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, disposition: {}",
                           NS_FuncNameV, id, handle, disposition);
        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Disconnect)),
                endian::native_to_little(handle),
                endian::native_to_little(disposition));

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransaction>
    PcscRemote::sendBeginTransaction(const int32_t & id, const uint64_t & handle) {
        co_await transactionLock(id);
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}",
                           NS_FuncNameV, id, handle);
        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::BeginTransaction)),
                endian::native_to_little(handle));

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        if(ret != SCARD_S_SUCCESS) {
            transactionUnlock(id);
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransaction>
    PcscRemote::sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, disposition: {}",
                           NS_FuncNameV, id, handle, disposition);
        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::EndTransaction)),
                endian::native_to_little(handle),
                endian::native_to_little(disposition));

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        transactionUnlock(id);

        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransmit>
    PcscRemote::sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data1) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, pciProtocol: {:#010x}, pciLength: {}, send size: {}, recv size: {}",
                           NS_FuncNameV, id, handle, ioSendPciProtocol, ioSendPciLength, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", NS_FuncNameV, str);
        }

        uint32_t ioRecvPciProtocol, ioRecvPciLength, ret;
        binary_buf data2;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Transmit)),
                endian::native_to_little(handle),
                endian::native_to_little(ioSendPciProtocol),
                endian::native_to_little(ioSendPciLength),
                endian::native_to_little(recvLength),
                endian::native_to_little(static_cast<uint32_t>(data1.size())), data1);

            uint32_t bytesReturned;
            co_await async_recv_values(ioRecvPciProtocol, ioRecvPciLength, bytesReturned, ret);

            endian::little_to_native_inplace(ioRecvPciProtocol);
            endian::little_to_native_inplace(ioRecvPciLength);
            endian::little_to_native_inplace(bytesReturned);
            endian::little_to_native_inplace(ret);

            data2 = co_await async_recv_buf<binary_buf>(bytesReturned);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, std::move(data2));
    }

    inline void fixed_string_name(std::string & str) {
        while(str.size() && str.back() == 0) {
            str.pop_back();
        }
    }

    asio::awaitable<RetStatus>
    PcscRemote::sendStatus(const int32_t & id, const uint64_t & handle) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}",
                           NS_FuncNameV, id, handle);

        uint32_t state, protocol, atrLen, ret;
        std::string readerName;
        binary_buf atr;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Status)),
                endian::native_to_little(handle));

            uint32_t nameLen, atrLen;
            co_await async_recv_values(state, protocol, nameLen, atrLen, ret);

            endian::little_to_native_inplace(state);
            endian::little_to_native_inplace(protocol);
            endian::little_to_native_inplace(nameLen);
            endian::little_to_native_inplace(atrLen);
            endian::little_to_native_inplace(ret);

            readerName = co_await async_recv_buf<std::string>(nameLen);
            atr = co_await async_recv_buf<binary_buf>(atrLen);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        fixed_string_name(readerName);

        send_lock_.unlock();
        co_return std::make_tuple(std::move(readerName), state, protocol, ret, std::move(atr));
    }

    asio::awaitable<RetControl>
    PcscRemote::sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, controlCode: {:#010x}, send size: {}, recv size: {}",
                           NS_FuncNameV, id, handle, controlCode, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", NS_FuncNameV, str);
        }

        uint32_t ret;
        binary_buf data2;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Control)),
                endian::native_to_little(handle),
                endian::native_to_little(controlCode),
                endian::native_to_little(static_cast<uint32_t>(data1.size())),
                endian::native_to_little(recvLength), data1);

            uint32_t bytesReturned;
            co_await async_recv_values(bytesReturned, ret);

            endian::little_to_native_inplace(bytesReturned);
            endian::little_to_native_inplace(ret);
        
            data2 = co_await async_recv_buf<binary_buf>(bytesReturned);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret, std::move(data2));
    }

    asio::awaitable<RetGetAttrib>
    PcscRemote::sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#018x}, attrId: {}",
                           NS_FuncNameV, id, handle, attrId);

        uint32_t ret;
        binary_buf attr;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::GetAttrib)),
                endian::native_to_little(handle),
                endian::native_to_little(attrId));

            uint32_t attrLen;
            co_await async_recv_values(attrLen, ret);

            endian::little_to_native_inplace(attrLen);
            endian::little_to_native_inplace(ret);

            assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
            attr = co_await async_recv_buf<binary_buf>(attrLen);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret, std::move(attr));
    }

    asio::awaitable<RetSetAttrib>
    PcscRemote::sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64 {:#018x}, attrId: {}, attrLength {}",
                           NS_FuncNameV, id, handle, attrId, attr.size());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(attr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: attr: [{}]", NS_FuncNameV, str);
        }

        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::SetAttrib)),
                endian::native_to_little(handle),
                endian::native_to_little(attrId),
                endian::native_to_little(static_cast<uint32_t>(attr.size())), attr);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetCancel>
    PcscRemote::sendCancel(const int32_t & id, const uint64_t & context) {
        co_await send_lock_.async_lock();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64 {:#018x}",
                           NS_FuncNameV, id, context);

        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::Cancel)),
                endian::native_to_little(context));

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }
/*
        if(timer_context_ == context) {
            timer_stop_.emit(asio::cancellation_type::terminal);
            timer_context_ = 0;
        }
*/
        send_lock_.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<ListReaders> PcscRemote::sendListReaders(const int32_t & id, const uint64_t & context) {
        co_await send_lock_.async_lock();

        uint32_t readersCount;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::ListReaders)),
                endian::native_to_little(context));

            readersCount = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32: {:#010x}, readers count: {}",
                           NS_FuncNameV, id, context, readersCount);

        ListReaders names;

        while(readersCount--) {
            try {
                uint32_t readerLen = co_await async_recv_le32();
                auto readerName = co_await async_recv_buf<std::string>(readerLen);
                names.emplace_back(std::move(readerName));
            } catch(const system::system_error& err) {
                ec_ = err.code();
            }

            Application::debug(DebugType::Pcsc, "{}: reader - '{}'", names.back());

            if(names.back().size() > MAX_READERNAME - 1) {
                names.back().resize(MAX_READERNAME - 1);
            }
        }

        send_lock_.unlock();
        co_return names;
    }

    asio::awaitable<uint32_t> PcscRemote::sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount) {
        co_await send_lock_.async_lock();

        uint32_t ret;

        try {
            co_await async_send_values(
                endian::native_to_little(static_cast<uint16_t>(PcscOp::Lite)),
                endian::native_to_little(static_cast<uint16_t>(PcscLite::GetStatusChange)),
                endian::native_to_little(context),
                endian::native_to_little(timeout),
                endian::native_to_little(statesCount));

            for(uint32_t it = 0; it < statesCount; ++it) {
                const SCARD_READERSTATE & state = states[it];
                auto len = strnlen(state.szReader, MAX_READERNAME);

                co_await async_send_values(
                    endian::native_to_little(static_cast<uint32_t>(len)),
                    endian::native_to_little(static_cast<uint32_t>(state.dwCurrentState)),
                    endian::native_to_little(static_cast<uint32_t>(state.cbAtr)),
                    asio::const_buffer(state.szReader, len),
                    asio::const_buffer(state.rgbAtr, state.cbAtr));
            }

            uint32_t counts;
            co_await async_recv_values(counts, ret);

            endian::little_to_native_inplace(counts);
            endian::little_to_native_inplace(ret);

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context64: {:#018x}, timeout: {}, states: {}",
                               NS_FuncNameV, id, context, timeout, counts);

            assertm(counts == statesCount, "count states invalid");

            for(uint32_t it = 0; it < statesCount; ++it) {
                SCARD_READERSTATE & state = states[it];

                uint32_t curState, dwState, readerLen, cbAtr;

                co_await async_recv_values(curState, dwState, readerLen, cbAtr);

                endian::little_to_native_inplace(curState);
                endian::little_to_native_inplace(dwState);
                endian::little_to_native_inplace(readerLen);
                endian::little_to_native_inplace(cbAtr);

                assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

                state.dwCurrentState = curState;
                state.dwEventState = dwState;
                state.cbAtr = cbAtr;

                std::string readerName(readerLen, 0);
                co_await async_recv_values(readerName, asio::buffer(state.rgbAtr, cbAtr));

                if(readerName != state.szReader) {
                    Application::warning("{}: invalid reader, `{}' != `{}'", NS_FuncNameV, readerName, state.szReader);
                }
            }
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock_.unlock();
        co_return ret;
    }

    asio::awaitable<uint32_t> PcscRemote::syncReaders(const int32_t & id, const uint64_t & context, bool* changed) {
        ListReaders names = co_await sendListReaders(id, context);

        if(names.empty()) {
            Application::warning("{}: no readers available", NS_FuncNameV);

            bool res = PcscLite::readersReset();

            if(changed && res) {
                *changed = true;
            }

            co_return static_cast<uint32_t>(SCARD_E_NO_READERS_AVAILABLE);
        }

        if(PcscLite::readersSyncNotFound(names)) {
            if(changed) {
                *changed = true;
            }
        }

        for(const auto & name : names) {
            auto it = std::ranges::find_if(PcscLite::readers, [&name](auto & rd) {
                return 0 == name.compare(rd.name);
            });

            // not found, add new
            if(it == PcscLite::readers.end()) {
                Application::debug(DebugType::Pcsc, "{}: added reader, name: `{}'", NS_FuncNameV, name);
                // find unused slot
                auto rd = std::ranges::find_if(PcscLite::readers, [](auto & rd) {
                    return 0 == rd.name[0];
                });

                if(rd == PcscLite::readers.end()) {
                    LTSM::Application::error("{}: failed, {}", NS_FuncNameV, "all slots is busy");
                    co_return static_cast<uint32_t>(SCARD_E_NO_MEMORY);
                }

                rd->reset();
                co_await syncReaderStatus(id, context, name, *rd, changed);
            }
        }

        co_return static_cast<uint32_t>(SCARD_S_SUCCESS);
    }

    asio::awaitable<uint32_t> PcscRemote::syncReaderStatus(const int32_t & id, const uint64_t & context,
            const std::string & readerName, PcscLite::ReaderState & rd, bool* changed) {
        const uint32_t timeout = 0;
        SCARD_READERSTATE state = {};

        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;
        state.cbAtr = MAX_ATR_SIZE;

        uint32_t ret = co_await sendGetStatusChange(id, context, timeout, & state, 1);

        if(ret == SCARD_E_TIMEOUT) {
            Application::warning("{}: timeout", NS_FuncNameV);
            co_return ret;
        }

        if(ret != SCARD_S_SUCCESS) {
            Application::warning("{}: error: {:#010x} ({})", NS_FuncNameV, ret, PcscLite::err2str(ret));
            co_return ret;
        }

        Application::debug(DebugType::Pcsc, "{}: reader: `{}', currentState: {:#010x}, eventState: {:#010x}, atrLen: {}",
                           NS_FuncNameV, readerName, state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::rangeHexString(state.rgbAtr, state.rgbAtr + state.cbAtr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: atr: [{}]", NS_FuncNameV, str);
        }

        if(state.dwEventState & SCARD_STATE_CHANGED) {
            assertm(readerName.size() < sizeof(rd.name), "reader name invalid");
            assertm(state.cbAtr <= sizeof(rd.atr), "atr length invalid");
            rd.state = state.dwEventState & SCARD_STATE_PRESENT ? (PcscLite::StatePresent | PcscLite::StatePowered |
                       PcscLite::StateNegotiable) : PcscLite::StateAbsent;
            std::ranges::copy(readerName, rd.name);
            std::ranges::copy_n(state.rgbAtr, state.cbAtr, rd.atr);
            rd.atrLen = state.cbAtr;

            if(changed) {
                *changed = true;
            }
        }

        co_return static_cast<uint32_t>(SCARD_S_SUCCESS);
    }

    void PcscRemote::syncReaderTimerStop(void) {
/*
        if(timer_context_) {
            timer_stop_.emit(asio::cancellation_type::terminal);
            timer_context_ = 0;
        }
*/
    }

    asio::awaitable<void> PcscRemote::syncReaderTimerStart(const int32_t & id, const uint64_t & context) {
/*
        if(timer_context_) {
            co_return;
        }

        [[maybe_unused]] auto ret = co_await syncReaders(id, context, nullptr);
        timer_context_ = context;
        auto token = asio::bind_cancellation_slot(timer_stop_.slot(), asio::detached);

        asio::co_spawn(socket().get_executor(),
        [this, ex = socket().get_executor(), id, context]() -> asio::awaitable<void> {
            asio::steady_timer timer{ex};

            for(;;) {
                timer.expires_after(std::chrono::milliseconds(750));
                co_await timer.async_wait(asio::use_awaitable);
                co_await syncReaders(id, context, nullptr);
            }

            co_return;
        }, std::move(token));
*/
        co_return;
    }

    asio::awaitable<bool> PcscLocal::handlerClientWaitCommand(void) {
        // begin data: len32, cmd32
        uint32_t len, cmd;
        co_await async_recv_values(len, cmd);

        endian::little_to_native_inplace(len);
        endian::little_to_native_inplace(cmd);

        bool alive = co_await clientAction(cmd, len);

        if(alive) {
            co_return true;
        }

        // client ended
        if(auto ptr = remote_.lock()) {
            ptr->transactionUnlock(id());
        }

        co_return false;
    }

    asio::awaitable<bool> PcscLocal::clientAction(uint32_t cmd, uint32_t len) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, cmd: {:#010x}, len: {}", NS_FuncNameV, id(), cmd, len);

        switch(cmd) {
            case PcscLite::EstablishContext:
                co_return co_await proxyEstablishContext();

            case PcscLite::ReleaseContext:
                co_return co_await proxyReleaseContext();

            case PcscLite::Connect:
                co_return co_await proxyConnect();

            case PcscLite::Reconnect:
                co_return co_await proxyReconnect();

            case PcscLite::Disconnect:
                co_return co_await proxyDisconnect();

            case PcscLite::BeginTransaction:
                co_return co_await proxyBeginTransaction();

            case PcscLite::EndTransaction:
                co_return co_await proxyEndTransaction();

            case PcscLite::Transmit:
                co_return co_await proxyTransmit();

            case PcscLite::Status:
                co_return co_await proxyStatus();

            case PcscLite::Control:
                co_return co_await proxyControl();

            case PcscLite::GetAttrib:
                co_return co_await proxyGetAttrib();

            case PcscLite::SetAttrib:
                co_return co_await proxySetAttrib();

            case PcscLite::Cancel:
                co_return co_await proxyCancel();

            case PcscLite::GetVersion:
                co_return co_await proxyGetVersion();

            case PcscLite::GetReaderState:
                co_return co_await proxyGetReaderState();

            case PcscLite::WaitReaderStateChangeStart:
                co_return co_await proxyReaderStateChangeStart();

            case PcscLite::WaitReaderStateChangeStop:
                co_return co_await proxyReaderStateChangeStop();

            // not used
            case PcscLite::ListReaders:
            case PcscLite::GetStatusChange:
            case PcscLite::CancelTransaction:
                Application::error("{}: not used cmd: {:#010x}, len: {}", NS_FuncNameV, cmd, len);
                break;

            default:
                Application::error("{}: unknown cmd: {:#010x}, len: {}", NS_FuncNameV, cmd, len);
                break;
        }

        co_return false;
    }

    asio::awaitable<void> PcscLocal::replyError(uint32_t cmd, uint32_t err) {
        uint32_t zero = 0;

        switch(cmd) {
            case PcscLite::EstablishContext:
            case PcscLite::ReleaseContext:
            case PcscLite::Connect:
            case PcscLite::Reconnect:
            case PcscLite::Disconnect:
            case PcscLite::BeginTransaction:
            case PcscLite::EndTransaction:
            case PcscLite::Transmit:
            case PcscLite::Status:
            case PcscLite::Control:
            case PcscLite::GetAttrib:
            case PcscLite::SetAttrib:
                zero = PcscLite::apiCmdLength(cmd) - 4;
                break;

            case PcscLite::Cancel:
            case PcscLite::GetVersion:
            case PcscLite::GetReaderState:
            case PcscLite::WaitReaderStateChangeStart:
            case PcscLite::WaitReaderStateChangeStop:
                Application::warning("{}: not implemented, cmd: {:#010x}", NS_FuncNameV, cmd);
                co_return;

            default:
                Application::error("{}: unknown command, cmd: {:#010x}", NS_FuncNameV, cmd);
                co_return;
        }

        binary_buf buf(zero, 0);
        co_await async_send_values(buf, endian::native_to_little(err));
    }

    asio::awaitable<bool> PcscLocal::proxyEstablishContext(void) {
        uint32_t scope, context, ret;

        co_await async_recv_values(scope, context, ret);

        endian::little_to_native_inplace(scope);
        endian::little_to_native_inplace(context);
        endian::little_to_native_inplace(ret);

        if(auto ptr = remote_.lock()) {
            std::tie(context64_, ret) = co_await ptr->sendEstablishedContext(id(), scope);

            if(ret == SCARD_S_SUCCESS) {
                // make 32bit context
                context = context32_ = ptr->makeContext64(context64_);

                Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context64: {:#018x}, context32: {:#010x}",
                                   NS_FuncNameV, id(), context64_, context32_);

                // init readers status
                co_await ptr->syncReaders(id(), context64_, nullptr);
            } else {
                Application::error("{}: clientId: {}, error: {:#010x} ({})",
                                   NS_FuncNameV, id(), ret, PcscLite::err2str(ret));
            }

        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::EstablishContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        co_await async_send_values(
            endian::native_to_little(scope),
            endian::native_to_little(context),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReleaseContext(void) {
        uint32_t context, ret;
        co_await async_recv_values(context, ret);

        endian::little_to_native_inplace(context);
        endian::little_to_native_inplace(ret);

        if(! context || context != context32_) {
            Application::error("{}: clientId: {}, invalid context32: {:#010x}", NS_FuncNameV, id(), context);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(context64_) {
            std::tie(ret) = co_await ptr->sendReleaseContext(id(), context64_);

            if(ret != SCARD_S_SUCCESS) {
                Application::error("{}: clientId: {}, context32: {:#010x}, error: {:#010x} ({})",
                                   NS_FuncNameV, id(), context32_, ret, PcscLite::err2str(ret));
            }
        }

        co_await async_send_values(
            endian::native_to_little(context),
            endian::native_to_little(ret));

        ptr->removeContext32(context32_);

        context32_ = 0;
        context64_ = 0;

        // set shutdown
        co_return false;
    }

    asio::awaitable<bool> PcscLocal::proxyConnect(void) {
        uint32_t context, shareMode, prefferedProtocols, handle, activeProtocol, ret;
        std::array<char, MAX_READERNAME> readerData{};

        co_await async_recv_values(context, readerData, shareMode, prefferedProtocols, handle, activeProtocol, ret);

        endian::little_to_native_inplace(context);
        endian::little_to_native_inplace(shareMode);
        endian::little_to_native_inplace(prefferedProtocols);
        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(activeProtocol);
        endian::little_to_native_inplace(ret);

        if(! context || context != context32_) {
            Application::error("{}: clientId: {}, invalid context32: {:#010x}", NS_FuncNameV, id(), context);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto readerName = std::string_view{readerData.begin(), std::ranges::find(readerData, 0)};
        auto currentReader = PcscLite::findReaderState(readerName);

        if(! currentReader) {
            Application::error("{}: failed, reader not found: `{}'", NS_FuncNameV, readerName);
            co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! context64_) {
                Application::error("{}: clientId: {}, invalid context64", NS_FuncNameV, id());
                co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(handle64_, activeProtocol, ret) = co_await ptr->sendConnect(id(), context64_, shareMode, prefferedProtocols, readerName);

            if(ret == SCARD_S_SUCCESS) {
                // make local handle
                handle = handle32_ = ret != SCARD_S_SUCCESS ? 0 : ptr->makeContext64(handle64_);
            }
        } else {
            Application::error("{}: failed, reader not found: `{}'", NS_FuncNameV, readerName);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_VALUE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // sync reader
            reader_ = currentReader;

            if(reader_) {
                reader_->share = shareMode;
                reader_->protocol = activeProtocol;
            }

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle64: {:#018x}, handle32: {:#010x}, activeProtocol: {}",
                               NS_FuncNameV, id(), handle64_, handle32_, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, context32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), context32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(context),
            readerData,
            endian::native_to_little(shareMode),
            endian::native_to_little(prefferedProtocols),
            endian::native_to_little(handle),
            endian::native_to_little(activeProtocol),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReconnect(void) {
        uint32_t handle, shareMode, prefferedProtocols, initialization, activeProtocol, ret;
        co_await async_recv_values(handle, shareMode, prefferedProtocols, initialization, activeProtocol, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(shareMode);
        endian::little_to_native_inplace(prefferedProtocols);
        endian::little_to_native_inplace(initialization);
        endian::little_to_native_inplace(activeProtocol);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::Reconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
                co_await replyError(PcscLite::Reconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(activeProtocol, ret) = co_await ptr->sendReconnect(id(), handle64_, shareMode, prefferedProtocols, initialization);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Reconnect, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            assertm(reader_, "reader not connected");
            reader_->share = shareMode;
            reader_->protocol = activeProtocol;
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}, activeProtocol: {}",
                               NS_FuncNameV, id(), handle32_, shareMode, prefferedProtocols, initialization, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(shareMode),
            endian::native_to_little(prefferedProtocols),
            endian::native_to_little(initialization),
            endian::native_to_little(activeProtocol),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyDisconnect(void) {
        uint32_t handle, disposition, ret;
        co_await async_recv_values(handle, disposition, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(disposition);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::Disconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
                co_await replyError(PcscLite::Disconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = co_await ptr->sendDisconnect(id(), handle64_, disposition);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Disconnect, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // sync after
            handle32_ = 0;
            handle64_ = 0;
            assertm(reader_, "reader not connected");

            reader_->share = 0;
            reader_->protocol = 0;
            reader_ = nullptr;

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, disposition: {}",
                               NS_FuncNameV, id(), handle32_, disposition);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(disposition),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyBeginTransaction(void) {
        uint32_t handle, ret;
        co_await async_recv_values(handle, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
                co_await replyError(PcscLite::BeginTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            assertm(reader_, "reader not connected");
            std::tie(ret) = co_await ptr->sendBeginTransaction(id(), handle64_);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}",
                               NS_FuncNameV, id(), handle32_);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyEndTransaction(void) {
        uint32_t handle, disposition, ret;
        co_await async_recv_values(handle, disposition, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(disposition);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
                co_await replyError(PcscLite::EndTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = co_await ptr->sendEndTransaction(id(), handle64_, disposition);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, disposition: {}",
                               NS_FuncNameV, id(), handle32_, disposition);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(disposition),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyTransmit(void) {
        uint32_t handle, ioSendPciProtocol, ioSendPciLength, sendLength,
            ioRecvPciProtocol, ioRecvPciLength, recvLength, ret;

        co_await async_recv_values(handle, ioSendPciProtocol, ioSendPciLength,
            sendLength, ioRecvPciProtocol, ioRecvPciLength, recvLength, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(ioSendPciProtocol);
        endian::little_to_native_inplace(ioSendPciLength);
        endian::little_to_native_inplace(sendLength);
        endian::little_to_native_inplace(ioRecvPciProtocol);
        endian::little_to_native_inplace(ioRecvPciLength);
        endian::little_to_native_inplace(recvLength);
        endian::little_to_native_inplace(ret);

        auto data1 = co_await async_recv_buf<binary_buf>(sendLength);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::Transmit, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        binary_buf data2;
        std::tie(ioRecvPciProtocol, ioRecvPciLength, ret, data2) = co_await ptr->sendTransmit(id(), handle64_, ioSendPciProtocol, ioSendPciLength, recvLength, data1);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, pciProtocol: {:#010x}, pciLength: {}, recv size: {}",
                               NS_FuncNameV, id(), handle32_, ioRecvPciProtocol, ioRecvPciLength, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", NS_FuncNameV, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        recvLength = data2.size();

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(ioSendPciProtocol),
            endian::native_to_little(ioSendPciLength),
            endian::native_to_little(sendLength),
            endian::native_to_little(ioRecvPciProtocol),
            endian::native_to_little(ioRecvPciLength),
            endian::native_to_little(recvLength),
            endian::native_to_little(ret),
            data2);

        co_return ret == SCARD_S_SUCCESS;
    }

    void PcscLocal::statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, reader: `{}', state: {:#010x}, protocol: {}, atrLen: {}",
                           NS_FuncNameV, id(), name, state, protocol, atr.size());

        assertm(reader_, "reader not connected");
        assertm(atr.size() <= sizeof(reader_->atr), "atr length invalid");

        // atr changed
        if(! std::equal(atr.begin(), atr.end(), std::begin(reader_->atr))) {
            std::fill(std::begin(reader_->atr), std::end(reader_->atr), 0);
            std::ranges::copy(atr, std::begin(reader_->atr));

            reader_->atrLen = atr.size();

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(atr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: atr: [{}]", NS_FuncNameV, str);
            }
        }

        // protocol changed
        if(protocol != reader_->protocol) {
            reader_->protocol = protocol;
        }

        if(state != reader_->state) {
            reader_->state = state;
        }
    }

    asio::awaitable<bool> PcscLocal::proxyStatus(void) {
        uint32_t handle, ret;
        co_await async_recv_values(handle, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::Status, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::string name;
        uint32_t state, protocol;
        binary_buf atr;

        std::tie(name, state, protocol, ret, atr) = co_await ptr->sendStatus(id(), handle64_);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}",
                               NS_FuncNameV, id(), handle32_);

            statusApply(name, state, protocol, atr);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyControl(void) {
        uint32_t handle, controlCode, sendLength, recvLength, bytesReturned, ret;
        co_await async_recv_values(handle, controlCode, sendLength, recvLength, bytesReturned, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(controlCode);
        endian::little_to_native_inplace(sendLength);
        endian::little_to_native_inplace(recvLength);
        endian::little_to_native_inplace(bytesReturned);
        endian::little_to_native_inplace(ret);

        auto data1 = co_await async_recv_buf<binary_buf>(sendLength);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::Control, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        binary_buf data2;
        std::tie(ret, data2) = co_await ptr->sendControl(id(), handle64_, controlCode, recvLength, data1);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, controlCode: {:#010x}, bytesReturned: {}",
                               NS_FuncNameV, id(), handle32_, controlCode, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", NS_FuncNameV, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        bytesReturned = data2.size();

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(controlCode),
            endian::native_to_little(sendLength),
            endian::native_to_little(recvLength),
            endian::native_to_little(bytesReturned),
            endian::native_to_little(ret),
            data2);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyGetAttrib(void) {
        uint32_t handle, attrId, attrLen, ret;
        binary_buf attr(MAX_BUFFER_SIZE);

        co_await async_recv_values(handle, attrId, attr, attrLen, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(attrId);
        endian::little_to_native_inplace(attrLen);
        endian::little_to_native_inplace(ret);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::GetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::tie(ret, attr) = co_await ptr->sendGetAttrib(id(), handle64_, attrId);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#010x}, attrId: {}, attrLen: {}",
                               NS_FuncNameV, id(), handle32_, attrId, attr.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(attr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: attr: [{}]", NS_FuncNameV, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        assertm(attr.size() <= MAX_BUFFER_SIZE, "attr length invalid");

        attrLen = std::min(attr.size(), static_cast<size_t>(MAX_BUFFER_SIZE));
        attr.resize(MAX_BUFFER_SIZE, 0);

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(attrId),
            attr,
            endian::native_to_little(attrLen),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxySetAttrib(void) {
        uint32_t handle, attrId, attrLen, ret;
        binary_buf attr(MAX_BUFFER_SIZE);

        co_await async_recv_values(handle, attrId, attr, attrLen, ret);

        endian::little_to_native_inplace(handle);
        endian::little_to_native_inplace(attrId);
        endian::little_to_native_inplace(attrLen);
        endian::little_to_native_inplace(ret);

        // fixed attr
        assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
        attr.resize(attrLen);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#010x}", NS_FuncNameV, id(), handle);
            co_await replyError(PcscLite::SetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", NS_FuncNameV, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::tie(ret) = co_await ptr->sendSetAttrib(id(), handle64_, attrId, attr);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32 {:#010x}",
                               NS_FuncNameV, id(), handle32_);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        // revert attr size
        attr.resize(MAX_BUFFER_SIZE, 0);

        co_await async_send_values(
            endian::native_to_little(handle),
            endian::native_to_little(attrId),
            attr,
            endian::native_to_little(attrLen),
            endian::native_to_little(ret));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyCancel(void) {
        uint32_t context, ret;

        co_await async_recv_values(context, ret);

        endian::little_to_native_inplace(context);
        endian::little_to_native_inplace(ret);

        uint64_t cancelContext = 0;

        if(auto ptr = remote_.lock()) {
            cancelContext = ptr->findContext32(context);
            Application::debug(DebugType::Pcsc, "{}: clientId: {}, cancel context {:#010x}, remote: {:#010x}",
                               NS_FuncNameV, id(), context, cancelContext);
            std::tie(ret) = co_await ptr->sendCancel(id(), cancelContext);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_await replyError(PcscLite::Cancel, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32 {:#010x}",
                               NS_FuncNameV, id(), context32_);
        } else {
            Application::error("{}: clientId: {}, context32: {:#010x}, error: {:#010x} ({})",
                               NS_FuncNameV, id(), context32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_values(
            endian::native_to_little(context),
            endian::native_to_little(ret));

        asio::post(socket().get_executor(),
                       std::bind(&PcscSessionBus::stopClientContext, session_, cancelContext));

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetVersion(void) {
        uint32_t versionMajor, versionMinor, ret;
        co_await async_recv_values(versionMajor, versionMinor, ret);

        endian::little_to_native_inplace(versionMajor);
        endian::little_to_native_inplace(versionMinor);
        endian::little_to_native_inplace(ret);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> protocol version: {}.{}",
                           NS_FuncNameV, id(), versionMajor, versionMinor);

        // supported only 4.4 protocol or higher
        if(versionMajor * 10 + versionMinor < 44) {
            Application::warning("{}: clientId: {}, unsupported version: version: {}.{}",
                                 NS_FuncNameV, id(), versionMajor, versionMinor);
            ret = SCARD_E_NO_SERVICE;
        }

        co_await async_send_values(
            endian::native_to_little(versionMajor),
            endian::native_to_little(versionMinor),
            endian::native_to_little(ret));

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetReaderState(void) {

        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32: {:#010x}, readers length: {}",
                           NS_FuncNameV, id(), context32_, readersLength);

        // send all readers
        co_await async_send_buf(asio::buffer(PcscLite::readers.data(), readersLength));
        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStart(void) {
        // new protocol 4.4: empty params
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context32: {:#010x}, timeout: {}",
                           NS_FuncNameV, id(), context32_);

        if(auto ptr = remote_.lock()) {
            co_await ptr->syncReaderTimerStart(id(), context64_);
        } else {
            Application::error("{}: no service", NS_FuncNameV);
            co_return false;
        }

        // send all readers
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
        co_await async_send_buf(asio::buffer(PcscLite::readers.data(), readersLength));

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStop(void) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context32: {:#010x}",
                           NS_FuncNameV, id(), context32_);

        if(auto ptr = remote_.lock()) {
            ptr->syncReaderTimerStop();
        }

        const uint32_t timeout = 0;
        const uint32_t ret = SCARD_S_SUCCESS;

        co_await async_send_values(
            endian::native_to_little(timeout),
            endian::native_to_little(ret));

        co_return true;
    }

    /// PcscSessionBus
    PcscSessionBus::PcscSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_pcsc"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_pcsc_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_pcsc_path),
#endif
        SDBus::AsioCoroConnector(std::move(conn)),
        signals_ {ioc_} {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    PcscSessionBus::~PcscSessionBus() {
        unregisterAdaptor();
    }

    void PcscSessionBus::stop(void) {
        sdbusLoopCancel();
        remote_cancel_.emit(asio::cancellation_type::terminal);
        listen_stop_.emit(asio::cancellation_type::terminal);
        clients_.clear();
        remote_.reset();
        signals_.cancel();
    }

    asio::awaitable<void> PcscSessionBus::signalsHandler(void) {
        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        try {
            for(;;) {
                int signal = co_await signals_.async_wait(asio::use_awaitable);
                if(signal == SIGTERM || signal == SIGINT) {
                    asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
                    co_return;
                }
            }
        } catch(const system::system_error& err) {
            auto ec = err.code();
            if(ec != asio::error::operation_aborted) {
                Application::error("{}: system error: `{}', code: {}",
                    NS_FuncNameV, ec.message(), ec.value());
            }
        }
    }

    asio::awaitable<void> PcscSessionBus::sdbusHandler(void) {
        try {
            co_await sdbusEventLoop();
        } catch(const system::system_error& err) {
            auto ec = err.code();
            Application::error("{}: system error: `{}', code: {}",
                    NS_FuncNameV, ec.message(), ec.value());
        } catch(const sdbus::Error& err) {
            Application::error("{}: failed, sdbus error: {}", NS_FuncNameV, err.getName());
            asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
        }
    }

    int PcscSessionBus::start(void) {
        Application::info("service started, uid: {}, pid: {}, version: {}", getuid(), getpid(), LTSM_SESSION_PCSC_VERSION);

        auto pcsc_path = getenv("PCSCLITE_CSOCK_NAME");

        if(! pcsc_path) {
            Application::error("{}: environment not found: {}", NS_FuncNameV, "PCSCLITE_CSOCK_NAME");
            return EXIT_FAILURE;
        }

        Application::info("{}: socket path: `{}'", NS_FuncNameV, pcsc_path);

        pcsc_ep_.path(pcsc_path);
        remote_ = std::make_shared<PcscRemote>(asio::local::stream_protocol::socket{ioc_});

        // main loop
        asio::co_spawn(ioc_, sdbusHandler(), asio::detached);
        asio::co_spawn(ioc_, signalsHandler(), asio::detached);
        ioc_.run();

        Application::notice("{}: service shutdown", NS_FuncNameV);
        std::filesystem::remove(pcsc_path);

        return EXIT_SUCCESS;
    }

    int32_t PcscSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", NS_FuncNameV);
        return LTSM_SESSION_PCSC_VERSION;
    }

    void PcscSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", NS_FuncNameV, getpid());
        asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
    }

    void PcscSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", NS_FuncNameV, level);
        setDebugLevel(level);
    }

    void PcscSessionBus::stopClientContext(uint64_t ctx) {

        auto it = std::find_if(clients_.begin(), clients_.end(), [ = ](auto & cli) {
            return cli.proxyContext() == ctx;
        });

        if(it != clients_.end()) {
            Application::debug(DebugType::Dbus, "{}: stop remote: {:#018x}", "stopClientHandler", ctx);

            asio::co_spawn(clients_guard_, [it]() -> asio::awaitable<void> {
                it->stopSignal();
                co_return;
            }, asio::detached);
        }
    }

    asio::awaitable<void> PcscSessionBus::acceptHandler(PcscLocal & client) {

        Application::debug(DebugType::App, "{}: clientId: {}", NS_FuncNameV, client.id());
        bool success = true;

        while(success) {
            try {
                success = co_await client.handlerClientWaitCommand();
            } catch(const system::system_error& err) {
                auto ec = err.code();

                if(ec != asio::error::eof && ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: {}, code: {}",
                                       NS_FuncNameV, ec.message(), ec.value());
                }

                co_return;
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
                co_return;
            }

            if(remote_->isError()) {
                asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
                success = false;
            }
        }

        co_return;
    }

    void PcscSessionBus::clientStoppedHandler(const PcscLocal* client, std::exception_ptr eptr) {
        auto it = std::find_if(clients_.begin(), clients_.end(), [&](auto & cli) {
            return client == std::addressof(cli);
        });

        if(it != clients_.end()) {
            Application::debug(DebugType::Dbus, "{}: clientId: {}, destroy", NS_FuncNameV, it->id());

            remote_->transactionUnlock(it->id());
            clients_.erase(it);
        }
    }

    asio::awaitable<void> PcscSessionBus::listenerHandler(void) {
        // remote connected
        if(std::filesystem::is_socket(pcsc_ep_.path())) {
            std::filesystem::remove(pcsc_ep_.path());
            Application::warning("{}: old socket removed", NS_FuncNameV);
        }

        try {
            auto executor = co_await asio::this_coro::executor;
            asio::local::stream_protocol::acceptor acceptor(executor, pcsc_ep_);
            int client_id = 11;

            for(;;) {
                asio::local::stream_protocol::socket peer = co_await acceptor.async_accept(asio::use_awaitable);

                clients_.emplace_back(std::move(peer), client_id++, remote_, this);
                auto & client = clients_.back();

                auto token = asio::bind_cancellation_slot(client.stopSlot(),
                             std::bind(&PcscSessionBus::clientStoppedHandler, this, &client, std::placeholders::_1));

                asio::co_spawn(executor, acceptHandler(client), std::move(token));
            }
        } catch(const system::system_error& err) {
            auto ec = err.code();

            if(ec != asio::error::operation_aborted) {
                Application::error("{}: system error: {}, code: {}", NS_FuncNameV, ec.message(), ec.value());
            }
        }

        for(auto & cli : clients_) {
            cli.stopSignal();
        }
    }

    bool PcscSessionBus::connectChannel(const std::string & socketPath) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", NS_FuncNameV, socketPath);

        if(remote_->isConnected()) {
            Application::warning("{}: also connected, socket path: `{}'", NS_FuncNameV, remote_->socketPath());
            return false;
        }

        asio::co_spawn(ioc_,
        [socketPath, this]() -> asio::awaitable<void>  {
            auto executor = co_await asio::this_coro::executor;

            try {
                co_await remote_->retryConnect(socketPath, 5);
                co_await remote_->remoteHandshake();

                // start local listener
                asio::co_spawn(clients_guard_, listenerHandler(),
                           asio::bind_cancellation_slot(listen_stop_.slot(), asio::detached));

            } catch(const system::system_error& err) {
                auto ec = err.code();
                if(ec != asio::error::operation_aborted) {
                    Application::error("{}: system error: `{}', code: {}",
                        NS_FuncNameV, ec.message(), ec.value());
                }
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", NS_FuncNameV, err.what());
            }

        }, asio::bind_cancellation_slot(remote_cancel_.slot(), asio::detached));

        return true;
    }

    void PcscSessionBus::disconnectChannel(const std::string & clientPath) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", NS_FuncNameV, clientPath);

        remote_.reset();
    }
}

int main(int argc, char** argv) {
    bool debug = false;

    for(int it = 1; it < argc; ++it) {
        if(0 == std::strcmp(argv[it], "--help") || 0 == std::strcmp(argv[it], "-h")) {
            std::cout << "usage: " << argv[0] << " [--version] [--debug]" << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--version") || 0 == std::strcmp(argv[it], "-v")) {
            std::cout << "version: " << LTSM_SESSION_PCSC_VERSION << std::endl;
            return EXIT_SUCCESS;
        } else if(0 == std::strcmp(argv[it], "--debug") || 0 == std::strcmp(argv[it], "-d")) {
            debug = true;
        }
    }

    if(0 == getuid()) {
        std::cerr << "for users only" << std::endl;
        return EXIT_FAILURE;
    }

    try {
#ifdef SDBUS_2_0_API
        auto conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_pcsc_name});
#else
        auto conn = sdbus::createSessionBusConnection(LTSM::dbus_session_pcsc_name);
#endif

        return LTSM::PcscSessionBus(std::move(conn), debug).start();
    } catch(const std::exception & err) {
        LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return EXIT_FAILURE;
}
