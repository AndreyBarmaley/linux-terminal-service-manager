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

using namespace std::chrono_literals;

namespace PcscLite {
    uint32_t apiVersion = 0;

    std::array<ReaderState, PCSCLITE_MAX_READERS_CONTEXTS> readers;
    std::mutex readersLock;

    ReaderState* findReaderState(const std::string & name) {
        std::scoped_lock guard{ readersLock };
        auto it = std::find_if(readers.begin(), readers.end(),
        [ &](auto & rd) {
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

            auto it = std::find_if(names.begin(), names.end(),
            [&rd](auto & name) {
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
                LTSM::Application::warning("%s: unknown cmd: 0x%08" PRIx32, __FUNCTION__, cmd);
                break;
        }

        return 0;
    }
}

namespace LTSM {
    std::unique_ptr<sdbus::IConnection> conn;

    std::mutex transLock;
    int32_t transactionId = 0;

    void signalHandler(int sig) {
        if(sig == SIGTERM || sig == SIGINT) {
            if(conn) {
                conn->leaveEventLoop();
            }
        }
    }

    /// PcscRemote
    std::tuple<uint64_t, uint32_t>
    PcscRemote::sendEstablishedContext(const int32_t & id, const uint32_t & scope) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << scope: %" PRIu32, __FUNCTION__, id, scope);

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::EstablishContext);
        sock.sendIntLE32(scope);
        sock.sendFlush();

        // recv
        auto context = sock.recvIntLE64();
        auto ret = sock.recvIntLE32();

        return std::make_tuple(context, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendReleaseContext(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteContext: 0x%016" PRIx64,
                           __FUNCTION__, id, context);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::ReleaseContext);
        sock.sendIntLE64(context);
        sock.sendFlush();

        // recv
        auto ret = sock.recvIntLE32();

        return std::make_tuple(ret);
    }

    std::tuple<uint64_t, uint32_t, uint32_t>
    PcscRemote::sendConnect(const int32_t & id, const uint64_t & context, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const std::string & readerName) {
        transLock.lock();
        transactionId = id;

        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteContext: 0x%016" PRIx64 ", shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32 ", reader: `%s'",
                           __FUNCTION__, id, context, shareMode, prefferedProtocols, readerName.c_str());
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Connect);
        sock.sendIntLE64(context).sendIntLE32(shareMode).sendIntLE32(prefferedProtocols);
        sock.sendIntLE32(readerName.size()).sendString(readerName);
        sock.sendFlush();

        // recv
        auto handle = sock.recvIntLE64();
        auto activeProtocol = sock.recvIntLE32();
        auto ret = sock.recvIntLE32();

        transactionId = 0;
        transLock.unlock();

        return std::make_tuple(handle, activeProtocol, ret);
    }

    std::tuple<uint32_t, uint32_t>
    PcscRemote::sendReconnect(const int32_t & id, const uint64_t & handle, const uint32_t & shareMode, const uint32_t & prefferedProtocols, const uint32_t & initialization) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", shareMode: %" PRIu32
                           ", prefferedProtocols: %" PRIu32 ", inititalization: %" PRIu32,
                           __FUNCTION__, id, handle, shareMode, prefferedProtocols, initialization);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Reconnect);
        sock.sendIntLE64(handle).sendIntLE32(shareMode).sendIntLE32(prefferedProtocols).sendIntLE32(initialization);
        sock.sendFlush();

        // recv
        auto activeProtocol = sock.recvIntLE32();
        auto ret = sock.recvIntLE32();

        return std::make_tuple(activeProtocol, ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendDisconnect(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", disposition: %" PRIu32,
                           __FUNCTION__, id, handle, disposition);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Disconnect);
        sock.sendIntLE64(handle).sendIntLE32(disposition);
        sock.sendFlush();

        // recv
        auto ret = sock.recvIntLE32();
        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendBeginTransaction(const int32_t & id, const uint64_t & handle) {
        transLock.lock();
        transactionId = id;

        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64,
                           __FUNCTION__, id, handle);

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::BeginTransaction);
        sock.sendIntLE64(handle);
        sock.sendFlush();

        // recv
        auto ret = sock.recvIntLE32();

        if(ret != SCARD_S_SUCCESS) {
            transactionId = 0;
            transLock.unlock();
        }

        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendEndTransaction(const int32_t & id, const uint64_t & handle, const uint32_t & disposition) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", disposition: %" PRIu32,
                           __FUNCTION__, id, handle, disposition);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::EndTransaction);
        sock.sendIntLE64(handle).sendIntLE32(disposition);
        sock.sendFlush();

        // recv
        auto ret = sock.recvIntLE32();

        transactionId = 0;
        transLock.unlock();

        return std::make_tuple(ret);
    }

    std::tuple<uint32_t, uint32_t, uint32_t, binary_buf>
    PcscRemote::sendTransmit(const int32_t & id, const uint64_t & handle, const uint32_t & ioSendPciProtocol, const uint32_t & ioSendPciLength, const uint32_t & recvLength, const binary_buf & data1) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64
                           ", pciProtocol: 0x%08" PRIx32 ", pciLength: %" PRIu32  ", send size: %lu, recv size: %" PRIu32,
                           __FUNCTION__, id, handle, ioSendPciProtocol, ioSendPciLength, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::buffer2hexstring(data1.begin(), data1.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: send data: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Transmit);
        sock.sendIntLE64(handle).sendIntLE32(ioSendPciProtocol).sendIntLE32(ioSendPciLength).sendIntLE32(recvLength).sendIntLE32(data1.size());

        if(data1.size()) {
            sock.sendData(data1);
        }

        sock.sendFlush();

        // recv
        auto ioRecvPciProtocol = sock.recvIntLE32();
        auto ioRecvPciLength = sock.recvIntLE32();
        auto bytesReturned = sock.recvIntLE32();
        auto ret = sock.recvIntLE32();
        auto data2 = sock.recvData(bytesReturned);

        return std::make_tuple(ioRecvPciProtocol, ioRecvPciLength, ret, data2);
    }

    std::tuple<std::string, uint32_t, uint32_t, uint32_t, binary_buf>
    PcscRemote::sendStatus(const int32_t & id, const uint64_t & handle) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64,
                           __FUNCTION__, id, handle);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Status);
        sock.sendIntLE64(handle);
        sock.sendFlush();

        // recv
        auto nameLen = sock.recvIntLE32();
        auto name = sock.recvString(nameLen);
        auto state = sock.recvIntLE32();
        auto protocol = sock.recvIntLE32();
        auto atrLen = sock.recvIntLE32();
        auto atr = sock.recvData(atrLen);
        auto ret = sock.recvIntLE32();

        return std::make_tuple(name, state, protocol, ret, atr);
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendControl(const int32_t & id, const uint64_t & handle, const uint32_t & controlCode, const uint32_t & recvLength, const binary_buf & data1) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", controlCode: 0x%08"
                           PRIx32 ", send size: %lu, recv size: %" PRIu32,
                           __FUNCTION__, id, handle, controlCode, data1.size(), recvLength);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::buffer2hexstring(data1.begin(), data1.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: send data: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Control);
        sock.sendIntLE64(handle).sendIntLE32(controlCode).sendIntLE32(data1.size()).sendIntLE32(recvLength);

        if(data1.size()) {
            sock.sendData(data1);
        }

        sock.sendFlush();

        // recv
        auto bytesReturned = sock.recvIntLE32();
        auto ret = sock.recvIntLE32();
        auto data2 = sock.recvData(bytesReturned);

        return std::make_tuple(ret, data2);
    }

    std::tuple<uint32_t, binary_buf>
    PcscRemote::sendGetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle: 0x%016" PRIx64 ", attrId: %" PRIu32,
                           __FUNCTION__, id, handle, attrId);
        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::GetAttrib);
        sock.sendIntLE64(handle).sendIntLE32(attrId);
        sock.sendFlush();

        // recv
        auto attrLen = sock.recvIntLE32();
        auto ret = sock.recvIntLE32();
        assertm(attrLen <= MAX_BUFFER_SIZE, "attr length invalid");
        auto attr = sock.recvData(attrLen);

        return std::make_tuple(ret, attr);
    }

    std::tuple<uint32_t>
    PcscRemote::sendSetAttrib(const int32_t & id, const uint64_t & handle, const uint32_t & attrId, const binary_buf & attr) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteHandle 0x%016" PRIx64 ", attrId: %" PRIu32 ", attrLength %lu",
                           __FUNCTION__, id, handle, attrId, attr.size());

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::buffer2hexstring(attr.begin(), attr.end(), 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: attr: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::SetAttrib);
        sock.sendIntLE64(handle).sendIntLE32(attrId).sendIntLE32(attr.size());

        if(attr.size()) {
            sock.sendData(attr);
        }

        sock.sendFlush();

        // recv
        auto ret = sock.recvIntLE32();
        return std::make_tuple(ret);
    }

    std::tuple<uint32_t>
    PcscRemote::sendCancel(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sockLock };
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << remoteContext 0x%016" PRIx64,
                           __FUNCTION__, id, context);

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::Cancel);
        sock.sendIntLE64(context).sendFlush();

        // recv
        auto ret = sock.recvIntLE32();
        return std::make_tuple(ret);
    }

    std::list<std::string> PcscRemote::sendListReaders(const int32_t & id, const uint64_t & context) {
        const std::scoped_lock guard{ sockLock };

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::ListReaders);
        sock.sendIntLE64(context);
        sock.sendFlush();

        // recv
        uint32_t readersCount = sock.recvIntLE32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localContext: 0x%08" PRIx32 ", readers count: %" PRIu32,
                           __FUNCTION__, id, context, readersCount);

        std::list<std::string> names;

        while(readersCount--) {
            uint32_t len = sock.recvIntLE32();
            names.emplace_back(sock.recvString(len));

            if(names.back().size() > MAX_READERNAME - 1) {
                names.back().resize(MAX_READERNAME - 1);
            }
        }

        return names;
    }

    uint32_t PcscRemote::sendGetStatusChange(const int32_t & id, const uint64_t & context, uint32_t timeout, SCARD_READERSTATE* states, uint32_t statesCount) {
        const std::scoped_lock guard{ sockLock };

        // send
        sock.sendIntLE16(PcscOp::Init).sendIntLE16(PcscLite::GetStatusChange);
        sock.sendIntLE64(context).sendIntLE32(timeout).sendIntLE32(statesCount);

        for(uint32_t it = 0; it < statesCount; ++it) {
            const SCARD_READERSTATE & state = states[it];
            sock.sendIntLE32(strnlen(state.szReader, MAX_READERNAME));
            sock.sendIntLE32(state.dwCurrentState);
            sock.sendIntLE32(state.cbAtr);
            sock.sendString(state.szReader);
            sock.sendRaw(state.rgbAtr, state.cbAtr);
        }

        sock.sendFlush();

        // recv
        uint32_t counts = sock.recvIntLE32();
        uint32_t ret = sock.recvIntLE32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> remoteContext: 0x%016" PRIx64 ", timeout: %" PRIu32 ", states: %" PRIu32,
                           __FUNCTION__, id, context, timeout, counts);

        assertm(counts == statesCount, "count states invalid");

        for(uint32_t it = 0; it < statesCount; ++it) {
            SCARD_READERSTATE & state = states[it];
            state.dwCurrentState = sock.recvIntLE32();
            state.dwEventState = sock.recvIntLE32();

            uint32_t szReader = sock.recvIntLE32();
            uint32_t cbAtr = sock.recvIntLE32();

            std::string reader = sock.recvString(szReader);

            if(reader != state.szReader) {
                Application::warning("%s: invalid reader, `%s' != `'", __FUNCTION__, reader.c_str(), state.szReader);
            }

            assertm(cbAtr <= sizeof(state.rgbAtr), "atr length invalid");
            state.cbAtr = cbAtr;
            sock.recvData(state.rgbAtr, cbAtr);
        }

        return ret;
    }

    /// PcscLocal
    PcscLocal::PcscLocal(int fd, const std::shared_ptr<PcscRemote> & ptr, PcscSessionBus* sessionBus)
        : sock(fd, false /* disable statistic */), remote(ptr) {
        clientCanceledCb = std::bind(&PcscSessionBus::clientCanceledNotify, sessionBus, std::placeholders::_1);

        thread = std::thread([this, clientShutdownCb = std::bind(&PcscSessionBus::clientShutdownNotify, sessionBus, std::placeholders::_1)]() {
            bool processed = true;

            while(processed) {
                try {
                    processed = this->clientAction();
                } catch(const std::exception & ex) {
                    Application::error("%s: client id: %" PRId32 ", context: 0x%08" PRIx32 ", exception: %s",
                                       "PcscLocalThread", this->id(), this->context, ex.what());
                    processed = false;
                }
            }

            if(transactionId == this->id()) {
                transactionId = 0;
                transLock.unlock();
            }

            std::thread(clientShutdownCb, this).detach();
        });
    }

    PcscLocal::~PcscLocal() {
        if(transactionId == id()) {
            transactionId = 0;
            transLock.unlock();
        }

        sock.reset();

        waitStatusChanged.stop();

        if(thread.joinable()) {
            thread.join();
        }
    }

    bool PcscLocal::clientAction(void) {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", wait command", __FUNCTION__, id());

        uint32_t len = sock.recvInt32();
        uint32_t cmd = sock.recvInt32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 ", cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, id(), cmd, len);

        if(len != PcscLite::apiCmdLength(cmd) ||
           // transmit: cmd length + variable size
           (cmd == PcscLite::Transmit && len < PcscLite::apiCmdLength(cmd)) ||
           // control: cmd length + variable size
           (cmd == PcscLite::Control && len < PcscLite::apiCmdLength(cmd))) {
            Application::error("%s: clientId: %" PRId32 ", assert len: %" PRIu32, __FUNCTION__, id(), len);
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
                Application::error("%s: not used cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, cmd, len);
                break;

            default:
                Application::error("%s: unknown cmd: 0x%08" PRIx32 ", len: %" PRIu32, __FUNCTION__, cmd, len);
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
                Application::warning("%s: not implemented, cmd: 0x%08" PRIx32, __FUNCTION__, cmd);
                return;

            default:
                Application::error("%s: unknown command, cmd: 0x%08" PRIx32, __FUNCTION__, cmd);
                return;
        }

        sock.sendZero(zero).sendInt32(err).sendFlush();
    }

    bool PcscLocal::proxyEstablishContext(void) {
        const uint32_t scope = sock.recvInt32();
        // skip: context, ret
        sock.recvSkip(8);

        if(context) {
            Application::error("%s: clientId: %" PRId32 ", invalid context", __FUNCTION__, id());
            replyError(PcscLite::EstablishContext, SCARD_E_INVALID_PARAMETER);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote.lock()) {
            std::tie(remoteContext, ret) = ptr->sendEstablishedContext(id(), scope);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::EstablishContext, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make 32bit context
            context = Tools::crc32b((const uint8_t*) & remoteContext, sizeof(remoteContext));
            context &= 0x7FFFFFFF;

            // init readers status
            syncReaders();

            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> remoteContext: 0x%016" PRIx64 ", localContext: 0x%08" PRIx32,
                               __FUNCTION__, id(), remoteContext, context);
        } else {
            Application::error("%s: clientId: %" PRId32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(scope).sendInt32(context).sendInt32(ret).sendFlush();
        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyReleaseContext(void) {
        const uint32_t ctx = sock.recvInt32();
        // skip: ret
        sock.recvSkip(4);

        if(! ctx || ctx != context) {
            Application::error("%s: clientId: %" PRId32 ", invalid localContext: 0x%08" PRIx32, __FUNCTION__, id(), ctx);
            replyError(PcscLite::ReleaseContext, SCARD_E_INVALID_HANDLE);
            return false;
        }

        auto ptr = remote.lock();

        if(! ptr) {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::ReleaseContext, SCARD_E_NO_SERVICE);
            return false;
        }

        uint32_t ret = SCARD_S_SUCCESS;

        if(remoteContext) {
            std::tie(ret) = ptr->sendReleaseContext(id(), remoteContext);

            if(ret != SCARD_S_SUCCESS) {
                Application::error("%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                                   __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
            }
        }

        sock.sendInt32(context).sendInt32(ret).sendFlush();

        context = 0;
        remoteContext = 0;

        // set shutdown
        return false;
    }

    bool PcscLocal::proxyConnect(void) {
        const uint32_t ctx = sock.recvInt32();
        const auto readerData = sock.recvData(MAX_READERNAME);
        const uint32_t shareMode = sock.recvInt32();
        const uint32_t prefferedProtocols = sock.recvInt32();
        // skip: handle, activeProtocol, ret
        sock.recvSkip(12);

        if(! ctx || ctx != context) {
            Application::error("%s: clientId: %" PRId32 ", invalid localContext: 0x%08" PRIx32, __FUNCTION__, id(), ctx);
            replyError(PcscLite::Connect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        auto readerName = std::string(readerData.begin(),
                                      std::find(readerData.begin(), readerData.end(), 0));
        auto currentReader = PcscLite::findReaderState(readerName);

        if(! currentReader) {
            Application::error("%s: failed, reader not found: `%s'", __FUNCTION__, readerName.c_str());
            replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
            return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote.lock()) {
            if(! remoteContext) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteContext", __FUNCTION__, id());
                replyError(PcscLite::Connect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(remoteHandle, activeProtocol, ret) = ptr->sendConnect(id(), remoteContext, shareMode, prefferedProtocols, readerName);
        } else {
            Application::error("%s: failed, reader not found: `%s'", __FUNCTION__, readerName.c_str());
            replyError(PcscLite::Connect, SCARD_E_INVALID_VALUE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // make localHandle
            handle = ret != SCARD_S_SUCCESS ? 0 : Tools::crc32b((const uint8_t*) & remoteHandle, sizeof(remoteHandle));
            handle &= 0x7FFFFFFF;

            // sync reader
            reader = currentReader;

            if(reader) {
                std::scoped_lock guard{ PcscLite::readersLock };
                reader->share = shareMode;
                reader->protocol = activeProtocol;
            }

            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> remoteHandle: 0x%016" PRIx64 ", localHandle: 0x%08" PRIx32
                               ", activeProtocol: %" PRIu32, __FUNCTION__, id(), remoteHandle, handle, activeProtocol);
        } else {
            Application::error("%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(context).sendData(readerData).sendInt32(shareMode).
            sendInt32(prefferedProtocols).sendInt32(handle).sendInt32(activeProtocol).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyReconnect(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t shareMode = sock.recvInt32();
        const uint32_t prefferedProtocols = sock.recvInt32();
        const uint32_t initialization = sock.recvInt32();
        // skip activeProtocol, ret
        sock.recvSkip(8);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::Reconnect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t activeProtocol, ret;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Reconnect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(activeProtocol, ret) = ptr->sendReconnect(id(), remoteHandle, shareMode, prefferedProtocols, initialization);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::Reconnect, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            assertm(reader, "reader not connected");
            std::scoped_lock guard{ PcscLite::readersLock };
            reader->share = shareMode;
            reader->protocol = activeProtocol;
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32 ", inititalization: %" PRIu32 ", activeProtocol: %" PRIu32,
                               __FUNCTION__, id(), handle, shareMode, prefferedProtocols, initialization, activeProtocol);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(shareMode).sendInt32(prefferedProtocols).
            sendInt32(initialization).sendInt32(activeProtocol).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyDisconnect(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t disposition = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::Disconnect, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Disconnect, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendDisconnect(id(), remoteHandle, disposition);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::Disconnect, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            // sync after
            handle = 0;
            remoteHandle = 0;
            assertm(reader, "reader not connected");

            std::scoped_lock guard{ PcscLite::readersLock };
            reader->share = 0;
            reader->protocol = 0;
            reader = nullptr;

            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", disposition: %" PRIu32,
                               __FUNCTION__, id(), handle, disposition);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(disposition).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }


    bool PcscLocal::proxyBeginTransaction(void) {
        const uint32_t hdl = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::BeginTransaction, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::BeginTransaction, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            assertm(reader, "reader not connected");
            std::tie(ret) = ptr->sendBeginTransaction(id(), remoteHandle);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::BeginTransaction, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32,
                               __FUNCTION__, id(), handle);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyEndTransaction(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t disposition = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::EndTransaction, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::EndTransaction, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendEndTransaction(id(), remoteHandle, disposition);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::EndTransaction, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", disposition: %" PRIu32,
                               __FUNCTION__, id(), handle, disposition);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(disposition).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyTransmit(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t ioSendPciProtocol = sock.recvInt32();
        const uint32_t ioSendPciLength = sock.recvInt32();
        const uint32_t sendLength = sock.recvInt32();
        // skip ioRecvPciProtocol, ioRecvPciLength
        sock.recvSkip(8);
        const uint32_t recvLength = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);
        const auto data1 = sock.recvData(sendLength);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::Transmit, SCARD_E_INVALID_HANDLE);
            return false;
        }

        if(sendLength != data1.size()) {
            Application::error("%s: clientId: %" PRId32 ", invalid length, send: %" PRIu32 ", data: %lu", __FUNCTION__, id(),
                               sendLength, data1.size());
            return false;
        }

        uint32_t ioRecvPciProtocol, ioRecvPciLength, ret;
        binary_buf data2;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Transmit, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ioRecvPciProtocol, ioRecvPciLength, ret, data2) = ptr->sendTransmit(id(), remoteHandle, ioSendPciProtocol, ioSendPciLength, recvLength, data1);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::Transmit, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32
                               ", pciProtocol: 0x%08" PRIx32 ", pciLength: %" PRIu32 ", recv size: %lu",
                               __FUNCTION__, id(), handle, ioRecvPciProtocol, ioRecvPciLength, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::buffer2hexstring(data2.begin(), data2.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: recv data: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(ioSendPciProtocol).sendInt32(ioSendPciLength).sendInt32(sendLength).
            sendInt32(ioRecvPciProtocol).sendInt32(ioRecvPciLength).sendInt32(data2.size()).sendInt32(ret);

        if(data2.size()) {
            sock.sendData(data2);
        }

        sock.sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    void PcscLocal::statusApply(const std::string & name, const uint32_t & state, const uint32_t & protocol, const binary_buf & atr) {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " reader: `%s', state: %" PRIx32 ", protocol: %" PRIu32 ", atrLen: %" PRIu32,
                           __FUNCTION__, id(), name.c_str(), state, protocol, atr.size());

        assertm(reader, "reader not connected");
        assertm(atr.size() <= sizeof(reader->atr), "atr length invalid");
        std::scoped_lock guard{ PcscLite::readersLock };

        // atr changed
        if(! std::equal(atr.begin(), atr.end(), std::begin(reader->atr))) {
            std::fill(std::begin(reader->atr), std::end(reader->atr), 0);
            std::copy_n(atr.data(), atr.size(), std::begin(reader->atr));

            reader->atrLen = atr.size();

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::buffer2hexstring(atr.begin(), atr.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: atr: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        }

        // protocol changed
        if(protocol != reader->protocol) {
            reader->protocol = protocol;
        }

        if(state != reader->state) {
            reader->state = state;
        }
    }

    bool PcscLocal::proxyStatus(void) {
        const uint32_t hdl = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::Status, SCARD_E_INVALID_HANDLE);
            return false;
        }

        std::string name;
        uint32_t state, protocol, ret;
        binary_buf atr;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Status, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(name, state, protocol, ret, atr) = ptr->sendStatus(id(), remoteHandle);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::Status, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32,
                               __FUNCTION__, id(), handle);

            statusApply(name, state, protocol, atr);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(handle).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyControl(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t controlCode = sock.recvInt32();
        const uint32_t sendLength = sock.recvInt32();
        const uint32_t recvLength = sock.recvInt32();
        // skip bytesReturned, ret
        sock.recvSkip(8);
        auto data1 = sock.recvData(sendLength);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::Control, SCARD_E_INVALID_HANDLE);
            return false;
        }

        if(sendLength != data1.size()) {
            Application::error("%s: clientId: %" PRId32 ", invalid length, send: %" PRIu32 ", data: %lu", __FUNCTION__, id(),
                               sendLength, data1.size());
            return false;
        }

        uint32_t ret;
        binary_buf data2;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::Control, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret, data2) = ptr->sendControl(id(), remoteHandle, controlCode, recvLength, data1);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::Control, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", controlCode: 0x%08" PRIx32 ", bytesReturned: %lu",
                               __FUNCTION__, id(), handle, controlCode, data2.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::buffer2hexstring(data2.begin(), data2.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: recv data: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        // reply
        sock.sendInt32(handle).sendInt32(controlCode).sendInt32(sendLength).sendInt32(recvLength).
            sendInt32(data2.size()).sendInt32(ret);

        if(data2.size()) {
            sock.sendData(data2);
        }

        sock.sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyGetAttrib(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t attrId = sock.recvInt32();
        // skip attr[MAX_BUFFER_SIZE], attrLen, ret
        sock.recvSkip(MAX_BUFFER_SIZE + 8);

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::GetAttrib, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;
        binary_buf attr;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::GetAttrib, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret, attr) = ptr->sendGetAttrib(id(), remoteHandle, attrId);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::GetAttrib, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle: 0x%08" PRIx32 ", attrId: %" PRIu32 ", attrLen: %lu",
                               __FUNCTION__, id(), handle, attrId, attr.size());

            if(Application::isDebugLevel(DebugLevel::Trace)) {
                auto str = Tools::buffer2hexstring(attr.begin(), attr.end(), 2, ",", false);
                Application::debug(DebugType::Pcsc, "%s: attr: [ `%s' ]", __FUNCTION__, str.c_str());
            }
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        if(attr.size() != MAX_BUFFER_SIZE) {
            attr.resize(MAX_BUFFER_SIZE, 0);
        }

        sock.sendInt32(handle).sendInt32(attrId).
            sendData(attr).sendInt32(attr.size()).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxySetAttrib(void) {
        const uint32_t hdl = sock.recvInt32();
        const uint32_t attrId = sock.recvInt32();
        auto attr = sock.recvData(MAX_BUFFER_SIZE);
        const uint32_t attrLen = sock.recvInt32();
        // skip ret
        sock.recvSkip(4);

        // fixed attr
        if(attrLen < attr.size()) {
            attr.resize(attrLen);
        }

        if(hdl != handle) {
            Application::error("%s: clientId: %" PRId32 ", invalid localHandle: 0x%08" PRIx32, __FUNCTION__, id(), hdl);
            replyError(PcscLite::SetAttrib, SCARD_E_INVALID_HANDLE);
            return false;
        }

        uint32_t ret;

        if(auto ptr = remote.lock()) {
            if(! remoteHandle) {
                Application::error("%s: clientId: %" PRId32 ", invalid remoteHandle", __FUNCTION__, id());
                replyError(PcscLite::SetAttrib, SCARD_F_INTERNAL_ERROR);
                return false;
            }

            std::tie(ret) = ptr->sendSetAttrib(id(), remoteHandle, attrId, attr);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            replyError(PcscLite::SetAttrib, SCARD_E_NO_SERVICE);
            return false;
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localHandle 0x%08" PRIx32,
                               __FUNCTION__, id(), handle);
        } else {
            Application::error("%s: clientId: %" PRId32 ", handle: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), handle, ret, PcscLite::err2str(ret));
        }

        if(attr.size() != MAX_BUFFER_SIZE) {
            attr.resize(MAX_BUFFER_SIZE, 0);
        }

        sock.sendInt32(handle).sendInt32(attrId).
            sendData(attr).sendInt32(attrLen).sendInt32(ret).sendFlush();

        return ret == SCARD_S_SUCCESS;
    }

    bool PcscLocal::proxyCancel(void) {
        const uint32_t ctx = sock.recvInt32();
        uint32_t ret = sock.recvInt32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << context: 0x%08" PRIx32,
                           __FUNCTION__, id(), ctx);

        if(auto remoteContext2 = clientCanceledCb(ctx)) {
            if(auto ptr = remote.lock()) {
                std::tie(ret) = ptr->sendCancel(id(), remoteContext2);
            } else {
                Application::error("%s: no service", __FUNCTION__);
                replyError(PcscLite::Cancel, SCARD_E_NO_SERVICE);
                return false;
            }
        } else {
            Application::error("%s: clientId: 0x%08" PRIx32 ", canceled not found, context: 0x%08" PRIx32,
                               __FUNCTION__, id(), ctx);
        }

        if(ret == SCARD_S_SUCCESS) {
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localContext 0x%08" PRIx32,
                               __FUNCTION__, id(), context);
        } else {
            Application::error("%s: clientId: %" PRId32 ", context: 0x%08" PRIx32 ", error: 0x%08" PRIx32 " (%s)",
                               __FUNCTION__, id(), context, ret, PcscLite::err2str(ret));
        }

        sock.sendInt32(ctx).sendInt32(ret).sendFlush();
        return true;
    }

    bool PcscLocal::proxyGetVersion(void) {
        const uint32_t versionMajor = sock.recvInt32();
        const uint32_t versionMinor = sock.recvInt32();
        const uint32_t ret = sock.recvInt32();

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> protocol version: %" PRIu32 ".%" PRIu32,
                           __FUNCTION__, id(), versionMajor, versionMinor);
        PcscLite::apiVersion = versionMajor * 10 + versionMinor;

        sock.sendInt32(versionMajor).sendInt32(versionMinor).sendInt32(0).sendFlush();
        return true;
    }

    bool PcscLocal::proxyGetReaderState(void) {
        const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> localContext: 0x%08" PRIx32 ", readers length: %" PRIu32,
                           __FUNCTION__, id(), context, readersLength);

        std::scoped_lock guard{ PcscLite::readersLock };

        sock.sendRaw(PcscLite::readers.data(), readersLength);
        sock.sendFlush();

        return true;
    }

    uint32_t PcscLocal::waitReadersStatusChanged(uint32_t timeout) {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << localContext: 0x%08" PRIx32 ", timeout: %" PRIu32,
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

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " wait: %s", __FUNCTION__, id(), "started");

        while(ret == SCARD_E_TIMEOUT) {
            if(waitStatusChanged.canceled) {
                Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " wait: %s", __FUNCTION__, id(), "timeout");
                ret = SCARD_E_CANCELLED;
                break;
            }

            if(waitStatusChanged.stopped) {
                Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " wait: %s", __FUNCTION__, id(), "stopped");
                ret = SCARD_S_SUCCESS;
                break;
            }

            if(timeoutLimit.check()) {
                Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " wait: %s", __FUNCTION__, id(), "limit");
                break;
            }

            if(timeoutSyncReaders.check()) {
                bool readersChanged = false;
                auto ret2 = syncReaders(& readersChanged);

                if(ret2 != SCARD_S_SUCCESS || readersChanged) {
                    Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " wait: %s", __FUNCTION__, id(), "error");
                    ret = ret2;
                    break;
                }
            }

            std::this_thread::sleep_for(100ms);
        }

        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " >> timeout: %" PRIu32, __FUNCTION__, id(), timeout);
        sock.sendInt32(timeout).sendInt32(ret).sendFlush();
        waitStatusChanged.reset();

        return ret;
    }

    bool PcscLocal::proxyReaderStateChangeStart(void) {
        if(PcscLite::apiVersion < 43) {
            // old protocol: 4.2
            const uint32_t timeout = sock.recvInt32();
            const uint32_t ret = sock.recvInt32();
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << localContext: 0x%08" PRIx32 ", timeout: %" PRIu32,
                               __FUNCTION__, id(), context, timeout);
            waitStatusChanged.stop();
            waitStatusChanged.job = std::async(std::launch::async, & PcscLocal::waitReadersStatusChanged, this, timeout);
        } else {
            // new protocol 4.4: empty params
            Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << localContext: 0x%08" PRIx32 ", timeout: %" PRIu32,
                               __FUNCTION__, id(), context);
            waitReadersStatusChanged(0);

            // send all readers
            const uint32_t readersLength = PcscLite::readers.size() * sizeof(PcscLite::ReaderState);
            std::scoped_lock guard{ PcscLite::readersLock };
            sock.sendRaw(PcscLite::readers.data(), readersLength);
            sock.sendFlush();
        }

        return true;
    }

    bool PcscLocal::proxyReaderStateChangeStop(void) {
        Application::debug(DebugType::Pcsc, "%s: clientId: %" PRId32 " << localContext: 0x%08" PRIx32,
                           __FUNCTION__, id(), context);

        if(PcscLite::apiVersion < 43) {
            // old protocol: 4.2
            const uint32_t timeout = sock.recvInt32();
            const uint32_t ret = sock.recvInt32();
        } else {
            // new protocol: 4.4, empty params
        }

        // stop
        waitStatusChanged.stop();
        sock.sendInt32(0).sendInt32(SCARD_S_SUCCESS).sendFlush();

        return true;
    }

    uint32_t PcscLocal::syncReaderStatus(const std::string & readerName, PcscLite::ReaderState & rd, bool* changed) {
        const uint32_t timeout = 0;
        SCARD_READERSTATE state = {};

        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;
        state.cbAtr = MAX_ATR_SIZE;

        uint32_t ret = SCARD_S_SUCCESS;

        if(auto ptr = remote.lock()) {
            ret = ptr->sendGetStatusChange(id(), remoteContext, timeout, & state, 1);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            return SCARD_E_NO_SERVICE;
        }

        if(ret == SCARD_E_TIMEOUT) {
            Application::warning("%s: timeout", __FUNCTION__);
            return ret;
        }

        if(ret != SCARD_S_SUCCESS) {
            Application::warning("%s: error: 0x%08" PRIx32 " (%s)", __FUNCTION__, ret, PcscLite::err2str(ret));
            return ret;
        }

        Application::debug(DebugType::Pcsc, "%s: reader: `%s', currentState: 0x%08" PRIx32 ", eventState: 0x%08" PRIx32 ", atrLen: %" PRIu32,
                           __FUNCTION__, readerName.c_str(), state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(Application::isDebugLevel(DebugLevel::Trace)) {
            auto str = Tools::buffer2hexstring(state.rgbAtr, state.rgbAtr + state.cbAtr, 2, ",", false);
            Application::debug(DebugType::Pcsc, "%s: atr: [ `%s' ]", __FUNCTION__, str.c_str());
        }

        if(state.dwEventState & SCARD_STATE_CHANGED) {
            assertm(readerName.size() < sizeof(rd.name), "reader name invalid");
            assertm(state.cbAtr <= sizeof(rd.atr), "atr length invalid");
            rd.state = state.dwEventState & SCARD_STATE_PRESENT ? (PcscLite::StatePresent | PcscLite::StatePowered |
                       PcscLite::StateNegotiable) : PcscLite::StateAbsent;
            std::copy_n(readerName.data(), readerName.size(), rd.name);
            std::copy_n(state.rgbAtr, state.cbAtr, rd.atr);
            rd.atrLen = state.cbAtr;

            if(changed) {
                *changed = true;
            }
        }

        return SCARD_S_SUCCESS;
    }

    uint32_t PcscLocal::syncReaders(bool* changed) {
        std::list<std::string> names;

        if(auto ptr = remote.lock()) {
            names = ptr->sendListReaders(id(), remoteContext);
        } else {
            Application::error("%s: no service", __FUNCTION__);
            return SCARD_E_NO_SERVICE;
        }

        if(names.empty()) {
            Application::warning("%s: no readers available", __FUNCTION__);

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
            auto it = std::find_if(PcscLite::readers.begin(), PcscLite::readers.end(),
            [&name](auto & rd) {
                return 0 == name.compare(rd.name);
            });

            // not found, add new
            if(it == PcscLite::readers.end()) {
                Application::debug(DebugType::Pcsc, "%s: added reader, name: `%s'", __FUNCTION__, name.c_str());
                // find unused slot
                auto rd = std::find_if(PcscLite::readers.begin(), PcscLite::readers.end(),
                [](auto & rd) {
                    return 0 == rd.name[0];
                });

                if(rd == PcscLite::readers.end()) {
                    LTSM::Application::error("%s: failed, %s", __FUNCTION__, "all slots is busy");
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
        sock.reset();
    }

    /// PcscSessionBus
    PcscSessionBus::PcscSessionBus(sdbus::IConnection & conn, bool debug) : ApplicationLog("ltsm_session_pcsc"),
#ifdef SDBUS_2_0_API
        AdaptorInterfaces(conn, sdbus::ObjectPath {dbus_session_pcsc_path})
#else
        AdaptorInterfaces(conn, dbus_session_pcsc_path)
#endif
    {
        registerAdaptor();

        if(debug) {
            Application::setDebugLevel(DebugLevel::Debug);
        }
    }

    PcscSessionBus::~PcscSessionBus() {
        unregisterAdaptor();

        if(std::filesystem::is_socket(pcscSocketPath)) {
            std::filesystem::remove(pcscSocketPath);
        }
    }

    int PcscSessionBus::start(void) {
        Application::info("%s: uid: %d, pid: %d, version: %d", __FUNCTION__, getuid(), getpid(), LTSM_SESSION_PCSC_VERSION);

        if(auto envSockName = getenv("PCSCLITE_CSOCK_NAME")) {
            pcscSocketPath = envSockName;
        }

        if(pcscSocketPath.empty()) {
            Application::error("%s: environment not found: %s", __FUNCTION__, "PCSCLITE_CSOCK_NAME");
            return EXIT_FAILURE;
        }

        Application::info("%s: socket path: `%s'", __FUNCTION__, pcscSocketPath.c_str());

        if(std::filesystem::is_socket(pcscSocketPath)) {
            std::filesystem::remove(pcscSocketPath);
            Application::warning("%s: socket found: %s", __FUNCTION__, pcscSocketPath.c_str());
        }

        signal(SIGTERM, signalHandler);
        signal(SIGINT, signalHandler);
        int socketFd = UnixSocket::listen(pcscSocketPath, 50);

        if(0 > socketFd) {
            Application::error("%s: socket failed", __FUNCTION__);
            return EXIT_FAILURE;
        }

        auto thrAccept = std::thread([this, socketFd]() {
            while(true) {
                auto sock = UnixSocket::accept(socketFd);

                if(0 > sock) {
                    break;
                }

                Application::debug(DebugType::App, "%s: add clientId: %" PRId32, "PcscSessionBusThread", sock);

                const std::scoped_lock guard{ clientsLock };
                this->clients.emplace_front(sock, remote, this);
            }
        });

        // main loop
        conn->enterEventLoop();
        Application::notice("%s: PCSC session shutdown", __FUNCTION__);

        // shutdown
        shutdown(socketFd, SHUT_RDWR);
        close(socketFd);

        transLock.unlock();

        if(thrAccept.joinable()) {
            thrAccept.join();
        }

        return EXIT_SUCCESS;
    }

    uint64_t PcscSessionBus::clientCanceledNotify(uint32_t ctx) {
        const std::scoped_lock guard{ clientsLock };
        auto it = std::find_if(clients.begin(), clients.end(), [&ctx](auto & cl) {
            return 0 <= cl.id() && cl.localContext() == ctx;
        });

        if(it != clients.end()) {
            Application::debug(DebugType::App, "%s: calceled clientId: %" PRId32, __FUNCTION__, it->id());
            it->canceledAction();
            return it->proxyContext();
        }

        return 0;
    }

    void PcscSessionBus::clientShutdownNotify(const PcscLocal* cli) {
        Application::debug(DebugType::App, "%s: shutdown clientId: %" PRId32, __FUNCTION__, cli->id());

        const std::scoped_lock guard{ clientsLock };
        this->clients.remove_if([cli](auto & st) {
            return cli == std::addressof(st);
        });
    }

    int32_t PcscSessionBus::getVersion(void) {
        Application::debug(DebugType::Dbus, "%s", __FUNCTION__);
        return LTSM_SESSION_PCSC_VERSION;
    }

    void PcscSessionBus::serviceShutdown(void) {
        Application::debug(DebugType::Dbus, "%s: pid: %d", __FUNCTION__, getpid());
        conn->leaveEventLoop();
    }

    void PcscSessionBus::setDebug(const std::string & level) {
        Application::debug(DebugType::Dbus, "%s: level: %s", __FUNCTION__, level.c_str());
        setDebugLevel(level);
    }

    bool PcscSessionBus::connectChannel(const std::string & clientPath) {
        Application::debug(DebugType::Dbus, "%s: client socket path: `%s'", __FUNCTION__, clientPath.c_str());

        bool waitSocket = Tools::waitCallable<std::chrono::milliseconds>(5000, 100, [ &]() {
            return Tools::checkUnixSocket(clientPath);
        });

        if(! waitSocket) {
            Application::error("%s: checkUnixSocket failed, `%s'", __FUNCTION__, clientPath.c_str());
            return false;
        }

        int sockfd = UnixSocket::connect(clientPath);

        if(0 > sockfd) {
            return false;
        }

        remote = std::make_shared<PcscRemote>(sockfd);
        return true;
    }

    void PcscSessionBus::disconnectChannel(const std::string & clientPath) {
        Application::debug(DebugType::Dbus, "%s: client socket path: `%s'", __FUNCTION__, clientPath.c_str());
        remote.reset();
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
        LTSM::conn = sdbus::createSessionBusConnection(sdbus::ServiceName {LTSM::dbus_session_pcsc_name});
#else
        LTSM::conn = sdbus::createSessionBusConnection(LTSM::dbus_session_pcsc_name);
#endif

        if(! LTSM::conn) {
            std::cerr << "dbus connection failed, uid: " << getuid() << std::endl;
            return EXIT_FAILURE;
        }

        LTSM::PcscSessionBus pcscSession(*LTSM::conn, debug);
        return pcscSession.start();
    } catch(const sdbus::Error & err) {
        LTSM::Application::error("sdbus: [%s] %s", err.getName().c_str(), err.getMessage().c_str());
    } catch(const std::exception & err) {
        LTSM::Application::error("%s: exception: %s", NS_FuncName.c_str(), err.what());
    }

    return EXIT_FAILURE;
}
