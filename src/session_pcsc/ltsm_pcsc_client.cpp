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

#include "pcsclite.h"
#include "winscard.h"

#include "ltsm_pcsc.h"
#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_channels.h"
#include "ltsm_application.h"

namespace LTSM
{
    namespace Channel
    {
        namespace Connector
        {
            // channel_system.cpp
            void loopWriter(ConnectorBase*, Remote2Local*);
            void loopReader(ConnectorBase*, Local2Remote*);
        }
    }
}

using namespace std::chrono_literals;

// createClientPcscConnector
std::unique_ptr<LTSM::Channel::ConnectorBase> LTSM::Channel::createClientPcscConnector(uint8_t channel,
        const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(),
                      Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "pcsc mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<ConnectorClientPcsc>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientPcsc
LTSM::Channel::ConnectorClientPcsc::ConnectorClientPcsc(uint8_t ch, const std::string & url, const ConnectorMode & mod,
        const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), cid(ch)
{
    Application::info("%s: channelId: %" PRIu8, __FUNCTION__, cid);
    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientPcsc::~ConnectorClientPcsc()
{
    setRunning(false);
}

int LTSM::Channel::ConnectorClientPcsc::error(void) const
{
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientPcsc::channel(void) const
{
    return cid;
}

void LTSM::Channel::ConnectorClientPcsc::setSpeed(const Channel::Speed & speed)
{
}

void LTSM::Channel::ConnectorClientPcsc::pushData(std::vector<uint8_t> && recv)
{
    Application::trace("%s: data size: %u", __FUNCTION__, recv.size());
    StreamBufRef sb;

    if(last.empty())
    {
        sb.reset(recv.data(), recv.size());
    }
    else
    {
        last.insert(last.end(), recv.begin(), recv.end());
        recv.swap(last);
        sb.reset(recv.data(), recv.size());
        last.clear();
    }

    if(4 < sb.last())
    {
        // pcsc stream format:
        // <CMD16> - pcsc init
        // <CMD16> - pcsc cmd
        // <DATA> - pcsc data
        auto pcscInit = sb.readIntLE16();

        if(pcscInit == PcscOp::Init)
        {
            auto pcscCmd = sb.readIntLE16();
            Application::debug("%s: cmd: 0x%" PRIx16, __FUNCTION__, pcscCmd);

            switch(pcscCmd)
            {
                case PcscLite::EstablishContext:
                    return pcscEstablishContext(sb);

                case PcscLite::ReleaseContext:
                    return pcscReleaseContext(sb);

                case PcscLite::ListReaders:
                    return pcscListReaders(sb);

                case PcscLite::Connect:
                    return pcscConnect(sb);

                case PcscLite::Reconnect:
                    return pcscReconnect(sb);

                case PcscLite::Disconnect:
                    return pcscDisconnect(sb);

                case PcscLite::BeginTransaction:
                    return pcscBeginTransaction(sb);

                case PcscLite::EndTransaction:
                    return pcscEndTransaction(sb);

                case PcscLite::Transmit:
                    return pcscTransmit(sb);

                case PcscLite::Status:
                    return pcscStatus(sb);

                case PcscLite::GetStatusChange:
                    return pcscGetStatusChange(sb);

                case PcscLite::Control:
                    return pcscControl(sb);

                case PcscLite::Cancel:
                    return pcscCancel(sb);

                case PcscLite::GetAttrib:
                    return pcscGetAttrib(sb);

                case PcscLite::SetAttrib:
                    return pcscSetAttrib(sb);

                default:
                    break;
            }

            Application::error("%s: %s failed, cmd: 0x%" PRIx16 ", recv size: %u",
                               __FUNCTION__, "pcsc", pcscInit, pcscCmd, recv.size());
        }

        Application::error("%s: %s failed, op: 0x%" PRIx16 ", recv size: %u",
                           __FUNCTION__, "pcsc", pcscInit, recv.size());
        throw channel_error(NS_FuncName);
    }
    else
    {
        Application::error("%s: %s failed, recv size: %u", __FUNCTION__, "data", recv.size());
    }
}

void LTSM::Channel::ConnectorClientPcsc::pcscEstablishContext(const StreamBufRef & sb)
{
    uint32_t scope = sb.readIntLE32();
    Application::info("%s: dwScope: %" PRIu32, __FUNCTION__, scope);
    SCARDCONTEXT hContext = 0;
    LONG ret = SCardEstablishContext(scope, nullptr, nullptr, & hContext);

    if(ret == SCARD_S_SUCCESS)
    {
        Application::debug("%s: context: %" PRIx64, __FUNCTION__, hContext);
    }
    else
    {
        Application::error("%s: return code: 0x%08" PRIx32, __FUNCTION__, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE64(hContext).writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscReleaseContext(const StreamBufRef & sb)
{
    SCARDCONTEXT hContext = sb.readIntLE64();
    Application::info("%s: context: %" PRIx64, __FUNCTION__, hContext);
    LONG ret = SCardReleaseContext(hContext);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: context: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hContext, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

std::list<std::string> getListReaders(SCARDCONTEXT hContext)
{
    DWORD readersLength = 0;
    LONG ret = SCardListReaders(hContext, nullptr, nullptr, & readersLength);

    if(ret == SCARD_E_NO_READERS_AVAILABLE)
        return {};

    if(ret != SCARD_S_SUCCESS)
    {
        LTSM::Application::error("%s: context: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hContext, ret);
        return {};
    }

    std::list<std::string> readers;
    auto readersBuf = std::make_unique<char[]>(readersLength);
    ret = SCardListReaders(hContext, nullptr, readersBuf.get(), & readersLength);

    if(ret == SCARD_S_SUCCESS)
    {
        auto it = readersBuf.get();

        while(*it)
        {
            readers.emplace_back(it);
            it += strnlen(it, std::min((size_t)MAX_READERNAME, readersLength)) + 1;
        }
    }

    return readers;
}

void LTSM::Channel::ConnectorClientPcsc::pcscListReaders(const StreamBufRef & sb)
{
    SCARDCONTEXT hContext = sb.readIntLE64();
    Application::info("%s: context: %" PRIx64, __FUNCTION__, hContext);
    auto readers = getListReaders(hContext);
    // reply
    StreamBuf reply(256);
    reply.writeIntLE32(readers.size());

    for(auto & reader : readers)
    {
        reply.writeIntLE32(reader.size()).write(reader);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscConnect(const StreamBufRef & sb)
{
    SCARDCONTEXT hContext = sb.readIntLE64();
    uint32_t shareMode = sb.readIntLE32();
    uint32_t prefferedProtocols = sb.readIntLE32();
    uint32_t len = sb.readIntLE32();
    auto readerName = sb.readString(len);
    Application::info("%s: context: %" PRIx64 ", readerName: `%s', shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32,
                      __FUNCTION__, hContext, readerName.c_str(), shareMode, prefferedProtocols);
    SCARDHANDLE hCard = 0;
    DWORD activeProtocol = 0;
    LONG ret = SCardConnect(hContext, readerName.c_str(), shareMode, prefferedProtocols, & hCard, & activeProtocol);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: context: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hContext, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE64(hCard).writeIntLE32(activeProtocol).writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscReconnect(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t shareMode = sb.readIntLE32();
    uint32_t prefferedProtocols = sb.readIntLE32();
    uint32_t initialization = sb.readIntLE32();
    Application::info("%s: handle: %" PRIx64 ", shareMode: %" PRIu32 ", prefferedProtocols: %" PRIu32 ", initialization: %"
                      PRIu32,
                      __FUNCTION__, hCard, shareMode, prefferedProtocols, initialization);
    DWORD activeProtocol = 0;
    LONG ret = SCardReconnect(hCard, shareMode, prefferedProtocols, initialization, & activeProtocol);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(activeProtocol).writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscDisconnect(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t disposition = sb.readIntLE32();
    Application::info("%s: handle: %" PRIx64 ", disposition: %" PRIu32,
                      __FUNCTION__, hCard, disposition);
    LONG ret = SCardDisconnect(hCard, disposition);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscBeginTransaction(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    Application::info("%s: handle: %" PRIx64,
                      __FUNCTION__, hCard);
    LONG ret = SCardBeginTransaction(hCard);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscEndTransaction(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t disposition = sb.readIntLE32();
    Application::info("%s: handle: %" PRIx64 ", disposition: %" PRIu32,
                      __FUNCTION__, hCard, disposition);
    LONG ret = SCardEndTransaction(hCard, disposition);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscTransmit(const StreamBufRef & sb)
{
    SCARD_IO_REQUEST ioSendPci, ioRecvPci;
    SCARDHANDLE hCard = sb.readIntLE64();
    ioSendPci.dwProtocol = sb.readIntLE32();
    ioSendPci.cbPciLength = sb.readIntLE32();
    uint32_t sendLength = sb.readIntLE32();
    auto sendBuffer = sb.read(sendLength);
    Application::info("%s: handle: %" PRIx64 ", dwProtocol: %" PRIu32 ", pciLength: %" PRIu32 ", send size: %" PRIu32,
                      __FUNCTION__, hCard, ioSendPci.dwProtocol, ioSendPci.cbPciLength, sendLength);
    DWORD recvLength = MAX_BUFFER_SIZE_EXTENDED;
    std::vector<BYTE> recvBuffer(recvLength);
    LONG ret = SCardTransmit(hCard, & ioSendPci, sendBuffer.data(), sendBuffer.size(),
                             & ioRecvPci, recvBuffer.data(), & recvLength);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16 + recvLength);
    reply.writeIntLE32(ioRecvPci.dwProtocol).writeIntLE32(ioRecvPci.cbPciLength).writeIntLE32(recvLength).writeIntLE32(ret);

    if(recvLength)
    {
        reply.write(recvBuffer.data(), recvLength);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscStatus(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    Application::info("%s: handle: %" PRIx64, __FUNCTION__, hCard);
    DWORD state = 0;
    DWORD protocol = 0;
    char readerName[MAX_READERNAME];
    DWORD readerNameLen = sizeof(readerName);
    BYTE atrBuf[MAX_ATR_SIZE];
    DWORD atrLen = sizeof(atrBuf);
    LONG ret = SCardStatus(hCard, readerName, & readerNameLen, & state, & protocol, atrBuf, & atrLen);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(20 + sizeof(readerName) + sizeof(atrBuf));
    reply.writeIntLE32(readerNameLen).write(readerName, readerNameLen);
    reply.writeIntLE32(state).writeIntLE32(protocol);
    reply.writeIntLE32(atrLen).write(atrBuf, atrLen);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscGetStatusChange(const StreamBufRef & sb)
{
    SCARDHANDLE hContext = sb.readIntLE64();
    uint32_t timeout = sb.readIntLE32();
    uint32_t statesCount = sb.readIntLE32();
    std::vector<SCARD_READERSTATE> states(statesCount, SCARD_READERSTATE{});

    for(auto & state : states)
    {
        auto str = new std::string;
        auto len = sb.readIntLE32();
        str->assign(sb.readString(len));
        state.szReader = str->c_str();
        state.pvUserData = str;
        state.dwCurrentState = sb.readIntLE32();
        state.dwEventState = 0;
        state.cbAtr = sb.readIntLE32();
        assertm(state.cbAtr <= sizeof(state.rgbAtr), "atr length invalid");

        if(state.cbAtr)
        {
            sb.readTo(state.rgbAtr, state.cbAtr);
        }
    }

    Application::info("%s: context: %" PRIx64 ", timeout: %" PRIu32, __FUNCTION__, hContext, timeout);
    LONG ret = SCardGetStatusChange(hContext, timeout, states.data(), states.size());

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: context: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hContext, ret);
    }

    // reply
    StreamBuf reply(1024);
    reply.writeIntLE32(statesCount).writeIntLE32(ret);

    for(auto & state : states)
    {
        reply.writeIntLE32(state.dwCurrentState);
        reply.writeIntLE32(state.dwEventState);
        reply.writeIntLE32(state.cbAtr);
        reply.write(state.rgbAtr, state.cbAtr);
        auto name = static_cast<const std::string*>(state.pvUserData);
        delete name;
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscControl(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t controlCode = sb.readIntLE32();
    uint32_t sendLength = sb.readIntLE32();
    uint32_t recvLength = sb.readIntLE32();
    auto sendBuffer = sb.read(sendLength);
    Application::info("%s: handle: %" PRIx64 ", controlCode: 0x%08" PRIx32 ", send size: %" PRIu32 ", recv size: %" PRIu32,
                      __FUNCTION__, hCard, controlCode, sendLength, recvLength);
    DWORD bytesReturned = 0;
    std::vector<BYTE> recvBuffer(recvLength ? recvLength : MAX_BUFFER_SIZE_EXTENDED);
    LONG ret = SCardControl(hCard, controlCode, sendBuffer.data(), sendLength,
                            recvBuffer.data(), recvLength, & bytesReturned);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(bytesReturned).writeIntLE32(ret);

    if(bytesReturned)
    {
        reply.write(recvBuffer.data(), bytesReturned);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscCancel(const StreamBufRef & sb)
{
    SCARDHANDLE hContext = sb.readIntLE64();
    Application::info("%s: context: %" PRIx64, __FUNCTION__, hContext);
    LONG ret = SCardCancel(hContext);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: context: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hContext, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscGetAttrib(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t attrId = sb.readIntLE32();
    Application::info("%s: handle: %" PRIx64 ", attrId: %" PRIu32, __FUNCTION__, hCard, attrId);
    std::vector<BYTE> attrBuf(MAX_BUFFER_SIZE);
    DWORD attrLen = MAX_BUFFER_SIZE;
    LONG ret = SCardGetAttrib(hCard, attrId, attrBuf.data(), & attrLen);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(8 + attrLen);
    reply.writeIntLE32(attrLen).writeIntLE32(ret);

    if(attrLen)
    {
        reply.write(attrBuf.data(), attrLen);
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
}

void LTSM::Channel::ConnectorClientPcsc::pcscSetAttrib(const StreamBufRef & sb)
{
    SCARDHANDLE hCard = sb.readIntLE64();
    uint32_t attrId = sb.readIntLE32();
    uint32_t attrLen = sb.readIntLE32();
    auto attrBuf = sb.read(attrLen);
    Application::info("%s: handle: %" PRIx64 ", attrId: %" PRIu32 ", attrLen: %" PRIu32, __FUNCTION__, hCard, attrId,
                      attrLen);
    LONG ret = SCardSetAttrib(hCard, attrId, attrBuf.data(), attrLen);

    if(ret != SCARD_S_SUCCESS)
    {
        Application::error("%s: handle: %" PRIx64 ", return code: 0x%08" PRIx32, __FUNCTION__, hCard, ret);
    }

    // reply
    StreamBuf reply(16);
    reply.writeIntLE32(ret);
    owner->sendLtsmEvent(cid, reply.rawbuf());
}
