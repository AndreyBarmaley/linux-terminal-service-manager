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

#include "avast_asio_async_mutex.hpp"

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
                LTSM::Application::warning("{}: unknown cmd: {:#08x}", __FUNCTION__, cmd);
                break;
        }

        return 0;
    }
}

namespace LTSM {
    struct transaction_lock {
        avast::asio::async_mutex trans_lock;
        int32_t client_id{0};

        asio::awaitable<void> lock(int32_t id) {
            if(id) {
                co_await trans_lock.async_lock(asio::use_awaitable);
                client_id = id;
            }
            co_return;
        }

        void unlock(int32_t id = 0) {
            if((0 == id && 0 < client_id) || client_id == id) {
                client_id = 0;
                trans_lock.unlock();
            }
        }
    };

    avast::asio::async_mutex send_lock;
    transaction_lock trans_lock;

    std::unordered_map<uint32_t, uint64_t> map_context;

    uint32_t makeContext32(uint64_t remote) {
        uint32_t context = Tools::crc32b((const uint8_t*) & remote, sizeof(remote));
        context &= 0x7FFFFFFF;
        map_context.emplace(context, remote);
        return context;
    }

    uint64_t findContext64(uint32_t local) {
        auto it = map_context.find(local);
        return it != map_context.end() ? it->second : 0;
    }

    /// PcscRemote
    asio::awaitable<bool> PcscRemote::handlerWaitConnect(const std::string & path) {
        try {
            co_await socket().async_connect(path, asio::use_awaitable);
            connected_ = true;
        } catch(const std::exception & ex) {
            Application::error("{}: exception: {}", __FUNCTION__, ex.what());
        }

        co_return connected_;
    }

    asio::awaitable<RetEstablishedContext>
    PcscRemote::sendEstablishedContext(const int32_t & id, const uint32_t & scope) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << scope: {}", __FUNCTION__, id, scope);

        uint64_t context;
        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::EstablishContext);
            co_await async_send_le32(scope);

            context = co_await async_recv_le64();
            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(context, ret);
    }

    asio::awaitable<RetReleaseContext>
    PcscRemote::sendReleaseContext(const int32_t & id, const uint64_t & context) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64: {:#016x}",
                           __FUNCTION__, id, context);

        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::ReleaseContext);
            co_await async_send_le64(context);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetConnect>
    PcscRemote::sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, std::string_view readerName) {
        co_await trans_lock.lock(id);
        co_await send_lock.async_lock(asio::use_awaitable);
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64: {:#016x}, shareMode: {}, prefferedProtocols: {}, reader: `{}'",
                           __FUNCTION__, id, context, shareMode, prefferedProtocols, readerName);

        uint64_t handle;
        uint32_t activeProtocol, ret;

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
            write_le16(PcscLite::Connect).
            write_le64(context).
            write_le32(shareMode).
            write_le32(prefferedProtocols).
            write_le32(readerName.size()).
            write_string(readerName);

        try {
            co_await async_send_buf(sb);

            handle = co_await async_recv_le64();
            activeProtocol = co_await async_recv_le32();
            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        trans_lock.unlock();

        co_return std::make_tuple(handle, activeProtocol, ret);
    }

    asio::awaitable<RetReconnect>
    PcscRemote::sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}",
                           __FUNCTION__, id, handle, shareMode, prefferedProtocols, initialization);
        uint32_t activeProtocol, ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::Reconnect);
            co_await async_send_le64(handle);
            co_await async_send_le32(shareMode);
            co_await async_send_le32(prefferedProtocols);
            co_await async_send_le32(initialization);

            activeProtocol = co_await async_recv_le32();
            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(activeProtocol, ret);
    }

    asio::awaitable<RetDisconnect>
    PcscRemote::sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, disposition: {}",
                           __FUNCTION__, id, handle, disposition);
        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::Disconnect);
            co_await async_send_le64(handle);
            co_await async_send_le32(disposition);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransaction>
    PcscRemote::sendBeginTransaction(const int32_t & id, const uint64_t & handle) {
        co_await trans_lock.lock(id);
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}",
                           __FUNCTION__, id, handle);
        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::BeginTransaction);
            co_await async_send_le64(handle);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        if(ret != SCARD_S_SUCCESS) {
            trans_lock.unlock();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransaction>
    PcscRemote::sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, disposition: {}",
                           __FUNCTION__, id, handle, disposition);
        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::EndTransaction);
            co_await async_send_le64(handle);
            co_await async_send_le32(disposition);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        trans_lock.unlock();

        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetTransmit>
    PcscRemote::sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data1) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, pciProtocol: {:#08x}, pciLength: {}, send size: {}, recv size: {}",
                           __FUNCTION__, id, handle, ioSendPciProtocol, ioSendPciLength, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", __FUNCTION__, str);
        }

        uint32_t ioRecvPciProtocol, ioRecvPciLength, ret;
        binary_buf data2;

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
            write_le16(PcscLite::Transmit).
            write_le64(handle).
            write_le32(ioSendPciProtocol).
            write_le32(ioSendPciLength).
            write_le32(recvLength).
            write_le32(data1.size()).
            write_bytes(data1);

        try {
            co_await async_send_buf(sb);

            ioRecvPciProtocol = co_await async_recv_le32();
            ioRecvPciLength = co_await async_recv_le32();
            auto bytesReturned = co_await async_recv_le32();
            ret = co_await async_recv_le32();
            data2 = co_await async_recv_buf<binary_buf>(bytesReturned);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, std::move(data2));
    }

    inline void fixed_string_name(std::string & str) {
        while(str.size() && str.back() == 0) {
            str.pop_back();
        }
    }

    asio::awaitable<RetStatus>
    PcscRemote::sendStatus(const int32_t & id, const uint64_t & handle) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}",
                           __FUNCTION__, id, handle);

        uint32_t state, protocol, atrLen, ret;
        std::string readerName;
        binary_buf atr;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::Status);
            co_await async_send_le64(handle);

            auto nameLen = co_await async_recv_le32();
            readerName = co_await async_recv_buf<std::string>(nameLen);
            state = co_await async_recv_le32();
            protocol = co_await async_recv_le32();
            auto atrLen = co_await async_recv_le32();
            atr = co_await async_recv_buf<binary_buf>(atrLen);
            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        fixed_string_name(readerName);

        send_lock.unlock();
        co_return std::make_tuple(std::move(readerName), state, protocol, ret, std::move(atr));
    }

    asio::awaitable<RetControl>
    PcscRemote::sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, controlCode: {:#08x}, send size: {}, recv size: {}",
                           __FUNCTION__, id, handle, controlCode, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", __FUNCTION__, str);
        }

        uint32_t ret;
        binary_buf data2;

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
            write_le16(PcscLite::Control).
            write_le64(handle).
            write_le32(controlCode).
            write_le32(data1.size()).
            write_le32(recvLength).
            write_bytes(data1);

        try {
            co_await async_send_buf(sb);

            auto bytesReturned = co_await async_recv_le32();
            ret = co_await async_recv_le32();
            data2 = co_await async_recv_buf<binary_buf>(bytesReturned);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret, std::move(data2));
    }

    asio::awaitable<RetGetAttrib>
    PcscRemote::sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64: {:#016x}, attrId: {}",
                           __FUNCTION__, id, handle, attrId);

        uint32_t ret;
        binary_buf attr;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::GetAttrib);
            co_await async_send_le64(handle);
            co_await async_send_le32(attrId);

            auto attrLen = co_await async_recv_le32();
            ret = co_await async_recv_le32();

            assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
            attr = co_await async_recv_buf<binary_buf>(attrLen);
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret, std::move(attr));
    }

    asio::awaitable<RetSetAttrib>
    PcscRemote::sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << handle64 {:#016x}, attrId: {}, attrLength {}",
                           __FUNCTION__, id, handle, attrId, attr.size());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(attr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: attr: [{}]", __FUNCTION__, str);
        }

        uint32_t ret;

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
            write_le16(PcscLite::SetAttrib).
            write_le64(handle).
            write_le32(attrId).
            write_le32(attr.size()).
            write_bytes(attr);

        try {
            co_await async_send_buf(sb);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<RetCancel>
    PcscRemote::sendCancel(const int32_t & id, const uint64_t & context) {
        co_await send_lock.async_lock(asio::use_awaitable);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context64 {:#016x}",
                           __FUNCTION__, id, context);

        uint32_t ret;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::Cancel);
            co_await async_send_le64(context);

            ret = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        if(timer_context_ == context) {
            timer_stop_.emit(asio::cancellation_type::terminal);
            timer_context_ = 0;
        }

        send_lock.unlock();
        co_return std::make_tuple(ret);
    }

    asio::awaitable<ListReaders> PcscRemote::sendListReaders(const int32_t & id, const uint64_t & context) {
        co_await send_lock.async_lock(asio::use_awaitable);

        uint32_t readersCount;

        try {
            co_await async_send_le16(PcscOp::Init);
            co_await async_send_le16(PcscLite::ListReaders);
            co_await async_send_le64(context);

            readersCount = co_await async_recv_le32();
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32: {:#08x}, readers count: {}",
                           __FUNCTION__, id, context, readersCount);

        ListReaders names;

        while(readersCount--) {
            try {
                auto readerLen = co_await async_recv_le32();
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

        send_lock.unlock();
        co_return names;
    }

    asio::awaitable<uint32_t> PcscRemote::sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount) {
        co_await send_lock.async_lock(asio::use_awaitable);

        uint32_t ret;

        try {
            asio::streambuf sb;
            byte::streambuf bs(sb);

            bs.write_le16(PcscOp::Init).
                write_le16(PcscLite::GetStatusChange).
                write_le64(context).
                write_le32(timeout).
                write_le32(statesCount);

            for(uint32_t it = 0; it < statesCount; ++it) {
                const SCARD_READERSTATE & state = states[it];
                auto len = strnlen(state.szReader, MAX_READERNAME);

                bs.write_le32(len).
                    write_le32(state.dwCurrentState).
                    write_le32(state.cbAtr).
                    write_bytes(reinterpret_cast<const uint8_t*>(state.szReader), len).
                    write_bytes(reinterpret_cast<const uint8_t*>(state.rgbAtr), state.cbAtr);
            }

            co_await async_send_buf(sb);

            auto counts = co_await async_recv_le32();
            ret = co_await async_recv_le32();

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context64: {:#016x}, timeout: {}, states: {}",
                               __FUNCTION__, id, context, timeout, counts);

            assertm(counts == statesCount, "count states invalid");

            for(uint32_t it = 0; it < statesCount; ++it) {
                SCARD_READERSTATE & state = states[it];

                state.dwCurrentState = co_await async_recv_le32();
                state.dwEventState = co_await async_recv_le32();

                auto readerLen = co_await async_recv_le32();
                auto cbAtr = co_await async_recv_le32();
                auto readerName = co_await async_recv_buf<std::string>(readerLen);

                if(readerName != state.szReader) {
                    Application::warning("{}: invalid reader, `{}' != `{}'", __FUNCTION__, readerName, state.szReader);
                }

                assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

                state.cbAtr = cbAtr;
                auto atr = co_await async_recv_buf<binary_buf>(cbAtr);

                atr.resize(sizeof(state.rgbAtr), 0);
                std::ranges::copy_n(atr.data(), atr.size(), state.rgbAtr);
            }
        } catch(const system::system_error& err) {
            ec_ = err.code();
        }

        send_lock.unlock();
        co_return ret;
    }

    asio::awaitable<uint32_t> PcscRemote::syncReaders(const int32_t & id, const uint64_t & context, bool* changed) {
        ListReaders names = co_await sendListReaders(id, context);

        if(names.empty()) {
            Application::warning("{}: no readers available", __FUNCTION__);

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
                Application::debug(DebugType::Pcsc, "{}: added reader, name: `{}'", __FUNCTION__, name);
                // find unused slot
                auto rd = std::ranges::find_if(PcscLite::readers, [](auto & rd) {
                    return 0 == rd.name[0];
                });

                if(rd == PcscLite::readers.end()) {
                    LTSM::Application::error("{}: failed, {}", __FUNCTION__, "all slots is busy");
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
            Application::warning("{}: timeout", __FUNCTION__);
            co_return ret;
        }

        if(ret != SCARD_S_SUCCESS) {
            Application::warning("{}: error: {:#08x} ({})", __FUNCTION__, ret, PcscLite::err2str(ret));
            co_return ret;
        }

        Application::debug(DebugType::Pcsc, "{}: reader: `{}', currentState: {:#08x}, eventState: {:#08x}, atrLen: {}",
                           __FUNCTION__, readerName, state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::rangeHexString(state.rgbAtr, state.rgbAtr + state.cbAtr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: atr: [{}]", __FUNCTION__, str);
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

    /// PcscLocal
    PcscLocal::~PcscLocal() {
        socket().cancel();
        socket().close();
    }

    asio::awaitable<bool> PcscLocal::handlerClientWaitCommand(void) {
        // begin data: len32, cmd32
        uint32_t len = co_await async_recv_le32();
        uint32_t cmd = co_await async_recv_le32();

        bool alive = co_await clientAction(cmd, len);

        if(alive) {
            co_return true;
        }

        // client ended
        trans_lock.unlock(id());

        co_return false;
    }

    asio::awaitable<bool> PcscLocal::clientAction(uint32_t cmd, uint32_t len) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, cmd: {:#08x}, len: {}", __FUNCTION__, id(), cmd, len);

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
                Application::error("{}: not used cmd: {:#08x}, len: {}", __FUNCTION__, cmd, len);
                break;

            default:
                Application::error("{}: unknown cmd: {:#08x}, len: {}", __FUNCTION__, cmd, len);
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
                Application::warning("{}: not implemented, cmd: {:#08x}", __FUNCTION__, cmd);
                co_return;

            default:
                Application::error("{}: unknown command, cmd: {:#08x}", __FUNCTION__, cmd);
                co_return;
        }

        if(zero) {
            binary_buf buf(zero, 0);
            co_await async_send_buf(asio::buffer(buf));
        }

        co_await async_send_le32(err);
    }

    asio::awaitable<bool> PcscLocal::proxyEstablishContext(void) {
        auto scope = co_await async_recv_le32();
        auto context = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(auto ptr = remote_.lock()) {
            std::tie(context64_, ret) = co_await ptr->sendEstablishedContext(id(), scope);

            if(ret == SCARD_S_SUCCESS) {
                // make 32bit context
                context = context32_ = makeContext32(context64_);

                Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context64: {:#016x}, context32: {:#08x}",
                                   __FUNCTION__, id(), context64_, context32_);

                // init readers status
                co_await ptr->syncReaders(id(), context64_, nullptr);
            } else {
                Application::error("{}: clientId: {}, error: {:#08x} ({})",
                                   __FUNCTION__, id(), ret, PcscLite::err2str(ret));
            }

        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::EstablishContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        co_await async_send_le32(scope);
        co_await async_send_le32(context);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReleaseContext(void) {
        auto context = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(! context || context != context32_) {
            Application::error("{}: clientId: {}, invalid context32: {:#08x}", __FUNCTION__, id(), context);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(context64_) {
            std::tie(ret) = co_await ptr->sendReleaseContext(id(), context64_);

            if(ret != SCARD_S_SUCCESS) {
                Application::error("{}: clientId: {}, context32: {:#08x}, error: {:#08x} ({})",
                                   __FUNCTION__, id(), context32_, ret, PcscLite::err2str(ret));
            }
        }

        co_await async_send_le32(context);
        co_await async_send_le32(ret);

        context32_ = 0;
        context64_ = 0;

        // set shutdown
        co_return false;
    }

    asio::awaitable<bool> PcscLocal::proxyConnect(void) {
        auto context = co_await async_recv_le32();
        auto readerData = co_await async_recv_buf<char_buf>(MAX_READERNAME);
        auto shareMode = co_await async_recv_le32();
        auto prefferedProtocols = co_await async_recv_le32();
        auto handle = co_await async_recv_le32();
        auto activeProtocol = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(! context || context != context32_) {
            Application::error("{}: clientId: {}, invalid context32: {:#08x}", __FUNCTION__, id(), context);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto readerName = std::string_view{readerData.begin(), std::ranges::find(readerData, 0)};
        auto currentReader = PcscLite::findReaderState(readerName);

        if(! currentReader) {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! context64_) {
                Application::error("{}: clientId: {}, invalid context64", __FUNCTION__, id());
                co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(handle64_, activeProtocol, ret) = co_await ptr->sendConnect(id(), context64_, shareMode, prefferedProtocols, readerName);
        } else {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_VALUE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make local handle
            handle = handle32_ = ret != SCARD_S_SUCCESS ? 0 : makeContext32(handle64_);

            // sync reader
            reader_ = currentReader;

            if(reader_) {
                reader_->share = shareMode;
                reader_->protocol = activeProtocol;
            }

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle64: {:#016x}, handle32: {:#08x}, activeProtocol: {}",
                               __FUNCTION__, id(), handle64_, handle32_, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, context32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), context32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(context);
        co_await async_send_buf(asio::buffer(readerData));
        co_await async_send_le32(shareMode);
        co_await async_send_le32(prefferedProtocols);
        co_await async_send_le32(handle);
        co_await async_send_le32(activeProtocol);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReconnect(void) {
        auto handle = co_await async_recv_le32();
        auto shareMode = co_await async_recv_le32();
        auto prefferedProtocols = co_await async_recv_le32();
        auto initialization = co_await async_recv_le32();
        auto activeProtocol = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::Reconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
                co_await replyError(PcscLite::Reconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(activeProtocol, ret) = co_await ptr->sendReconnect(id(), handle64_, shareMode, prefferedProtocols, initialization);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Reconnect, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            assertm(reader_, "reader not connected");
            reader_->share = shareMode;
            reader_->protocol = activeProtocol;
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}, activeProtocol: {}",
                               __FUNCTION__, id(), handle32_, shareMode, prefferedProtocols, initialization, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(handle);
        co_await async_send_le32(shareMode);
        co_await async_send_le32(prefferedProtocols);
        co_await async_send_le32(initialization);
        co_await async_send_le32(activeProtocol);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyDisconnect(void) {
        auto handle = co_await async_recv_le32();
        auto disposition = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::Disconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
                co_await replyError(PcscLite::Disconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = co_await ptr->sendDisconnect(id(), handle64_, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
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

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, disposition: {}",
                               __FUNCTION__, id(), handle32_, disposition);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(handle);
        co_await async_send_le32(disposition);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyBeginTransaction(void) {
        auto handle = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
                co_await replyError(PcscLite::BeginTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            assertm(reader_, "reader not connected");
            std::tie(ret) = co_await ptr->sendBeginTransaction(id(), handle64_);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}",
                               __FUNCTION__, id(), handle32_);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(handle);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyEndTransaction(void) {
        auto handle = co_await async_recv_le32();
        auto disposition = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(auto ptr = remote_.lock()) {
            if(! handle64_) {
                Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
                co_await replyError(PcscLite::EndTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = co_await ptr->sendEndTransaction(id(), handle64_, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, disposition: {}",
                               __FUNCTION__, id(), handle32_, disposition);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(handle);
        co_await async_send_le32(disposition);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyTransmit(void) {
        auto handle = co_await async_recv_le32();
        auto ioSendPciProtocol = co_await async_recv_le32();
        auto ioSendPciLength = co_await async_recv_le32();
        auto sendLength = co_await async_recv_le32();
        auto ioRecvPciProtocol = co_await async_recv_le32();
        auto ioRecvPciLength = co_await async_recv_le32();
        auto recvLength = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();
        auto data1 = co_await async_recv_buf<binary_buf>(sendLength);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::Transmit, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        binary_buf data2;
        std::tie(ioRecvPciProtocol, ioRecvPciLength, ret, data2) = co_await ptr->sendTransmit(id(), handle64_, ioSendPciProtocol, ioSendPciLength, recvLength, data1);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, pciProtocol: {:#08x}, pciLength: {}, recv size: {}",
                               __FUNCTION__, id(), handle32_, ioRecvPciProtocol, ioRecvPciLength, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        recvLength = data2.size();

        co_await async_send_le32(handle);
        co_await async_send_le32(ioSendPciProtocol);
        co_await async_send_le32(ioSendPciLength);
        co_await async_send_le32(sendLength);
        co_await async_send_le32(ioRecvPciProtocol);
        co_await async_send_le32(ioRecvPciLength);
        co_await async_send_le32(recvLength);
        co_await async_send_le32(ret);
        co_await async_send_buf(asio::buffer(data2));

        co_return ret == SCARD_S_SUCCESS;
    }

    void PcscLocal::statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, reader: `{}', state: {:#08x}, protocol: {}, atrLen: {}",
                           __FUNCTION__, id(), name, state, protocol, atr.size());

        assertm(reader_, "reader not connected");
        assertm(atr.size() <= sizeof(reader_->atr), "atr length invalid");

        // atr changed
        if(! std::equal(atr.begin(), atr.end(), std::begin(reader_->atr))) {
            std::fill(std::begin(reader_->atr), std::end(reader_->atr), 0);
            std::ranges::copy(atr, std::begin(reader_->atr));

            reader_->atrLen = atr.size();

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(atr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: atr: [{}]", __FUNCTION__, str);
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
        auto handle = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::Status, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::string name;
        uint32_t state, protocol;
        binary_buf atr;

        std::tie(name, state, protocol, ret, atr) = co_await ptr->sendStatus(id(), handle64_);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}",
                               __FUNCTION__, id(), handle32_);

            statusApply(name, state, protocol, atr);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(handle);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyControl(void) {
        auto handle = co_await async_recv_le32();
        auto controlCode = co_await async_recv_le32();
        auto sendLength = co_await async_recv_le32();
        auto recvLength = co_await async_recv_le32();
        auto bytesReturned = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();
        auto data1 = co_await async_recv_buf<binary_buf>(sendLength);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::Control, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        binary_buf data2;
        std::tie(ret, data2) = co_await ptr->sendControl(id(), handle64_, controlCode, recvLength, data1);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, controlCode: {:#08x}, bytesReturned: {}",
                               __FUNCTION__, id(), handle32_, controlCode, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        bytesReturned = data2.size();

        co_await async_send_le32(handle);
        co_await async_send_le32(controlCode);
        co_await async_send_le32(sendLength);
        co_await async_send_le32(recvLength);
        co_await async_send_le32(bytesReturned);
        co_await async_send_le32(ret);
        co_await async_send_buf(asio::buffer(data2));

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyGetAttrib(void) {
        auto handle = co_await async_recv_le32();
        auto attrId = co_await async_recv_le32();
        auto attr = co_await async_recv_buf<binary_buf>(MAX_BUFFER_SIZE);
        auto attrLen = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::GetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::tie(ret, attr) = co_await ptr->sendGetAttrib(id(), handle64_, attrId);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32: {:#08x}, attrId: {}, attrLen: {}",
                               __FUNCTION__, id(), handle32_, attrId, attr.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(attr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: attr: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        assertm(attr.size() <= MAX_BUFFER_SIZE, "attr length invalid");

        attrLen = std::min(attr.size(), static_cast<size_t>(MAX_BUFFER_SIZE));
        attr.resize(MAX_BUFFER_SIZE, 0);

        co_await async_send_le32(handle);
        co_await async_send_le32(attrId);
        co_await async_send_buf(asio::buffer(attr));
        co_await async_send_le32(attrLen);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxySetAttrib(void) {
        auto handle = co_await async_recv_le32();
        auto attrId = co_await async_recv_le32();
        auto attr = co_await async_recv_buf<binary_buf>(MAX_BUFFER_SIZE);
        auto attrLen = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        // fixed attr
        assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
        attr.resize(attrLen);

        if(handle != handle32_) {
            Application::error("{}: clientId: {}, invalid handle32: {:#08x}", __FUNCTION__, id(), handle);
            co_await replyError(PcscLite::SetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(! handle64_) {
            Application::error("{}: clientId: {}, invalid handle64", __FUNCTION__, id());
            co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        std::tie(ret) = co_await ptr->sendSetAttrib(id(), handle64_, attrId, attr);

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> handle32 {:#08x}",
                               __FUNCTION__, id(), handle32_);
        } else {
            Application::error("{}: clientId: {}, handle32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle32_, ret, PcscLite::err2str(ret));
        }

        // revert attr size
        attr.resize(MAX_BUFFER_SIZE, 0);

        co_await async_send_le32(handle);
        co_await async_send_le32(attrId);
        co_await async_send_buf(asio::buffer(attr));
        co_await async_send_le32(attrLen);
        co_await async_send_le32(ret);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyCancel(void) {
        auto context = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        uint64_t cancelContext = 0;

        if(auto ptr = remote_.lock()) {
            cancelContext = findContext64(context);
            Application::debug(DebugType::Pcsc, "{}: clientId: {}, cancel context {:#08x}, remote: {:#08x}",
                               __FUNCTION__, id(), context, cancelContext);
            std::tie(ret) = co_await ptr->sendCancel(id(), cancelContext);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Cancel, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32 {:#08x}",
                               __FUNCTION__, id(), context32_);
        } else {
            Application::error("{}: clientId: {}, context32: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), context32_, ret, PcscLite::err2str(ret));
        }

        co_await async_send_le32(context);
        co_await async_send_le32(ret);

        asio::co_spawn(socket().get_executor(),
                       std::bind(&PcscSessionBus::handlerStopClient, session_, cancelContext), asio::detached);

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetVersion(void) {
        auto versionMajor = co_await async_recv_le32();
        auto versionMinor = co_await async_recv_le32();
        auto ret = co_await async_recv_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> protocol version: {}.{}",
                           __FUNCTION__, id(), versionMajor, versionMinor);

        // supported only 4.4 protocol or higher
        if(versionMajor * 10 + versionMinor < 44) {
            Application::warning("{}: clientId: {}, unsupported version: version: {}.{}",
                                 __FUNCTION__, id(), versionMajor, versionMinor);
            ret = SCARD_E_NO_SERVICE;
        }

        co_await async_send_le32(versionMajor);
        co_await async_send_le32(versionMinor);
        co_await async_send_le32(ret);

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetReaderState(void) {

        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> context32: {:#08x}, readers length: {}",
                           __FUNCTION__, id(), context32_, readersLength);

        // send all readers
        co_await async_send_buf(asio::buffer(PcscLite::readers.data(), readersLength));
        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStart(void) {
        // new protocol 4.4: empty params
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context32: {:#08x}, timeout: {}",
                           __FUNCTION__, id(), context32_);

        if(auto ptr = remote_.lock()) {
            co_await ptr->syncReaderTimerStart(id(), context64_);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_return false;
        }

        // send all readers
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
        co_await async_send_buf(asio::buffer(PcscLite::readers.data(), readersLength));

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStop(void) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context32: {:#08x}",
                           __FUNCTION__, id(), context32_);

        if(auto ptr = remote_.lock()) {
            ptr->syncReaderTimerStop();
        }

        const uint32_t timeout = 0;
        const uint32_t ret = SCARD_S_SUCCESS;

        co_await async_send_le32(timeout);
        co_await async_send_le32(ret);

        co_return true;
    }

    /// PcscSessionBus
    PcscSessionBus::PcscSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_pcsc"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_pcsc_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_pcsc_path),
#endif
        signals_ {ioc_}, dbus_conn_ {std::move(conn)} {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    PcscSessionBus::~PcscSessionBus() {
        unregisterAdaptor();
        stop();
    }

    void PcscSessionBus::stop(void) {
        listen_stop_.emit(asio::cancellation_type::terminal);
        signals_.cancel();
        dbus_conn_->leaveEventLoop();
    }

    int PcscSessionBus::start(void) {
        Application::info("{}: uid: {}, pid: {}, version: {}", __FUNCTION__, getuid(), getpid(), LTSM_SESSION_PCSC_VERSION);

        auto pcsc_path = getenv("PCSCLITE_CSOCK_NAME");

        if(! pcsc_path) {
            Application::error("{}: environment not found: {}", __FUNCTION__, "PCSCLITE_CSOCK_NAME");
            return EXIT_FAILURE;
        }

        Application::info("{}: socket path: `{}'", __FUNCTION__, pcsc_path);

        pcsc_ep_.path(pcsc_path);
        remote_ = std::make_shared<PcscRemote>(asio::local::stream_protocol::socket{ioc_});

        signals_.add(SIGTERM);
        signals_.add(SIGINT);

        signals_.async_wait([this](const system::error_code & ec, int signal) {
            // skip canceled
            if(ec != asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        auto sdbus_job = std::async(std::launch::async, [this]() {
            try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
            }
        });

        // main loop
        ioc_.run();

        Application::notice("{}: PCSC session shutdown", __FUNCTION__);

        dbus_conn_->leaveEventLoop();
        sdbus_job.wait();

        std::filesystem::remove(pcsc_path);

        return EXIT_SUCCESS;
    }

    int32_t PcscSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return LTSM_SESSION_PCSC_VERSION;
    }

    void PcscSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
    }

    void PcscSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    asio::awaitable<void> PcscSessionBus::handlerStopClient(uint64_t ctx) {

        asio::co_spawn(clients_guard_, [this, ctx]() -> asio::awaitable<void> {
            auto it = std::find_if(clients_.begin(), clients_.end(), [ = ](auto & cli) {
                return cli.proxyContext() == ctx;
            });

            if(it != clients_.end()) {
                Application::debug(DebugType::Dbus, "{}: stop remote: {:#016x}", "handlerStopClient", ctx);
                it->stopSignal();
            }
            co_return;
        }, asio::detached);

        co_return;
    }

    asio::awaitable<void> PcscSessionBus::handlerLocalAccept(PcscLocal & client) {

        Application::debug(DebugType::App, "{}: clientId: {}", __FUNCTION__, client.id());
        bool success = true;

        while(success) {
            try {
                success = co_await client.handlerClientWaitCommand();
            } catch(const system::system_error& err) {
                auto ec = err.code();

                if(ec != asio::error::eof && ec != asio::error::operation_aborted) {
                    Application::error("{}: {} failed, code: {}, error: {}",
                                       __FUNCTION__, "handlerClientWaitCommand", ec.value(), ec.message());
                }

                success = false;
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", __FUNCTION__, err.what());
                success = false;
            }

            if(remote_->isError()) {
                asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
                success = false;
            }
        }

        co_return;
    }

    void PcscSessionBus::handlerLocalStopped(const PcscLocal* client, std::exception_ptr eptr) {
        auto it = std::find_if(clients_.begin(), clients_.end(), [&](auto & cli) {
            return client == std::addressof(cli);
        });

        if(it != clients_.end()) {
            Application::debug(DebugType::Dbus, "{}: clientId: {}, destroy", __FUNCTION__, it->id());

            trans_lock.unlock(it->id());
            clients_.erase(it);
        }
    }

    asio::awaitable<void> PcscSessionBus::handlerLocalListener(void) {
        try {
            auto executor = co_await asio::this_coro::executor;
            asio::local::stream_protocol::acceptor acceptor(executor, pcsc_ep_);
            int client_id = 11;

            for(;;) {
                asio::local::stream_protocol::socket peer = co_await acceptor.async_accept(asio::use_awaitable);

                clients_.emplace_back(std::move(peer), client_id++, remote_, this);
                auto & client = clients_.back();

                auto token = asio::bind_cancellation_slot(client.stopSlot(),
                             std::bind(&PcscSessionBus::handlerLocalStopped, this, &client, std::placeholders::_1));

                asio::co_spawn(executor, handlerLocalAccept(client), std::move(token));
            }
        } catch(const system::system_error& err) {
            auto ec = err.code();

            if(ec != asio::error::operation_aborted) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "handlerLocalAccept", ec.value(), ec.message());
            }
        }

        for(auto & cli : clients_) {
            cli.stopSignal();
        }
    }

    bool PcscSessionBus::connectChannel(const std::string & path) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", __FUNCTION__, path);

        if(remote_->isConnected()) {
            return false;
        }

        auto wait = asio::co_spawn(ioc_.get_executor(), remote_->handlerWaitConnect(path), asio::use_future);
        bool connected = false;

        try {
            connected = wait.get();
        } catch(const std::exception & ex) {
            Application::error("{}: exception: {}", __FUNCTION__, ex.what());
            return false;
        }

        if(connected) {
            if(std::filesystem::is_socket(pcsc_ep_.path())) {
                std::filesystem::remove(pcsc_ep_.path());
                Application::warning("{}: old socket removed", __FUNCTION__);
            }

            asio::co_spawn(clients_guard_, handlerLocalListener(),
                           asio::bind_cancellation_slot(listen_stop_.slot(), asio::detached));

            return true;
        }

        return false;
    }

    void PcscSessionBus::disconnectChannel(const std::string & clientPath) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", __FUNCTION__, clientPath);

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
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [{}] {}", err.getName(), err.getMessage());
    } catch(const std::exception & err) {
        LTSM::Application::error("{}: exception: {}", NS_FuncNameV, err.what());
    }

    return EXIT_FAILURE;
}
