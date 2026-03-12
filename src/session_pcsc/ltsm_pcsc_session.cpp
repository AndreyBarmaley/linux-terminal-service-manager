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

#include <signal.h>
#include <sys/socket.h>

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
    std::mutex readersLock;

    ReaderState* findReaderState(const std::string & name) {
        std::scoped_lock guard{ readersLock };
        auto it = std::ranges::find_if(readers, [&](auto & rd) {
            return 0 == name.compare(rd.name);
        });

        return it != readers.end() ? std::addressof(*it) : nullptr;
    }

    bool readersReset(void) {
        std::scoped_lock guard{ readersLock };
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
        std::scoped_lock guard{ readersLock };
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
    std::mutex trans_lock;
    std::atomic<int32_t> transaction_id{0};

    /// PcscRemote
    PcscRemote::PcscRemote(asio::io_context& ctx, const std::string & path, std::promise<bool> connected)
        : sock_{ctx} {

        sock_.async_connect(path, [res = std::move(connected)](const system::error_code & ec) mutable {
            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", "handlerRemoteConnected", "connect", ec.value(), ec.message());
                res.set_value(false);
            } else {
                res.set_value(true);
            }
        });
    }

    void PcscRemote::wait_async_send(asio::streambuf & sb) {
        auto send = asio::async_write(sock_, sb, asio::transfer_all(), asio::use_future);

        try {
            [[maybe_unused]] auto bytes = send.get();
        } catch(const system::error_code & ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            error_ = true;
            throw pcsc_error(NS_FuncNameS);
        }
    }

    std::tuple<uint64_t, uint32_t>
    PcscRemote::sendEstablishedContext(const int32_t & id, const uint32_t & scope) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << scope: {}", __FUNCTION__, id, scope);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).write_le16(PcscLite::EstablishContext).write_le32(scope);
        wait_async_send(sb);

        // rsz: context64 + ret32
        const size_t rsz = sizeof(uint64_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto context = bs.read_le64();
        auto ret = bs.read_le32();
        return std::make_tuple(context, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendReleaseContext(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteContext: {:#016x}",
                           __FUNCTION__, id, context);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::ReleaseContext).write_le64(context);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::tuple<uint64_t, uint32_t, uint32_t>
    PcscRemote::sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const std::string & readerName) {
        trans_lock.lock();
        transaction_id = id;

        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteContext: {:#016x}, shareMode: {}, prefferedProtocols: {}, reader: `{}'",
                           __FUNCTION__, id, context, shareMode, prefferedProtocols, readerName);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Connect).
          write_le64(context).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(readerName.size()).write_string(readerName);

        wait_async_send(sb);

        // rsz: handle64 + proto32 + ret32
        const size_t rsz = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto handle = bs.read_le64();
        auto activeProtocol = bs.read_le32();
        auto ret = bs.read_le32();

        transaction_id = 0;
        trans_lock.unlock();

        return std::make_tuple(handle, activeProtocol, ret);
    }

    std::tuple<uint32_t, uint32_t>
    PcscRemote::sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}",
                           __FUNCTION__, id, handle, shareMode, prefferedProtocols, initialization);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Reconnect).
          write_le64(handle).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(initialization);

        wait_async_send(sb);

        // rsz: proto32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto activeProtocol = bs.read_le32();
        auto ret = bs.read_le32();

        return std::make_tuple(activeProtocol, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, disposition: {}",
                           __FUNCTION__, id, handle, disposition);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Disconnect).
          write_le64(handle).
          write_le32(disposition);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendBeginTransaction(const int32_t & id, const uint64_t & handle) {
        trans_lock.lock();
        transaction_id = id;

        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}",
                           __FUNCTION__, id, handle);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::BeginTransaction).
          write_le64(handle);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();

        if(ret != SCARD_S_SUCCESS) {
            transaction_id = 0;
            trans_lock.unlock();
        }

        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, disposition: {}",
                           __FUNCTION__, id, handle, disposition);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::EndTransaction).
          write_le64(handle).
          write_le32(disposition);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();

        transaction_id = 0;
        trans_lock.unlock();

        return std::make_tuple(ret);
    }

    std::tuple<uint32_t, uint32_t, uint32_t, binary_buf>
    PcscRemote::sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data1) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, pciProtocol: {:#08x}, pciLength: {}, send size: {}, recv size: {}",
                           __FUNCTION__, id, handle, ioSendPciProtocol, ioSendPciLength, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", __FUNCTION__, str);
        }

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Transmit).
          write_le64(handle).
          write_le32(ioSendPciProtocol).
          write_le32(ioSendPciLength).
          write_le32(recvLength).
          write_le32(data1.size()).write_bytes(data1);

        wait_async_send(sb);

        // rsz: proto32 + length32 + bytes32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ioRecvPciProtocol = bs.read_le32();
        auto ioRecvPciLength = bs.read_le32();
        auto bytesReturned = bs.read_le32();
        auto ret = bs.read_le32();
        auto data2 = async_recv_buffer<binary_buf>(bytesReturned);

        return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, std::move(data2));
    }

    inline void fixed_string_name(std::string & str) {
        while(str.size() && str.back() == 0) {
            str.pop_back();
        }
    }

    std::tuple<std::string, uint32_t, uint32_t, uint32_t, binary_buf>
    PcscRemote::sendStatus(const int32_t & id, const uint64_t & handle) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}",
                           __FUNCTION__, id, handle);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Status).
          write_le64(handle);

        wait_async_send(sb);

        // rsz: name32
        size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto nameLen = bs.read_le32();
        auto name = async_recv_buffer<std::string>(nameLen);
        fixed_string_name(name);
    
        // rsz: state32 + proto32 + atr32
        rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto state = bs.read_le32();
        auto protocol = bs.read_le32();
        auto atrLen = bs.read_le32();
        auto atr = async_recv_buffer<binary_buf>(atrLen);

        // rsz: ret32
        rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);
        auto ret = bs.read_le32();

        return std::make_tuple(std::move(name), state, protocol, ret, std::move(atr));
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, controlCode: {:#08x}, send size: {}, recv size: {}",
                           __FUNCTION__, id, handle, controlCode, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [{}]", __FUNCTION__, str);
        }

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Control).
          write_le64(handle).
          write_le32(controlCode).
          write_le32(data1.size()).
          write_le32(recvLength).
          write_bytes(data1);

        wait_async_send(sb);

        // rsz: bytes32 + len32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto bytesReturned = bs.read_le32();
        auto ret = bs.read_le32();
        auto data2 = async_recv_buffer<binary_buf>(bytesReturned);

        return std::make_tuple(ret, std::move(data2));
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, attrId: {}",
                           __FUNCTION__, id, handle, attrId);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::GetAttrib).
          write_le64(handle).
          write_le32(attrId);

        wait_async_send(sb);

        // rsz: len32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto attrLen = bs.read_le32();
        auto ret = bs.read_le32();

        assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
        auto attr = async_recv_buffer<binary_buf>(attrLen);

        return std::make_tuple(ret, std::move(attr));
    }

    std::tuple<uint32_t>
    PcscRemote::sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle {:#016x}, attrId: {}, attrLength {}",
                           __FUNCTION__, id, handle, attrId, attr.size());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(attr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: attr: [{}]", __FUNCTION__, str);
        }

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::SetAttrib).
          write_le64(handle).
          write_le32(attrId).
          write_le32(attr.size()).
          write_bytes(attr);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendCancel(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteContext {:#016x}",
                           __FUNCTION__, id, context);

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Cancel).
          write_le64(context);

        wait_async_send(sb);

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::list<std::string> PcscRemote::sendListReaders(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::ListReaders).
          write_le64(context);

        wait_async_send(sb);

        // rsz: count32
        const size_t rsz = sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto readersCount = bs.read_le32();
        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localContext: {:#08x}, readers count: {}",
                           __FUNCTION__, id, context, readersCount);

        std::list<std::string> names;

        while(readersCount--) {
            // rsz: len32
            const size_t rsz = sizeof(uint32_t);
            wait_async_recv(sb, rsz);

            auto len = bs.read_le32();
            auto name = async_recv_buffer<std::string>(len);
            names.emplace_back(std::move(name));

            if(names.back().size() > MAX_READERNAME - 1) {
                names.back().resize(MAX_READERNAME - 1);
            }
        }

        return names;
    }

    uint32_t PcscRemote::sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount) {
        const std::scoped_lock guard{ sock_lock_ };

        asio::streambuf sb;
        byte::streambuf bs(sb);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::GetStatusChange).
          write_le64(context).
          write_le32(timeout).
          write_le32(statesCount);

        for(uint32_t it = 0; it < statesCount; ++it) {
            const SCARD_READERSTATE & state = states[it];
            bs.write_le32(strnlen(state.szReader, MAX_READERNAME)).
              write_le32(state.dwCurrentState).
              write_le32(state.cbAtr).
              write_string(state.szReader).
              write_bytes(state.rgbAtr, state.cbAtr);
        }

        wait_async_send(sb);

        // rsz: count32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        wait_async_recv(sb, rsz);

        auto counts = bs.read_le32();
        auto ret = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> remoteContext: {:#016x}, timeout: {}, states: {}",
                           __FUNCTION__, id, context, timeout, counts);

        assertm(counts == statesCount, "count states invalid");

        for(uint32_t it = 0; it < statesCount; ++it) {
            // rsz: state32 + state32 + name32 + atr32
            const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
            wait_async_recv(sb, rsz);

            SCARD_READERSTATE & state = states[it];

            state.dwCurrentState = bs.read_le32();
            state.dwEventState = bs.read_le32();

            auto szReader = bs.read_le32();
            auto cbAtr = bs.read_le32();

            std::string reader = async_recv_buffer<std::string>(szReader);
            if(reader != state.szReader) {
                Application::warning("{}: invalid reader, `{}' != `'", __FUNCTION__, reader, state.szReader);
            }

            assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

            state.cbAtr = cbAtr;
            auto wrapper = asio::buffer(state.rgbAtr, cbAtr);
            wait_async_recv(wrapper, cbAtr);
        }

        return ret;
    }

    static int start_client_id = 11;

    /// PcscLocal
    PcscLocal::PcscLocal(asio::local::stream_protocol::socket && sock, int cid, std::shared_ptr<PcscRemote> ptr, PcscSessionBus* sessionBus)
        : sock_{std::move(sock)}, cid_{cid}, remote_{ptr} {
        clientCanceledCb = std::bind(&PcscSessionBus::clientCanceledNotify, sessionBus, std::placeholders::_1);
    }

    PcscLocal::~PcscLocal() {
        sock_.cancel();
        sock_.close();

        if(timer_status_) {
            timer_status_->cancel();
        }

        if(transaction_id == id()) {
            transaction_id = 0;
            trans_lock.unlock();
        }
    }

    asio::awaitable<bool> PcscLocal::handlerClientWaitCommand(void) {
        bool status = false;

        try {
            // begin data: len32, cmd32
            uint32_t len = 0;
            co_await asio::async_read(sock_, asio::buffer(&len, sizeof(len)), asio::transfer_exactly(sizeof(len)), asio::use_awaitable);
            endian::little_to_native_inplace(len);

            uint32_t cmd = 0;
            co_await asio::async_read(sock_, asio::buffer(&cmd, sizeof(cmd)), asio::transfer_exactly(sizeof(len)), asio::use_awaitable);
            endian::little_to_native_inplace(cmd);

            status = co_await clientAction(cmd, len);
        } catch(const std::exception & ex) {
            Application::error("{}: client id: {}, context: {:#08x}, exception: {}",
                               __FUNCTION__, id(), context, ex.what());
        }

        if(! status) {
            if(transaction_id == id()) {
                transaction_id = 0;
                trans_lock.unlock();
            }
        }

        co_return status;
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
            co_await asio::async_write(sock_, asio::buffer(buf.data(), buf.size()), asio::use_awaitable);
        }

        endian::native_to_little_inplace(err);
        co_await asio::async_write(sock_, asio::buffer(&err, sizeof(err)), asio::transfer_exactly(sizeof(err)), asio::use_awaitable);
    }

    asio::awaitable<bool> PcscLocal::proxyEstablishContext(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: scope32, context32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t scope = bs.read_le32();
        // skip: context, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            std::tie(remoteContext, ret) = ptr->sendEstablishedContext(id(), scope);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::EstablishContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make 32bit context
            context = Tools::crc32b((const uint8_t*) & remoteContext, sizeof(remoteContext));
            context &= 0x7FFFFFFF;

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> remoteContext: {:#016x}, localContext: {:#08x}",
                               __FUNCTION__, id(), remoteContext, context);

            // init readers status
            co_await syncReaders();
        } else {
            Application::error("{}: clientId: {}, error: {:#08x} ({})",
                               __FUNCTION__, id(), ret, PcscLite::err2str(ret));
        }

        bs.write_le32(scope).
          write_le32(context).write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReleaseContext(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: context32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t ctx = bs.read_le32();
        // skip: ret
        bs.skip_bytes(sizeof(uint32_t));

        if(! ctx || ctx != context) {
            Application::error("{}: clientId: {}, invalid localContext: {:#08x}", __FUNCTION__, id(), ctx);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::ReleaseContext, SCARD_E_NO_SERVICE);
            co_return false;
        }

        uint32_t ret = SCARD_S_SUCCESS;

        if(remoteContext) {
            std::tie(ret) = ptr->sendReleaseContext(id(), remoteContext);

            if(ret != SCARD_S_SUCCESS) {
                Application::error("{}: clientId: {}, context: {:#08x}, error: {:#08x} ({})",
                                   __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
            }
        }

        bs.write_le32(context).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        context = 0;
        remoteContext = 0;

        // set shutdown
        co_return false;
    }

    asio::awaitable<bool> PcscLocal::proxyConnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: context32, MAX_READERNAME, share32, proto32, handl32, proto32, ret32
        const size_t rsz = sizeof(uint32_t) + MAX_READERNAME + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t ctx = bs.read_le32();
        const auto readerData = bs.read_bytes(MAX_READERNAME);
        const uint32_t shareMode = bs.read_le32();
        const uint32_t prefferedProtocols = bs.read_le32();
        // skip: handle, activeProtocol, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t));

        if(! ctx || ctx != context) {
            Application::error("{}: clientId: {}, invalid localContext: {:#08x}", __FUNCTION__, id(), ctx);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        auto readerName = std::string(readerData.begin(),
                                      std::ranges::find(readerData, 0));
        auto currentReader = PcscLite::findReaderState(readerName);

        if(! currentReader) {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
            co_return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteContext) {
                Application::error("{}: clientId: {}, invalid remoteContext", __FUNCTION__, id());
                co_await replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(remoteHandle, activeProtocol, ret) = ptr->sendConnect(id(), remoteContext, shareMode, prefferedProtocols, readerName);
        } else {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            co_await replyError(PcscLite::Connect, SCARD_E_INVALID_VALUE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make localHandle
            handle = ret != SCARD_S_SUCCESS ? 0 : Tools::crc32b((const uint8_t*) & remoteHandle, sizeof(remoteHandle));
            handle &= 0x7FFFFFFF;

            // sync reader
            reader_ = currentReader;

            if(reader_) {
                std::scoped_lock guard{ PcscLite::readersLock };
                reader_->share = shareMode;
                reader_->protocol = activeProtocol;
            }

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> remoteHandle: {:#016x}, localHandle: {:#08x}, activeProtocol: {}",
                               __FUNCTION__, id(), remoteHandle, handle, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, context: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(context).
          write_bytes(readerData).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(handle).
          write_le32(activeProtocol).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyReconnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, share32, proto32, init32, proto32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t shareMode = bs.read_le32();
        const uint32_t prefferedProtocols = bs.read_le32();
        const uint32_t initialization = bs.read_le32();
        // skip activeProtocol, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::Reconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::Reconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(activeProtocol, ret) = ptr->sendReconnect(id(), remoteHandle, shareMode, prefferedProtocols, initialization);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Reconnect, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            assertm(reader_, "reader not connected");
            std::scoped_lock guard{ PcscLite::readersLock };
            reader_->share = shareMode;
            reader_->protocol = activeProtocol;
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, shareMode: {}, prefferedProtocols: {}, inititalization: {}, activeProtocol: {}",
                               __FUNCTION__, id(), handle, shareMode, prefferedProtocols, initialization, activeProtocol);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(initialization).
          write_le32(activeProtocol).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyDisconnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, disp32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t disposition = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::Disconnect, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::Disconnect, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = ptr->sendDisconnect(id(), remoteHandle, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Disconnect, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // sync after
            handle = 0;
            remoteHandle = 0;
            assertm(reader_, "reader not connected");

            std::scoped_lock guard{ PcscLite::readersLock };
            reader_->share = 0;
            reader_->protocol = 0;
            reader_ = nullptr;

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, disposition: {}",
                               __FUNCTION__, id(), handle, disposition);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(disposition).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyBeginTransaction(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::BeginTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            assertm(reader_, "reader not connected");
            std::tie(ret) = ptr->sendBeginTransaction(id(), remoteHandle);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::BeginTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}",
                               __FUNCTION__, id(), handle);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyEndTransaction(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, disp32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t disposition = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::EndTransaction, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = ptr->sendEndTransaction(id(), remoteHandle, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::EndTransaction, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, disposition: {}",
                               __FUNCTION__, id(), handle, disposition);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(disposition).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyTransmit(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, sproto32, sprotolen32, slen32, rproto32, rprotolen32, rlen32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t ioSendPciProtocol = bs.read_le32();
        const uint32_t ioSendPciLength = bs.read_le32();
        const uint32_t sendLength = bs.read_le32();
        // skip ioRecvPciProtocol, ioRecvPciLength
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));
        const uint32_t recvLength = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        binary_buf data1;

        if(sendLength) {
            co_await asio::async_read(sock_, sb_, asio::transfer_exactly(sendLength), asio::use_awaitable);
            data1 = bs.read_bytes(sendLength);
        }

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::Transmit, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(sendLength != data1.size()) {
            Application::error("{}: clientId: {}, invalid length, send: {}, data: {}", __FUNCTION__, id(),
                               sendLength, data1.size());
            co_return false;
        }

        uint32_t ioRecvPciProtocol, ioRecvPciLength, ret;
        binary_buf data2;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::Transmit, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ioRecvPciProtocol, ioRecvPciLength, ret, data2) = ptr->sendTransmit(id(), remoteHandle, ioSendPciProtocol, ioSendPciLength, recvLength, data1);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Transmit, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, pciProtocol: {:#08x}, pciLength: {}, recv size: {}",
                               __FUNCTION__, id(), handle, ioRecvPciProtocol, ioRecvPciLength, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(ioSendPciProtocol).
          write_le32(ioSendPciLength).
          write_le32(sendLength).
          write_le32(ioRecvPciProtocol).
          write_le32(ioRecvPciLength).
          write_le32(data2.size()).
          write_le32(ret);

        if(data2.size()) {
            bs.write_bytes(data2);
        }

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    void PcscLocal::statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, reader: `{}', state: {:#08x}, protocol: {}, atrLen: {}",
                           __FUNCTION__, id(), name, state, protocol, atr.size());

        assertm(reader_, "reader not connected");
        assertm(atr.size() <= sizeof(reader_->atr), "atr length invalid");
        std::scoped_lock guard{ PcscLite::readersLock };

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
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::Status, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        std::string name;
        uint32_t state, protocol, ret;
        binary_buf atr;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(name, state, protocol, ret, atr) = ptr->sendStatus(id(), remoteHandle);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}",
                               __FUNCTION__, id(), handle);

            statusApply(name, state, protocol, atr);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyControl(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, code32, slen32, rlen32, bytes32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t controlCode = bs.read_le32();
        const uint32_t sendLength = bs.read_le32();
        const uint32_t recvLength = bs.read_le32();
        // skip bytesReturned, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        binary_buf data1;

        if(sendLength) {
            co_await asio::async_read(sock_, sb_, asio::transfer_exactly(sendLength), asio::use_awaitable);
            data1 = bs.read_bytes(sendLength);
        }

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::Control, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        if(sendLength != data1.size()) {
            Application::error("{}: clientId: {}, invalid length, send: {}, data: {}", __FUNCTION__, id(),
                               sendLength, data1.size());
            co_return false;
        }

        uint32_t ret;
        binary_buf data2;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::Control, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret, data2) = ptr->sendControl(id(), remoteHandle, controlCode, recvLength, data1);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::Control, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, controlCode: {:#08x}, bytesReturned: {}",
                               __FUNCTION__, id(), handle, controlCode, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(handle).
          write_le32(controlCode).
          write_le32(sendLength).
          write_le32(recvLength).
          write_le32(data2.size()).
          write_le32(ret);

        if(data2.size()) {
            bs.write_bytes(data2);
        }

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyGetAttrib(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, attr32, MAX_BUFFER_SIZE, len32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t attrId = bs.read_le32();
        // skip attr[MAX_BUFFER_SIZE], attrLen, ret
        bs.skip_bytes(MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::GetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t ret;
        binary_buf attr;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::GetAttrib, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret, attr) = ptr->sendGetAttrib(id(), remoteHandle, attrId);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::GetAttrib, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, attrId: {}, attrLen: {}",
                               __FUNCTION__, id(), handle, attrId, attr.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(attr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: attr: [{}]", __FUNCTION__, str);
            }
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        if(attr.size() != MAX_BUFFER_SIZE) {
            attr.resize(MAX_BUFFER_SIZE, 0);
        }

        bs.write_le32(handle).
          write_le32(attrId).
          write_bytes(attr).
          write_le32(attr.size()).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxySetAttrib(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, attr32, MAX_BUFFER_SIZE, len32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t hdl = bs.read_le32();
        const uint32_t attrId = bs.read_le32();
        auto attr = bs.read_bytes(MAX_BUFFER_SIZE);
        const uint32_t attrLen = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        // fixed attr
        if(attrLen < attr.size()) {
            attr.resize(attrLen);
        }

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            co_await replyError(PcscLite::SetAttrib, SCARD_E_INVALID_HANDLE);
            co_return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                co_await replyError(PcscLite::SetAttrib, SCARD_F_INTERNAL_ERROR);
                co_return false;
            }

            std::tie(ret) = ptr->sendSetAttrib(id(), remoteHandle, attrId, attr);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_await replyError(PcscLite::SetAttrib, SCARD_E_NO_SERVICE);
            co_return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle {:#08x}",
                               __FUNCTION__, id(), handle);
        } else {
            Application::error("{}: clientId: {}, handle: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        if(attr.size() != MAX_BUFFER_SIZE) {
            attr.resize(MAX_BUFFER_SIZE, 0);
        }

        bs.write_le32(handle).
          write_le32(attrId).
          write_bytes(attr).
          write_le32(attr.size()).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return ret == SCARD_S_SUCCESS;
    }

    asio::awaitable<bool> PcscLocal::proxyCancel(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: ctx32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t ctx = bs.read_le32();
        uint32_t ret = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context: {:#08x}",
                           __FUNCTION__, id(), ctx);

        if(auto remoteContext2 = clientCanceledCb(ctx)) {
            if(auto ptr = remote_.lock()) {
                std::tie(ret) = ptr->sendCancel(id(), remoteContext2);
            } else {
                Application::error("{}: no service", __FUNCTION__);
                co_await replyError(PcscLite::Cancel, SCARD_E_NO_SERVICE);
                co_return false;
            }
        } else {
            Application::error("{}: clientId: {:#08x}, canceled not found, context: {:#08x}",
                               __FUNCTION__, id(), ctx);
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localContext {:#08x}",
                               __FUNCTION__, id(), context);
        } else {
            Application::error("{}: clientId: {}, context: {:#08x}, error: {:#08x} ({})",
                               __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
        }

        bs.write_le32(ctx).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetVersion(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: ver32, ver32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

        co_await asio::async_read(sock_, sb_, asio::transfer_exactly(rsz), asio::use_awaitable);

        const uint32_t versionMajor = bs.read_le32();
        const uint32_t versionMinor = bs.read_le32();
        // skip ret32
        bs.skip_bytes(sizeof(uint32_t));

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> protocol version: {}.{}",
                           __FUNCTION__, id(), versionMajor, versionMinor);

        uint32_t ret = SCARD_S_SUCCESS;

        // supported only 4.4 protocol or higher
        if(versionMajor * 10 + versionMinor < 44) {
            Application::warning("{}: clientId: {}, unsupported version: version: {}.{}",
                           __FUNCTION__, id(), versionMajor, versionMinor);
            ret = SCARD_E_NO_SERVICE;
        }

        bs.write_le32(versionMajor).
          write_le32(versionMinor).
          write_le32(ret);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyGetReaderState(void) {
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localContext: {:#08x}, readers length: {}",
                           __FUNCTION__, id(), context, readersLength);

        std::scoped_lock guard{ PcscLite::readersLock };
        co_await asio::async_write(sock_, asio::buffer(PcscLite::readers.data(), readersLength), asio::transfer_all(), asio::use_awaitable);

        co_return true;
    }

    asio::awaitable<std::pair<bool,uint32_t>> PcscLocal::readersStatusChanged(const boost::system::error_code & ec, int32_t timeout) {

        uint32_t ret = SCARD_S_SUCCESS;

        if(ec) {
            if(ec.value() == boost::system::errc::operation_canceled) {
                ret = SCARD_E_CANCELLED;
                if(status_cancel_) {
                    Application::debug(DebugType::Pcsc, "{}: clientId: {}, {}", __FUNCTION__, id(), "canceled");
                } else {
                    Application::debug(DebugType::Pcsc, "{}: clientId: {}, {}", __FUNCTION__, id(), "stopped");
                }
            } else {
                ret = SCARD_F_INTERNAL_ERROR;
                Application::warning("{}: {} failed, code: {}, error: {}", __FUNCTION__, "timer", ec.value(), ec.message());
            }
            co_return std::make_pair(false, ret);
        }

        if(timeout < 0) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {}, {}", __FUNCTION__, id(), "timeout");
            ret = SCARD_E_TIMEOUT;
            co_return std::make_pair(false, ret);
        }

        Application::trace(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}, continue: {}",
                           __FUNCTION__, id(), context, timeout);

        bool readersChanged = false;
        auto ret2 = co_await syncReaders(& readersChanged);

        if(readersChanged) {
            ret = ret2;
        }

        co_return std::make_pair(true, ret);
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStart(void) {

        // new protocol 4.4: empty params
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}, timeout: {}",
                               __FUNCTION__, id(), context);

        co_await syncReaders();

        // run timer
        status_cancel_ = false;
        timer_ret_ = SCARD_S_SUCCESS;

        uint32_t timeout = INT32_MAX;
        const size_t pause_ms = 750;
        timer_status_ = std::make_unique<boost::asio::steady_timer>(sock_.get_executor(), std::chrono::milliseconds(pause_ms));

        while(true) {
            auto [ec] = co_await timer_status_->async_wait(asio::as_tuple(asio::use_awaitable));
            auto [cont, ret] = co_await readersStatusChanged(ec, timeout);
            if(! cont) {
                break;
            }
            timeout -= pause_ms;
        }
    
        // send all readers
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
        std::scoped_lock guard{ PcscLite::readersLock };

        co_await boost::asio::async_write(sock_, boost::asio::buffer(PcscLite::readers.data(), readersLength), boost::asio::transfer_all(), asio::use_awaitable);

        co_return true;
    }

    asio::awaitable<bool> PcscLocal::proxyReaderStateChangeStop(void) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}",
                           __FUNCTION__, id(), context);

        // stop
        status_cancel_ = false;
        timer_status_.reset();

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le32(0).
          write_le32(SCARD_S_SUCCESS);

        co_await asio::async_write(sock_, sb_, asio::transfer_all(), asio::use_awaitable);

        co_return true;
    }

    asio::awaitable<uint32_t> PcscLocal::syncReaderStatus(const std::string & readerName, PcscLite::ReaderState & rd, bool* changed) {
        const uint32_t timeout = 0;
        SCARD_READERSTATE state = {};

        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;
        state.cbAtr = MAX_ATR_SIZE;

        uint32_t ret = SCARD_S_SUCCESS;

        if(auto ptr = remote_.lock()) {
            ret = ptr->sendGetStatusChange(id(), remoteContext, timeout, & state, 1);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_return static_cast<uint32_t>(SCARD_E_NO_SERVICE);
        }

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

    asio::awaitable<uint32_t> PcscLocal::syncReaders(bool* changed) {
        std::list<std::string> names;

        if(auto ptr = remote_.lock()) {
            names = ptr->sendListReaders(id(), remoteContext);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            co_return static_cast<uint32_t>(SCARD_E_NO_SERVICE);
        }

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

        std::scoped_lock guard{ PcscLite::readersLock };

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
                co_await syncReaderStatus(name, *rd, changed);
            }
        }

        co_return static_cast<uint32_t>(SCARD_S_SUCCESS);
    }

    void PcscLocal::canceledAction(void) {
        status_cancel_ = true;
        timer_status_.reset();
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
        listen_stop_.emit(asio::cancellation_type::all); 
        clients_.clear();
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
        trans_lock.unlock();

        dbus_conn_->leaveEventLoop();
        sdbus_job.wait();

        std::filesystem::remove(pcsc_path);

        return EXIT_SUCCESS;
    }

    uint64_t PcscSessionBus::clientCanceledNotify(uint32_t ctx) {
        const std::scoped_lock guard{ clients_lock_ };
        auto it = std::ranges::find_if(clients_, [&ctx](auto & cl) {
            return 0 <= cl.id() && cl.localContext() == ctx;
        });

        if(it != clients_.end()) {
            Application::debug(DebugType::App, "{}: calceled clientId: {}", __FUNCTION__, it->id());
            it->canceledAction();
            return it->proxyContext();
        }

        return 0;
    }

    void PcscSessionBus::clientShutdownNotify(const PcscLocal* cli) {
        Application::debug(DebugType::App, "{}: shutdown clientId: {}", __FUNCTION__, cli->id());

        const std::scoped_lock guard{ clients_lock_ };

        std::erase_if(clients_, [cli](auto & st) {
            return cli == std::addressof(st);
        });

        if(remote_->isError()) {
            asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
        }
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

/*
    void PcscSessionBus::handlerLocalAccepted(const system::error_code & ec, asio::local::stream_protocol::socket peer) {
        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "accept", ec.value(), ec.message());
            return;
        }

        const std::scoped_lock guard{ clients_lock_ };
        int cid = start_client_id++;

        Application::debug(DebugType::App, "{}: add clientId: {}, handler: {}", __FUNCTION__, cid, peer.native_handle());
        this->clients_.emplace_front(std::move(peer), cid, remote_, this);

        // next accept
        pcsc_sock_.async_accept(std::bind(&PcscSessionBus::handlerLocalAccepted, this, std::placeholders::_1, std::placeholders::_2));
    }
*/

    asio::awaitable<void> PcscSessionBus::handlerLocalAccept(asio::local::stream_protocol::socket peer) {
        // FIXME atomic?
        int cid = start_client_id++;

        Application::debug(DebugType::App, "{}: clientId: {}, handler: {}", __FUNCTION__, cid, peer.native_handle());
        PcscLocal pcscLocal(std::move(peer), cid, remote_, this);
        bool success = true;

        while(success) {
            success = co_await pcscLocal.handlerClientWaitCommand();
        }

        // FIXME remove func
        //clientShutdownNotify(&pcscLocal);

        if(remote_->isError()) {
            auto executor = co_await asio::this_coro::executor;
            asio::post(executor, std::bind(&PcscSessionBus::stop, this));
        }
        
        co_return;
    }

    asio::awaitable<void> PcscSessionBus::handlerLocalListener(void) {
        auto executor = co_await asio::this_coro::executor;
        asio::local::stream_protocol::acceptor acceptor(executor, pcsc_ep_);

        for (;;) {
            asio::local::stream_protocol::socket sock = co_await acceptor.async_accept(asio::use_awaitable);
            asio::co_spawn(executor, handlerLocalAccept(std::move(sock)), asio::detached);
        }
    }

    bool PcscSessionBus::connectChannel(const std::string & path) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", __FUNCTION__, path);

        std::promise<bool> connected;
        auto res = connected.get_future();
        remote_ = std::make_shared<PcscRemote>(ioc_, path, std::move(connected));

        if(res.get()) {
            if(std::filesystem::is_socket(pcsc_ep_.path())) {
                std::filesystem::remove(pcsc_ep_.path());
                Application::warning("{}: old socket removed", __FUNCTION__);
            }

            try {
                asio::co_spawn(ioc_, handlerLocalListener(),
                    asio::bind_cancellation_slot(listen_stop_.slot(), asio::detached));
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", __FUNCTION__, err.what());
                return false;
            }

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
