/***************************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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
 **************************************************************************/

#include <chrono>
#include <string>
#include <memory>
#include <cstring>
#include <filesystem>
#include <forward_list>

#ifdef __UNIX__
#include "pcsclite.h"
#endif

#ifdef __WIN32__
#define MAX_ATR_SIZE    33
#define MAX_READERNAME  128
#define MAX_BUFFER_SIZE 264
#define MAX_BUFFER_SIZE_EXTENDED (4 + 3 + (1<<16) + 3 + 2)

const char* pcsc_stringify_error(int err);
#endif

#ifdef __APPLE__
#include "PCSC/wintypes.h"
#include "PCSC/pcsclite.h"
#include "PCSC/winscard.h"
#else
#include "winscard.h"
#endif

#include "ltsm_pcsc.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_channels.h"
#include "ltsm_application.h"

namespace PcscLite {
    const char* commandName(uint16_t cmd) {
        switch(cmd) {
            case EstablishContext:
                return "EstablishContext";

            case ReleaseContext:
                return "ReleaseContext";

            case ListReaders:
                return "ListReaders";

            case Connect:
                return "Connect";

            case Reconnect:
                return "Reconnect";

            case Disconnect:
                return "Disconnect";

            case BeginTransaction:
                return "BeginTransaction";

            case EndTransaction:
                return "EndTransaction";

            case Transmit:
                return "Transmit";

            case Control:
                return "Control";

            case Status:
                return "Status";

            case GetStatusChange:
                return "GetStatusChange";

            case Cancel:
                return "Cancel";

            case CancelTransaction:
                return "CancelTransaction";

            case GetAttrib:
                return "GetAttrib";

            case SetAttrib:
                return "SetAttrib";

            case GetVersion:
                return "GetVersion";

            case GetReaderState:
                return "GetReaderState";

            case WaitReaderStateChangeStart:
                return "WaitReaderStateChangeStart";

            case WaitReaderStateChangeStop:
                return "WaitReaderStateChangeStop";

            default:
                break;
        }

        return "Unknown";
    }
}

namespace LTSM {
    namespace Channel {
        namespace Connector {
            // channel_system.cpp
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);
        }
    }

}

using namespace std::chrono_literals;

// createClientPcscConnector
std::unique_ptr<LTSM::Channel::ConnectorBase> LTSM::Channel::createClientPcscConnector(uint8_t channel,
        const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender) {
    Application::info("{}: id: {}, url: `{}', mode: {}", NS_FuncNameV, channel, url,
                      Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown) {
        Application::error("{}: {}, mode: {}", NS_FuncNameV, "pcsc mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncNameS);
    }

    return std::make_unique<ConnectorClientPcsc>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientPcsc
LTSM::Channel::ConnectorClientPcsc::ConnectorClientPcsc(uint8_t ch, const std::string & url, const ConnectorMode & mod,
        const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), cid(ch) {
    Application::info("{}: channelId: {}", NS_FuncNameV, cid);
    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientPcsc::~ConnectorClientPcsc() {
    setRunning(false);
}

int LTSM::Channel::ConnectorClientPcsc::error(void) const {
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientPcsc::channel(void) const {
    return cid;
}

void LTSM::Channel::ConnectorClientPcsc::setSpeed(const Channel::Speed & speed) {
}

void LTSM::Channel::ConnectorClientPcsc::pushData(std::vector<uint8_t> && recv) {
    Application::trace(DebugType::Pcsc, "{}: data size: {}", NS_FuncNameV, recv.size());
    StreamBufRef sb;

    if(last.empty()) {
        sb.reset(recv.data(), recv.size());
    } else {
        std::ranges::copy(recv, std::back_inserter(last));
        recv.swap(last);
        sb.reset(recv.data(), recv.size());
        last.clear();
    }

    const uint8_t* beginPacket = nullptr;
    const uint8_t* endPacket = nullptr;

    try {
        while(4 <= sb.last()) {
            beginPacket = sb.data();
            endPacket = beginPacket + sb.last();

            // pcsc stream format:
            // <CMD16> - pcsc init
            // <CMD16> - pcsc cmd
            // <DATA> - pcsc data
            auto pcscCmd = sb.readIntLE16();

            if(pcscCmd == PcscOp::Init) {
                if(! pcscOpInit(sb)) {
                    throw channel_error(NS_FuncNameS);
                }
            } else if(pcscCmd == PcscOp::Lite) {
                auto liteCmd = sb.readIntLE16();
                pcscLiteCommand(liteCmd, sb);
                //
            } else {
                Application::error("{}: {} failed, cmd: {:#06x}, recv size: {}",
                                   NS_FuncNameV, "pcsc init", pcscCmd, recv.size());
                throw channel_error(NS_FuncNameS);
            }
        }

        if(sb.last()) {
            throw std::underflow_error(NS_FuncNameS);
        }
    } catch(const std::underflow_error & err) {
        Application::warning("{}: underflow data: {}, func: {}", NS_FuncNameV, sb.last(), err.what());

        if(beginPacket) {
            last.assign(beginPacket, endPacket);
        } else {
            last.swap(recv);
        }
    }
}

bool LTSM::Channel::ConnectorClientPcsc::pcscOpInit(const StreamBufRef & sb) {
    // <VER16> - proto ver
    if(2 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto protoVer = sb.readIntLE16();

    if(protoVer != PcscOp::ProtoVer) {
        Application::error("{}: unsupported version: {}", NS_FuncNameV, protoVer);
        throw pcsc_error(NS_FuncNameS);
    }

    Application::info("{}: server proto version: {}", NS_FuncNameV, protoVer);

    // reply
    StreamBuf reply(8);
    reply.writeIntLE16(PcscOp::Init);

    // no errors
    reply.writeIntLE16(0);
    // proto ver
    reply.writeIntLE16(PcscOp::ProtoVer);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));

    return true;

}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteCommand(uint16_t cmd, const StreamBufRef & sb) {
    Application::debug(DebugType::Pcsc, "{}: cmd: {} ({:#06x})", NS_FuncNameV, PcscLite::commandName(cmd), cmd);

    switch(cmd) {
        case PcscLite::EstablishContext:
            return pcscLiteEstablishContext(sb);

        case PcscLite::ReleaseContext:
            return pcscLiteReleaseContext(sb);

        case PcscLite::ListReaders:
            return pcscLiteListReaders(sb);

        case PcscLite::Connect:
            return pcscLiteConnect(sb);

        case PcscLite::Reconnect:
            return pcscLiteReconnect(sb);

        case PcscLite::Disconnect:
            return pcscLiteDisconnect(sb);

        case PcscLite::BeginTransaction:
            return pcscLiteBeginTransaction(sb);

        case PcscLite::EndTransaction:
            return pcscLiteEndTransaction(sb);

        case PcscLite::Transmit:
            return pcscLiteTransmit(sb);

        case PcscLite::Status:
            return pcscLiteStatus(sb);

        case PcscLite::GetStatusChange:
            return pcscLiteGetStatusChange(sb);

        case PcscLite::Control:
            return pcscLiteControl(sb);

        case PcscLite::Cancel:
            return pcscLiteCancel(sb);

        case PcscLite::GetAttrib:
            return pcscLiteGetAttrib(sb);

        case PcscLite::SetAttrib:
            return pcscLiteSetAttrib(sb);

        default:
            break;
    }

    Application::error("{}: {} failed, cmd: {:#06x}, last size: {}",
                       NS_FuncNameV, "pcsc", cmd, sb.last());
    throw channel_error(NS_FuncNameS);
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteEstablishContext(const StreamBufRef & sb) {
    if(4 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    uint32_t scope = sb.readIntLE32();
    Application::debug(DebugType::Pcsc, "{}: << dwScope: {}", NS_FuncNameV, scope);
    SCARDCONTEXT hContext = 0;
    uint32_t ret = SCardEstablishContext(scope, nullptr, nullptr, & hContext);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> context: {:#018x}", NS_FuncNameV, hContext);
    } else {
        Application::error("{}: error: {:#010x} ({})", NS_FuncNameV, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE64(hContext).writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteReleaseContext(const StreamBufRef & sb) {
    if(8 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDCONTEXT hContext = sb.readIntLE64();
    Application::debug(DebugType::Pcsc, "{}: << context: {:#018x}", NS_FuncNameV, hContext);
    uint32_t ret = SCardReleaseContext(hContext);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

std::list<std::string> getListReaders(SCARDCONTEXT hContext) {
    DWORD readersLength = 0;
#ifdef __WIN32__
    uint32_t ret = SCardListReadersA(hContext, nullptr, nullptr, & readersLength);
#else
    uint32_t ret = SCardListReaders(hContext, nullptr, nullptr, & readersLength);
#endif

    if(ret == SCARD_E_NO_READERS_AVAILABLE)
        return {};

    if(ret != SCARD_S_SUCCESS) {
        LTSM::Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
        return {};
    }

    auto readersBuf = std::make_unique<char[]>(readersLength);
#ifdef __WIN32__
    ret = SCardListReadersA(hContext, nullptr, readersBuf.get(), & readersLength);
#else
    ret = SCardListReaders(hContext, nullptr, readersBuf.get(), & readersLength);
#endif

    if(ret == SCARD_S_SUCCESS) {
        std::list<std::string> readers;
        auto it1 = readersBuf.get();

        while(*it1) {
            auto it2 = it1 + strnlen(it1, std::min((DWORD)MAX_READERNAME, readersLength));
            readers.emplace_back(it1, it2);
            it1 = std::next(it2);
        }

        return readers;
    }

    LTSM::Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
    return {};
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteListReaders(const StreamBufRef & sb) {
    if(8 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDCONTEXT hContext = sb.readIntLE64();
    Application::debug(DebugType::Pcsc, "{}: << context: {:#018x}", NS_FuncNameV, hContext);
    auto readers = getListReaders(hContext);
    // reply
    StreamBuf reply(256);
    reply.writeIntLE32(readers.size());
    Application::debug(DebugType::Pcsc, "{}: >> readers count: {}", NS_FuncNameV, readers.size());

    for(const auto & reader : readers) {
        Application::debug(DebugType::Pcsc, "{}: >> reader: `{}'", NS_FuncNameV, reader);
        reply.writeIntLE32(reader.size()).write(reader);
    }

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteConnect(const StreamBufRef & sb) {
    if(20 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDCONTEXT hContext = sb.readIntLE64();
    uint32_t shareMode = sb.readIntLE32();
    uint32_t prefferedProtocols = sb.readIntLE32();
    uint32_t len = sb.readIntLE32();

    if(len > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto readerName = sb.readString(len);
    Application::debug(DebugType::Pcsc, "{}: << context: {:#018x}, readerName: `{}', shareMode: {}, prefferedProtocols: {}",
                       NS_FuncNameV, hContext, readerName, shareMode, prefferedProtocols);
    SCARDHANDLE hCard = 0;
    DWORD activeProtocol = 0;
#ifdef __WIN32__
    uint32_t ret = SCardConnectA(hContext, readerName.c_str(), shareMode, prefferedProtocols, & hCard, & activeProtocol);
#else
    uint32_t ret = SCardConnect(hContext, readerName.c_str(), shareMode, prefferedProtocols, & hCard, & activeProtocol);
#endif

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> handle: {:#018x}, activeProtocol: {}", NS_FuncNameV, hCard, activeProtocol);
    } else {
        Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE64(hCard).writeIntLE32(activeProtocol).writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteReconnect(const StreamBufRef & sb) {
    if(20 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t shareMode = sb.readIntLE32();
    uint32_t prefferedProtocols = sb.readIntLE32();
    uint32_t initialization = sb.readIntLE32();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, shareMode: {}, prefferedProtocols: {}, initialization: {}",
                       NS_FuncNameV, hCard, shareMode, prefferedProtocols, initialization);
    DWORD activeProtocol = 0;
    uint32_t ret = SCardReconnect(hCard, shareMode, prefferedProtocols, initialization, & activeProtocol);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> activeProtocol: {}", NS_FuncNameV, activeProtocol);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(activeProtocol).writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteDisconnect(const StreamBufRef & sb) {
    if(12 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t disposition = sb.readIntLE32();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, disposition: {}",
                       NS_FuncNameV, hCard, disposition);
    uint32_t ret = SCardDisconnect(hCard, disposition);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteBeginTransaction(const StreamBufRef & sb) {
    if(8 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}",
                       NS_FuncNameV, hCard);
    uint32_t ret = SCardBeginTransaction(hCard);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteEndTransaction(const StreamBufRef & sb) {
    if(12 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t disposition = sb.readIntLE32();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, disposition: {}",
                       NS_FuncNameV, hCard, disposition);
    uint32_t ret = SCardEndTransaction(hCard, disposition);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteTransmit(const StreamBufRef & sb) {
    if(24 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARD_IO_REQUEST ioSendPci = {};
    SCARD_IO_REQUEST ioRecvPci = {};
    SCARDHANDLE hCard = sb.readIntLE64();
    ioSendPci.dwProtocol = sb.readIntLE32();
    ioSendPci.cbPciLength = sb.readIntLE32();
    DWORD recvLength = sb.readIntLE32();
    uint32_t sendLength = sb.readIntLE32();

    if(sendLength > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto sendBuffer = sb.read(sendLength);
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, dwProtocol: {}, pciLength: {}, send size: {}, recv size: {}",
                       NS_FuncNameV, hCard, ioSendPci.dwProtocol, ioSendPci.cbPciLength, sendLength, recvLength);

    std::vector<BYTE> recvBuffer(recvLength ? recvLength : MAX_BUFFER_SIZE_EXTENDED);
    uint32_t ret = SCardTransmit(hCard, & ioSendPci, sendBuffer.data(), sendBuffer.size(),
                                 & ioRecvPci, recvBuffer.data(), & recvLength);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> dwProtocol: {}, pciLength: {}, recv size: {}",
                           NS_FuncNameV, ioRecvPci.dwProtocol, ioRecvPci.cbPciLength, recvLength);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16 + recvLength);
    reply.writeIntLE32(ioRecvPci.dwProtocol).writeIntLE32(ioRecvPci.cbPciLength).writeIntLE32(recvLength).writeIntLE32(ret);

    if(recvLength) {
        reply.write(recvBuffer.data(), recvLength);
    }

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteStatus(const StreamBufRef & sb) {
    if(8 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}", NS_FuncNameV, hCard);
    DWORD state = 0;
    DWORD protocol = 0;
    char readerName[MAX_READERNAME] = {};
    DWORD readerNameLen = sizeof(readerName);
    BYTE atrBuf[MAX_ATR_SIZE];
    DWORD atrLen = sizeof(atrBuf);
#ifdef __WIN32__
    uint32_t ret = SCardStatusA(hCard, readerName, & readerNameLen, & state, & protocol, atrBuf, & atrLen);
#else
    uint32_t ret = SCardStatus(hCard, readerName, & readerNameLen, & state, & protocol, atrBuf, & atrLen);
#endif

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> readerName: `{:.{}}', state: {:#010x}, protocol: {}, atrLen: {}",
                           NS_FuncNameV, readerName, readerNameLen, state, protocol, atrLen);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    readerNameLen = strlen(readerName);
    StreamBuf reply(20 + sizeof(readerName) + sizeof(atrBuf));

    reply.writeIntLE32(state).writeIntLE32(protocol).
        writeIntLE32(readerNameLen).writeIntLE32(atrLen).writeIntLE32(ret);
    reply.write(readerName, readerNameLen).write(atrBuf, atrLen);

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteGetStatusChange(const StreamBufRef & sb) {
    if(16 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hContext = sb.readIntLE64();
    uint32_t timeout = sb.readIntLE32();
    uint32_t statesCount = sb.readIntLE32();

    std::vector<SCARD_READERSTATE> states(statesCount, SCARD_READERSTATE{});
    std::forward_list<std::string> names;

    if(statesCount * 12 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    for(auto & state : states) {
        if(12 > sb.last()) {
            throw std::underflow_error(NS_FuncNameS);
        }

        auto szReader = sb.readIntLE32();

        state.dwCurrentState = sb.readIntLE32();
        state.dwEventState = 0;
        state.szReader = nullptr;
        state.pvUserData = nullptr;
        state.cbAtr = sb.readIntLE32();
        assertm(state.cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

        if(szReader + state.cbAtr > sb.last()) {
            throw std::underflow_error(NS_FuncNameS);
        }

        if(szReader) {
            names.emplace_front(sb.readString(szReader));
            state.szReader = names.front().c_str();
            state.pvUserData = std::addressof(names.front());
        }

        if(state.cbAtr) {
            sb.readTo(state.rgbAtr, state.cbAtr);
        }
    }

    Application::debug(DebugType::Pcsc, "{}: << context: {:#018x}, timeout: {}, states count: {}", NS_FuncNameV, hContext, timeout, statesCount);
#ifdef __WIN32__
    uint32_t ret = SCardGetStatusChangeA(hContext, timeout, states.data(), states.size());
#else
    uint32_t ret = SCardGetStatusChange(hContext, timeout, states.data(), states.size());
#endif

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> statesCount: {}", NS_FuncNameV, statesCount);
    } else {
        Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(1024);
    reply.writeIntLE32(statesCount).writeIntLE32(ret);

    for(const auto & state : states) {
        reply.writeIntLE32(state.dwCurrentState);
        reply.writeIntLE32(state.dwEventState);

        auto szReader = static_cast<const std::string*>(state.pvUserData);
        reply.writeIntLE32(szReader ? szReader->size() : 0);
        reply.writeIntLE32(state.cbAtr);

        Application::debug(DebugType::Pcsc, "{}: >> reader: `{}', currentState: {:#010x}, eventState: {:#010x}, atrLen: {}",
                           NS_FuncNameV, state.szReader, state.dwCurrentState, state.dwEventState, state.cbAtr);

        if(szReader) {
            reply.write(*szReader);
        }

        if(state.cbAtr) {
            reply.write(state.rgbAtr, state.cbAtr);
        }
    }

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteControl(const StreamBufRef & sb) {
    if(20 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t controlCode = sb.readIntLE32();
    uint32_t sendLength = sb.readIntLE32();
    uint32_t recvLength = sb.readIntLE32();

    if(sendLength > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto sendBuffer = sb.read(sendLength);
    Application::debug(DebugType::Pcsc, "{}: handle: << {:#018x}, controlCode: {:#010x}, send size: {}, recv size: {}",
                       NS_FuncNameV, hCard, controlCode, sendLength, recvLength);
    DWORD bytesReturned = 0;
    std::vector<BYTE> recvBuffer(recvLength ? recvLength : MAX_BUFFER_SIZE_EXTENDED);
    uint32_t ret = SCardControl(hCard, controlCode, sendBuffer.data(), sendLength,
                                recvBuffer.data(), recvLength, & bytesReturned);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> bytesReturned: {}", NS_FuncNameV, bytesReturned);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(bytesReturned).writeIntLE32(ret);

    if(bytesReturned) {
        reply.write(recvBuffer.data(), bytesReturned);
    }

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteCancel(const StreamBufRef & sb) {
    if(8 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hContext = sb.readIntLE64();
    Application::debug(DebugType::Pcsc, "{}: << context: {:#018x}", NS_FuncNameV, hContext);
    uint32_t ret = SCardCancel(hContext);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: context: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hContext, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteGetAttrib(const StreamBufRef & sb) {
    if(12 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t attrId = sb.readIntLE32();
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, attrId: {}", NS_FuncNameV, hCard, attrId);
    std::vector<BYTE> attrBuf(MAX_BUFFER_SIZE);
    DWORD attrLen = MAX_BUFFER_SIZE;
    uint32_t ret = SCardGetAttrib(hCard, attrId, attrBuf.data(), & attrLen);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> attrLen: {}", NS_FuncNameV, attrLen);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(8 + attrLen);
    reply.writeIntLE32(attrLen).writeIntLE32(ret);

    if(attrLen) {
        reply.write(attrBuf.data(), attrLen);
    }

    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}

void LTSM::Channel::ConnectorClientPcsc::pcscLiteSetAttrib(const StreamBufRef & sb) {
    if(16 > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t attrId = sb.readIntLE32();
    uint32_t attrLen = sb.readIntLE32();

    if(attrLen > sb.last()) {
        throw std::underflow_error(NS_FuncNameS);
    }

    auto attrBuf = sb.read(attrLen);
    Application::debug(DebugType::Pcsc, "{}: << handle: {:#018x}, attrId: {}, attrLen: {}",
                       NS_FuncNameV, hCard, attrId, attrLen);
    uint32_t ret = SCardSetAttrib(hCard, attrId, attrBuf.data(), attrLen);

    if(ret == SCARD_S_SUCCESS) {
        Application::debug(DebugType::Pcsc, "{}: >> success", NS_FuncNameV);
    } else {
        Application::error("{}: handle: {:#018x}, error: {:#010x} ({})", NS_FuncNameV, hCard, ret, pcsc_stringify_error(ret));
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmChannelData(cid, std::move(reply.rawbuf()));
}
