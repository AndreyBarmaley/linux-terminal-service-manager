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

namespace PcscLite {
    uint32_t apiVersion = 0;

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
                return 0;

            case WaitReaderStateChangeStart:
                return apiVersion < 43 ? 8 : 0;

            case WaitReaderStateChangeStop:
                return apiVersion < 43 ? 8 : 0;

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
    PcscRemote::PcscRemote(boost::asio::io_context& ctx, const std::string & path, std::promise<bool> connected)
        : ioc_{ctx}, sock_{ioc_} {

        sock_.async_connect(path, [res = std::move(connected)](const boost::system::error_code & ec) mutable {
            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "handlerRemoteConnected", "connect", ec.value(), ec.message());
                res.set_value(false);
            } else {
                res.set_value(true);
            }
        });
    }

    std::tuple<uint64_t, uint32_t>
    PcscRemote::sendEstablishedContext(const int32_t & id, const uint32_t & scope) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << scope: {}", __FUNCTION__, id, scope);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::EstablishContext).write_le32(scope);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: context64 + ret32
        const size_t rsz = sizeof(uint64_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto context = bs.read_le64();
        auto ret = bs.read_le32();

        return std::make_tuple(context, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendReleaseContext(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteContext: {:#016x}",
                           __FUNCTION__, id, context);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::ReleaseContext).write_le64(context);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

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

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Connect).
          write_le64(context).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(readerName.size()).write_string(readerName);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: handle64 + proto32 + ret32
        const size_t rsz = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

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

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Reconnect).
          write_le64(handle).
          write_le32(shareMode).
          write_le32(prefferedProtocols).
          write_le32(initialization);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: proto32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto activeProtocol = bs.read_le32();
        auto ret = bs.read_le32();

        return std::make_tuple(activeProtocol, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, disposition: {}",
                           __FUNCTION__, id, handle, disposition);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Disconnect).
          write_le64(handle).
          write_le32(disposition);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

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

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::BeginTransaction).
          write_le64(handle);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

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

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::EndTransaction).
          write_le64(handle).
          write_le64(disposition);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

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
            Application::debug(DebugType::Pcsc, "{}: send data: [ `{}' ]", __FUNCTION__, str);
        }

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Transmit).
          write_le64(handle).
          write_le32(ioSendPciProtocol).
          write_le32(ioSendPciLength).
          write_le32(recvLength).
          write_le32(data1.size());

        if(data1.size()) {
            bs.write_bytes(data1);
        }

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: proto32 + length32 + bytes32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto ioRecvPciProtocol = bs.read_le32();
        auto ioRecvPciLength = bs.read_le32();
        auto bytesReturned = bs.read_le32();
        auto ret = bs.read_le32();

        if(bytesReturned) {
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(bytesReturned), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            auto data2 = bs.read_bytes(bytesReturned);
            return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, std::move(data2));
        }

        return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, binary_buf{});
    }

    std::tuple<std::string, uint32_t, uint32_t, uint32_t, binary_buf>
    PcscRemote::sendStatus(const int32_t & id, const uint64_t & handle) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}",
                           __FUNCTION__, id, handle);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Status).
          write_le64(handle);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: str32
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(sizeof(uint32_t)), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto nameLen = bs.read_le32();

        // rsz: nameLen + state32 + proto32 + len32
        const size_t rsz = nameLen + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto name = bs.read_string(nameLen);
        auto state = bs.read_le32();
        auto protocol = bs.read_le32();
        auto atrLen = bs.read_le32();

        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(atrLen + sizeof(uint32_t)), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto atr = bs.read_bytes(atrLen);
        auto ret = bs.read_le32();

        return std::make_tuple(name, state, protocol, ret, atr);
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, controlCode: {:#08x}, send size: {}, recv size: {}",
                           __FUNCTION__, id, handle, controlCode, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(data1, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: send data: [ `{}' ]", __FUNCTION__, str);
        }

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Control).
          write_le64(handle).
          write_le32(controlCode).
          write_le32(data1.size()).
          write_le32(recvLength);

        if(data1.size()) {
            bs.write_bytes(data1);
        }

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: bytes32 + len32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto bytesReturned = bs.read_le32();
        auto ret = bs.read_le32();

        if(bytesReturned) {
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(bytesReturned), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            auto data2 = bs.read_bytes(bytesReturned);
            return std::make_tuple(ret, std::move(data2));
        }

        return std::make_tuple(ret, binary_buf{});
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle: {:#016x}, attrId: {}",
                           __FUNCTION__, id, handle, attrId);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::GetAttrib).
          write_le64(handle).
          write_le32(attrId);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: len32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto attrLen = bs.read_le32();
        auto ret = bs.read_le32();

        if(attrLen) {
            assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(attrLen), ec);
            auto attr = bs.read_bytes(attrLen);
            return std::make_tuple(ret, std::move(attr));
        }

        return std::make_tuple(ret, binary_buf{});
    }

    std::tuple<uint32_t>
    PcscRemote::sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteHandle {:#016x}, attrId: {}, attrLength {}",
                           __FUNCTION__, id, handle, attrId, attr.size());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::hexString(attr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: attr: [ `{}' ]", __FUNCTION__, str);
        }

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::SetAttrib).
          write_le64(handle).
          write_le32(attrId).
          write_le32(attr.size());

        if(attr.size()) {
            bs.write_bytes(attr);
        }

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendCancel(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << remoteContext {:#016x}",
                           __FUNCTION__, id, context);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::Cancel).
          write_le64(context);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: ret32
        const size_t rsz = sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto ret = bs.read_le32();
        return std::make_tuple(ret);
    }

    std::list<std::string> PcscRemote::sendListReaders(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sock_lock_ };

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le16(PcscOp::Init).
          write_le16(PcscLite::ListReaders).
          write_le64(context);

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: count32
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(sizeof(uint32_t)), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto readersCount = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localContext: {:#08x}, readers count: {}",
                           __FUNCTION__, id, context, readersCount);

        std::list<std::string> names;

        while(readersCount--) {
            // rsz: len32
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(sizeof(uint32_t)), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            uint32_t len = bs.read_le32();
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(len), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            names.emplace_back(bs.read_string(len));

            if(names.back().size() > MAX_READERNAME - 1) {
                names.back().resize(MAX_READERNAME - 1);
            }
        }

        return names;
    }

    uint32_t PcscRemote::sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount) {
        const std::scoped_lock guard{ sock_lock_ };

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

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

        boost::system::error_code ec;
        boost::asio::write(sock_, sb_, boost::asio::transfer_all(), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "write", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        // rsz: count32 + ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
            throw pcsc_error(NS_FuncNameS);
        }

        auto counts = bs.read_le32();
        auto ret = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> remoteContext: {:#016x}, timeout: {}, states: {}",
                           __FUNCTION__, id, context, timeout, counts);

        assertm(counts == statesCount, "count states invalid");

        for(uint32_t it = 0; it < statesCount; ++it) {
            // rsz: state32 + state32 + name32 + atr32
            const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            SCARD_READERSTATE & state = states[it];

            state.dwCurrentState = bs.read_le32();
            state.dwEventState = bs.read_le32();

            auto szReader = bs.read_le32();
            auto cbAtr = bs.read_le32();

            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(szReader), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }

            auto reader = bs.read_string(szReader);

            if(reader != state.szReader) {
                Application::warning("{}: invalid reader, `{}' != `'", __FUNCTION__, reader, state.szReader);
            }

            assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

            state.cbAtr = cbAtr;
            boost::asio::read(sock_, boost::asio::buffer(state.rgbAtr, cbAtr), boost::asio::transfer_exactly(cbAtr), ec);

            if(ec) {
                Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "read", ec.value(), ec.message());
                throw pcsc_error(NS_FuncNameS);
            }
        }

        return ret;
    }

    /// PcscLocal
    PcscLocal::PcscLocal(boost::asio::io_context& ctx, boost::asio::local::stream_protocol::socket && sock, std::shared_ptr<PcscRemote> ptr, PcscSessionBus* sessionBus)
        : ioc_{ctx}, timer_{ioc_}, sock_{std::move(sock)}, remote_{ptr} {
        clientCanceledCb = std::bind(&PcscSessionBus::clientCanceledNotify, sessionBus, std::placeholders::_1);
        clientShutdownCb = std::bind(&PcscSessionBus::clientShutdownNotify, sessionBus, std::placeholders::_1);

        sock_id_ = sock_.native_handle();

        timer_.expires_after(1ms);
        timer_.async_wait(std::bind(&PcscLocal::handlerClientActionStarted, this, std::placeholders::_1));
    }

    PcscLocal::~PcscLocal() {
        sock_.cancel();
        sock_.close();

        timer_.cancel();

        if(transaction_id == id()) {
            transaction_id = 0;
            trans_lock.unlock();
        }

        waitStatusChanged.stop();
    }

    void PcscLocal::handlerClientActionStarted(const boost::system::error_code & ec) {
        if(ec) {
            return;
        }

        bool processed = false;

        try {
            processed = clientAction();
        } catch(const std::exception & ex) {
            Application::error("{}: client id: {}, context: {:#08x}, exception: {}",
                               __FUNCTION__, id(), context, ex.what());
        }

        if(processed) {
            timer_.expires_after(1ms);
            timer_.async_wait(std::bind(&PcscLocal::handlerClientActionStarted, this, std::placeholders::_1));
        } else {
            if(transaction_id == id()) {
                transaction_id = 0;
                trans_lock.unlock();
            }

            boost::asio::post(ioc_, std::bind(clientShutdownCb, this));
        }
    }

    bool PcscLocal::clientAction(void) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {}, wait command", __FUNCTION__, id());

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: len32, cmd32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        uint32_t len = bs.read_le32();
        uint32_t cmd = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {}, cmd: {:#08x}, len: {}", __FUNCTION__, id(), cmd, len);

        if(len != PcscLite::apiCmdLength(cmd) ||
           // transmit: cmd length + variable size
           (cmd == PcscLite::Transmit && len < PcscLite::apiCmdLength(cmd)) ||
           // control: cmd length + variable size
           (cmd == PcscLite::Control && len < PcscLite::apiCmdLength(cmd))) {
            Application::error("{}: clientId: {}, assert len: {}", __FUNCTION__, id(), len);
            return false;
        }

        switch(cmd) {
            case PcscLite::EstablishContext:
                return proxyEstablishContext();

            case PcscLite::ReleaseContext:
                return proxyReleaseContext();

            case PcscLite::Connect:
                return proxyConnect();

            case PcscLite::Reconnect:
                return proxyReconnect();

            case PcscLite::Disconnect:
                return proxyDisconnect();

            case PcscLite::BeginTransaction:
                return proxyBeginTransaction();

            case PcscLite::EndTransaction:
                return proxyEndTransaction();

            case PcscLite::Transmit:
                return proxyTransmit();

            case PcscLite::Status:
                return proxyStatus();

            case PcscLite::Control:
                return proxyControl();

            case PcscLite::GetAttrib:
                return proxyGetAttrib();

            case PcscLite::SetAttrib:
                return proxySetAttrib();

            case PcscLite::Cancel:
                return proxyCancel();

            case PcscLite::GetVersion:
                return proxyGetVersion();

            case PcscLite::GetReaderState:
                return proxyGetReaderState();

            case PcscLite::WaitReaderStateChangeStart:
                return proxyReaderStateChangeStart();

            case PcscLite::WaitReaderStateChangeStop:
                return proxyReaderStateChangeStop();

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

        return false;
    }

    void PcscLocal::replyError(uint32_t cmd, uint32_t err) {
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
                return;

            default:
                Application::error("{}: unknown command, cmd: {:#08x}", __FUNCTION__, cmd);
                return;
        }

        if(zero) {
            binary_buf buf(zero, 0);
            boost::asio::write(sock_, boost::asio::buffer(buf.data(), buf.size()));
        }

        boost::endian::native_to_little_inplace(err);
        boost::asio::write(sock_, boost::asio::buffer(&err, sizeof(err)), boost::asio::transfer_exactly(sizeof(err)));
    }

    bool PcscLocal::proxyEstablishContext(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: scope32, context32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t scope = bs.read_le32();
        // skip: context, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            std::tie(remoteContext, ret) = ptr->sendEstablishedContext(id(), scope);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::EstablishContext, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make 32bit context
            context = Tools::crc32b((const uint8_t*) & remoteContext, sizeof(remoteContext));
            context &= 0x7FFFFFFF;

            // init readers status
            syncReaders();

            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> remoteContext: {:#016x}, localContext: {:#08x}",
                               __FUNCTION__, id(), remoteContext, context);
        } else {
            Application::error("{}: clientId: {}, error: {:#08x} ({})",
                               __FUNCTION__, id(), ret, PcscLite::err2str(ret));
        }

        bs.write_le32(scope).
          write_le32(context).write_le32(ret);

        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyReleaseContext(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: context32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t ctx = bs.read_le32();
        // skip: ret
        bs.skip_bytes(sizeof(uint32_t));

        if(! ctx || ctx != context) {
            Application::error("{}: clientId: {}, invalid localContext: {:#08x}", __FUNCTION__, id(), ctx);
            replyError(PcscLite::ReleaseContext, SCARD_E_INVALID_HANDLE);
            return false;
        }

        auto ptr = remote_.lock();

        if(! ptr) {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::ReleaseContext, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        context = 0;
        remoteContext = 0;

        // set shutdown
        return false;
    }

    bool PcscLocal::proxyConnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: context32, MAX_READERNAME, share32, proto32, handl32, proto32, ret32
        const size_t rsz = sizeof(uint32_t) + MAX_READERNAME + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t ctx = bs.read_le32();
        const auto readerData = bs.read_bytes(MAX_READERNAME);
        const uint32_t shareMode = bs.read_le32();
        const uint32_t prefferedProtocols = bs.read_le32();
        // skip: handle, activeProtocol, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t));

        if(! ctx || ctx != context) {
            Application::error("{}: clientId: {}, invalid localContext: {:#08x}", __FUNCTION__, id(), ctx);
            replyError(PcscLite::Connect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        auto readerName = std::string(readerData.begin(),
                                      std::ranges::find(readerData, 0));
        auto currentReader = PcscLite::findReaderState(readerName);

        if(! currentReader) {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
            return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteContext) {
                Application::error("{}: clientId: {}, invalid remoteContext", __FUNCTION__, id());
                replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(remoteHandle, activeProtocol, ret) = ptr->sendConnect(id(), remoteContext, shareMode, prefferedProtocols, readerName);
        } else {
            Application::error("{}: failed, reader not found: `{}'", __FUNCTION__, readerName);
            replyError(PcscLite::Connect, SCARD_E_INVALID_VALUE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyReconnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, share32, proto32, init32, proto32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        const uint32_t shareMode = bs.read_le32();
        const uint32_t prefferedProtocols = bs.read_le32();
        const uint32_t initialization = bs.read_le32();
        // skip activeProtocol, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::Reconnect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Reconnect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(activeProtocol, ret) = ptr->sendReconnect(id(), remoteHandle, shareMode, prefferedProtocols, initialization);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::Reconnect, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyDisconnect(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, disp32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        const uint32_t disposition = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::Disconnect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Disconnect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendDisconnect(id(), remoteHandle, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::Disconnect, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }


    bool PcscLocal::proxyBeginTransaction(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::BeginTransaction, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::BeginTransaction, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            assertm(reader_, "reader not connected");
            std::tie(ret) = ptr->sendBeginTransaction(id(), remoteHandle);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::BeginTransaction, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyEndTransaction(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, disp32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        const uint32_t disposition = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::EndTransaction, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::EndTransaction, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendEndTransaction(id(), remoteHandle, disposition);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::EndTransaction, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyTransmit(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, sproto32, sprotolen32, slen32, rproto32, rprotolen32, rlen32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

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
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));
            data1 = bs.read_bytes(sendLength);
        }

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::Transmit, SCARD_E_INVALID_HANDLE);
            return false;
        }

        if(sendLength != data1.size()) {
            Application::error("{}: clientId: {}, invalid length, send: {}, data: {}", __FUNCTION__, id(),
                               sendLength, data1.size());
            return false;
        }

        uint32_t ioRecvPciProtocol, ioRecvPciLength, ret;
        binary_buf data2;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Transmit, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ioRecvPciProtocol, ioRecvPciLength, ret, data2) = ptr->sendTransmit(id(), remoteHandle, ioSendPciProtocol, ioSendPciLength, recvLength, data1);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::Transmit, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, pciProtocol: {:#08x}, pciLength: {}, recv size: {}",
                               __FUNCTION__, id(), handle, ioRecvPciProtocol, ioRecvPciLength, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [ `{}' ]", __FUNCTION__, str);
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

        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    void PcscLocal::statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} reader: `{}', state: {:#08x}, protocol: {}, atrLen: {}",
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
                Application::debug(DebugType::Pcsc, "{}: atr: [ `{}' ]", __FUNCTION__, str);
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

    bool PcscLocal::proxyStatus(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        // skip ret
        bs.skip_bytes(sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::Status, SCARD_E_INVALID_HANDLE);
            return false;
        }

        std::string name;
        uint32_t state, protocol, ret;
        binary_buf atr;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(name, state, protocol, ret, atr) = ptr->sendStatus(id(), remoteHandle);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyControl(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, code32, slen32, rlen32, bytes32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        const uint32_t controlCode = bs.read_le32();
        const uint32_t sendLength = bs.read_le32();
        const uint32_t recvLength = bs.read_le32();
        // skip bytesReturned, ret
        bs.skip_bytes(sizeof(uint32_t) + sizeof(uint32_t));

        binary_buf data1;

        if(sendLength) {
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));
            data1 = bs.read_bytes(sendLength);
        }

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::Control, SCARD_E_INVALID_HANDLE);
            return false;
        }

        if(sendLength != data1.size()) {
            Application::error("{}: clientId: {}, invalid length, send: {}, data: {}", __FUNCTION__, id(),
                               sendLength, data1.size());
            return false;
        }

        uint32_t ret;
        binary_buf data2;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Control, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret, data2) = ptr->sendControl(id(), remoteHandle, controlCode, recvLength, data1);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::Control, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, controlCode: {:#08x}, bytesReturned: {}",
                               __FUNCTION__, id(), handle, controlCode, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(data2, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: recv data: [ `{}' ]", __FUNCTION__, str);
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

        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyGetAttrib(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, attr32, MAX_BUFFER_SIZE, len32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t hdl = bs.read_le32();
        const uint32_t attrId = bs.read_le32();
        // skip attr[MAX_BUFFER_SIZE], attrLen, ret
        bs.skip_bytes(MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t));

        if(hdl != handle) {
            Application::error("{}: clientId: {}, invalid localHandle: {:#08x}", __FUNCTION__, id(), hdl);
            replyError(PcscLite::GetAttrib, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;
        binary_buf attr;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::GetAttrib, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret, attr) = ptr->sendGetAttrib(id(), remoteHandle, attrId);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::GetAttrib, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localHandle: {:#08x}, attrId: {}, attrLen: {}",
                               __FUNCTION__, id(), handle, attrId, attr.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::hexString(attr, 2, ",", false);
                Application::debug(DebugType::Pcsc, "{}: attr: [ `{}' ]", __FUNCTION__, str);
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxySetAttrib(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: handl32, attr32, MAX_BUFFER_SIZE, len32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) +
                           MAX_BUFFER_SIZE + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

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
            replyError(PcscLite::SetAttrib, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote_.lock()) {
            if(! remoteHandle) {
                Application::error("{}: clientId: {}, invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::SetAttrib, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendSetAttrib(id(), remoteHandle, attrId, attr);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            replyError(PcscLite::SetAttrib, SCARD_E_NO_SERVICE);
            return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyCancel(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: ctx32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t ctx = bs.read_le32();
        uint32_t ret = bs.read_le32();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} << context: {:#08x}",
                           __FUNCTION__, id(), ctx);

        if(auto remoteContext2 = clientCanceledCb(ctx)) {
            if(auto ptr = remote_.lock()) {
                std::tie(ret) = ptr->sendCancel(id(), remoteContext2);
            } else {
                Application::error("{}: no service", __FUNCTION__);
                replyError(PcscLite::Cancel, SCARD_E_NO_SERVICE);
                return false;
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
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return true;
    }

    bool PcscLocal::proxyGetVersion(void) {
        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        // rsz: ver32, ver32, ret32
        const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
        boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

        const uint32_t versionMajor = bs.read_le32();
        const uint32_t versionMinor = bs.read_le32();
        // skip ret32
        bs.skip_bytes(sizeof(uint32_t));

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> protocol version: {}.{}",
                           __FUNCTION__, id(), versionMajor, versionMinor);
        PcscLite::apiVersion = versionMajor * 10 + versionMinor;

        bs.write_le32(versionMajor).
          write_le32(versionMinor).
          write_le32(0);
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return true;
    }

    bool PcscLocal::proxyGetReaderState(void) {
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> localContext: {:#08x}, readers length: {}",
                           __FUNCTION__, id(), context, readersLength);

        std::scoped_lock guard{ PcscLite::readersLock };
        boost::asio::write(sock_, boost::asio::buffer(PcscLite::readers.data(), readersLength), boost::asio::transfer_all());

        return true;
    }

    uint32_t PcscLocal::waitReadersStatusChanged(uint32_t timeout) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}, timeout: {}",
                           __FUNCTION__, id(), context, timeout);

        if(0 == timeout) {
            auto ret = syncReaders();
            return ret == SCARD_E_NO_READERS_AVAILABLE ?
                   SCARD_S_SUCCESS : ret;
        }

        std::this_thread::sleep_for(100ms);
        auto wait_ms = std::chrono::milliseconds(timeout);
        uint32_t ret = SCARD_E_TIMEOUT;
        Tools::Timeout timeoutLimit(wait_ms);
        Tools::Timeout timeoutSyncReaders(1s);
        waitStatusChanged.start();

        Application::debug(DebugType::Pcsc, "{}: clientId: {} wait: {}", __FUNCTION__, id(), "started");

        while(ret == SCARD_E_TIMEOUT) {
            if(waitStatusChanged.canceled) {
                Application::debug(DebugType::Pcsc, "{}: clientId: {} wait: {}", __FUNCTION__, id(), "timeout");
                ret = SCARD_E_CANCELLED;
                break;
            }

            if(waitStatusChanged.stopped) {
                Application::debug(DebugType::Pcsc, "{}: clientId: {} wait: {}", __FUNCTION__, id(), "stopped");
                ret = SCARD_S_SUCCESS;
                break;
            }

            if(timeoutLimit.check()) {
                Application::debug(DebugType::Pcsc, "{}: clientId: {} wait: {}", __FUNCTION__, id(), "limit");
                break;
            }

            if(timeoutSyncReaders.check()) {
                bool readersChanged = false;
                auto ret2 = syncReaders(& readersChanged);

                if(ret2 != SCARD_S_SUCCESS || readersChanged) {
                    Application::debug(DebugType::Pcsc, "{}: clientId: {} wait: {}", __FUNCTION__, id(), "error");
                    ret = ret2;
                    break;
                }
            }

            std::this_thread::sleep_for(100ms);
        }

        Application::debug(DebugType::Pcsc, "{}: clientId: {} >> timeout: {}", __FUNCTION__, id(), timeout);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        bs.write_le32(timeout).
          write_le32(ret);
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        waitStatusChanged.reset();

        return ret;
    }

    bool PcscLocal::proxyReaderStateChangeStart(void) {

        if(PcscLite::apiVersion < 43) {
            // old protocol: 4.2
            sb_.consume(sb_.size());
            byte::streambuf bs(sb_);

            // rsz: timeout32, ret32
            const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

            const uint32_t timeout = bs.read_le32();
            [[maybe_unused]] const uint32_t ret = bs.read_le32();

            Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}, timeout: {}",
                               __FUNCTION__, id(), context, timeout);

            waitStatusChanged.stop();
            waitStatusChanged.job = std::async(std::launch::async, & PcscLocal::waitReadersStatusChanged, this, timeout);
        } else {
            // new protocol 4.4: empty params
            Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}, timeout: {}",
                               __FUNCTION__, id(), context);
            waitReadersStatusChanged(0);

            // send all readers
            const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
            std::scoped_lock guard{ PcscLite::readersLock };

            boost::asio::write(sock_, boost::asio::buffer(PcscLite::readers.data(), readersLength), boost::asio::transfer_all());
        }

        return true;
    }

    bool PcscLocal::proxyReaderStateChangeStop(void) {
        Application::debug(DebugType::Pcsc, "{}: clientId: {} << localContext: {:#08x}",
                           __FUNCTION__, id(), context);

        sb_.consume(sb_.size());
        byte::streambuf bs(sb_);

        if(PcscLite::apiVersion < 43) {
            // old protocol: 4.2
            // rsz: timeout32, ret32
            const size_t rsz = sizeof(uint32_t) + sizeof(uint32_t);
            boost::asio::read(sock_, sb_, boost::asio::transfer_exactly(rsz));

            [[maybe_unused]] const uint32_t timeout = bs.read_le32();
            [[maybe_unused]] const uint32_t ret = bs.read_le32();
        } else {
            // new protocol: 4.4, empty params
        }

        // stop
        waitStatusChanged.stop();

        bs.write_le32(0).
          write_le32(SCARD_S_SUCCESS);
        boost::asio::write(sock_, sb_, boost::asio::transfer_all());

        return true;
    }

    uint32_t PcscLocal::syncReaderStatus(const std::string & readerName, PcscLite::ReaderState & rd, bool* changed) {
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
            return SCARD_E_NO_SERVICE;
        }

        if(ret == SCARD_E_TIMEOUT) {
            Application::warning("{}: timeout", __FUNCTION__);
            return ret;
        }

        if(ret != SCARD_S_SUCCESS) {
            Application::warning("{}: error: {:#08x} ({})", __FUNCTION__, ret, PcscLite::err2str(ret));
            return ret;
        }

        Application::debug(DebugType::Pcsc, "{}: reader: `{}', currentState: {:#08x}, eventState: {:#08x}, atrLen: {}",
                           __FUNCTION__, readerName, state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::rangeHexString(state.rgbAtr, state.rgbAtr + state.cbAtr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "{}: atr: [ `{}' ]", __FUNCTION__, str);
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

        return SCARD_S_SUCCESS;
    }

    uint32_t PcscLocal::syncReaders(bool* changed) {
        std::list<std::string> names;

        if(auto ptr = remote_.lock()) {
            names = ptr->sendListReaders(id(), remoteContext);
        } else {
            Application::error("{}: no service", __FUNCTION__);
            return SCARD_E_NO_SERVICE;
        }

        if(names.empty()) {
            Application::warning("{}: no readers available", __FUNCTION__);

            bool res = PcscLite::readersReset();

            if(changed && res) {
                *changed = true;
            }

            return SCARD_E_NO_READERS_AVAILABLE;
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
                    return SCARD_E_NO_MEMORY;
                }

                rd->reset();
                syncReaderStatus(name, *rd, changed);
            }
        }

        return SCARD_S_SUCCESS;
    }

    void PcscLocal::canceledAction(void) {
        waitStatusChanged.cancel();
        sock_.close();
    }

    /// PcscSessionBus
    PcscSessionBus::PcscSessionBus(DBusConnectionPtr conn, bool debug) : ApplicationLog("ltsm_session_pcsc"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(*conn, sdbus::ObjectPath {dbus_session_pcsc_path}),
#else
        AdaptorInterfaces(*conn, dbus_session_pcsc_path),
#endif
        ioc_ {2}, signals_ {ioc_}, pcsc_sock_ {ioc_}, dbus_conn_ {std::move(conn)} {
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
        clients_.clear();
        signals_.cancel();
        pcsc_sock_.cancel();
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

        signals_.async_wait([this](const boost::system::error_code & ec, int signal) {
            // skip canceled
            if(ec != boost::asio::error::operation_aborted && (signal == SIGTERM || signal == SIGINT)) {
                this->stop();
            }
        });

        auto sdbus_job = std::async(std::launch::async, [this]() {
            try {
                dbus_conn_->enterEventLoop();
            } catch(const std::exception & err) {
                Application::error("sdbus exception: {}", err.what());
                boost::asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
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

    void PcscSessionBus::handlerLocalAccepted(const boost::system::error_code & ec, boost::asio::local::stream_protocol::socket peer) {
        if(ec) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "accept", ec.value(), ec.message());
            return;
        }

        const std::scoped_lock guard{ clients_lock_ };

        Application::debug(DebugType::App, "{}: add clientId: {}", __FUNCTION__, peer.native_handle());
        this->clients_.emplace_front(ioc_, std::move(peer), remote_, this);
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
    }

    int32_t PcscSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "{}", __FUNCTION__);
        return LTSM_SESSION_PCSC_VERSION;
    }

    void PcscSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "{}: pid: {}", __FUNCTION__, getpid());
        boost::asio::post(ioc_, std::bind(&PcscSessionBus::stop, this));
    }

    void PcscSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "{}: level: {}", __FUNCTION__, level);
        setDebugLevel(level);
    }

    bool PcscSessionBus::connectChannel(const std::string & path) {
        Application::debug(DebugType::Dbus, "{}: client socket path: `{}'", __FUNCTION__, path);

        std::promise<bool> connected;
        auto res = connected.get_future();
        remote_ = std::make_shared<PcscRemote>(ioc_, path, std::move(connected));

        if(res.get()) {
            if(pcsc_sock_.is_open()) {
                pcsc_sock_.close();
            }

            if(std::filesystem::is_socket(pcsc_ep_.path())) {
                std::filesystem::remove(pcsc_ep_.path());
                Application::warning("{}: old socket removed", __FUNCTION__);
            }

            try {
                pcsc_sock_.open(pcsc_ep_.protocol());
                pcsc_sock_.bind(pcsc_ep_);
                pcsc_sock_.listen();
            } catch(const std::exception & err) {
                Application::error("{}: exception: {}", __FUNCTION__, err.what());
                return false;
            }

            pcsc_sock_.async_accept(std::bind(&PcscSessionBus::handlerLocalAccepted, this, std::placeholders::_1, std::placeholders::_2));
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
