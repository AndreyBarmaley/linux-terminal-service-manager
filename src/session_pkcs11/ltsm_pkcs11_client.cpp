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
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_pkcs11.h"
#include "ltsm_global.h"
#include "ltsm_channels.h"
#include "ltsm_application.h"
#include "ltsm_pkcs11_wrapper.h"

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

// createClientPkcs11Connector
std::unique_ptr<LTSM::Channel::ConnectorBase>
    LTSM::Channel::createClientPkcs11Connector(uint8_t channel, const std::string & url, const ConnectorMode & mode, const Opts & chOpts, ChannelClient & sender)
{
    Application::info("%s: id: %" PRId8 ", url: `%s', mode: %s", __FUNCTION__, channel, url.c_str(), Channel::Connector::modeString(mode));

    if(mode == ConnectorMode::Unknown)
    {
        Application::error("%s: %s, mode: %s", __FUNCTION__, "pkcs11 mode failed", Channel::Connector::modeString(mode));
        throw channel_error(NS_FuncName);
    }

    return std::make_unique<ConnectorClientPkcs11>(channel, url, mode, chOpts, sender);
}

/// ConnectorClientPkcs11
LTSM::Channel::ConnectorClientPkcs11::ConnectorClientPkcs11(uint8_t ch, const std::string & url, const ConnectorMode & mod, const Opts & chOpts, ChannelClient & srv)
    : ConnectorBase(ch, mod, chOpts, srv), reply(4096), cid(ch)
{
    Application::info("%s: channelId: %" PRIu8, __FUNCTION__, cid);

    // start threads
    setRunning(true);
}

LTSM::Channel::ConnectorClientPkcs11::~ConnectorClientPkcs11()
{
    setRunning(false);
}

int LTSM::Channel::ConnectorClientPkcs11::error(void) const
{
    return 0;
}

uint8_t LTSM::Channel::ConnectorClientPkcs11::channel(void) const
{
    return cid;
}

void LTSM::Channel::ConnectorClientPkcs11::setSpeed(const Channel::Speed & speed)
{
}

void LTSM::Channel::ConnectorClientPkcs11::pushData(std::vector<uint8_t> && recv)
{
    Application::debug("%s: size: %u", __FUNCTION__, recv.size());

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

    const uint8_t* beginPacket = nullptr;
    const uint8_t* endPacket = nullptr;

    try
    {
        while(2 < sb.last())
        {
            // pkcs11 stream format:
            // <CMD16> - audio cmd
            // <DATA> - audio data
            beginPacket = sb.data();
            endPacket = beginPacket + sb.last();

            auto pkcs11Cmd = sb.readIntLE16();
            Application::debug("%s: cmd: 0x%" PRIx16, __FUNCTION__, pkcs11Cmd);

            if(Pkcs11Op::Init == pkcs11Cmd)
                pkcs11Init(sb);
            else
            if(Pkcs11Op::GetSlots == pkcs11Cmd)
                pkcs11GetSlots(sb);
            else
            if(Pkcs11Op::GetSlotMechanisms == pkcs11Cmd)
                pkcs11GetSlotMechanisms(sb);
            else
            if(Pkcs11Op::GetSlotCertificates == pkcs11Cmd)
                pkcs11GetSlotCertificates(sb);
            else
            if(Pkcs11Op::SignData == pkcs11Cmd)
                pkcs11SignData(sb);
            else
            if(Pkcs11Op::DecryptData == pkcs11Cmd)
                pkcs11DecryptData(sb);
            else
            {
                Application::error("%s: %s failed, cmd: 0x%" PRIx16 ", recv size: %u", __FUNCTION__, "audio", pkcs11Cmd, recv.size());
                throw channel_error(NS_FuncName);
            }
        }

        if(sb.last())
            throw std::underflow_error(NS_FuncName);
    }
    catch(const std::underflow_error &)
    {
        Application::warning("%s: underflow data: %u", __FUNCTION__, sb.last());
        
        if(beginPacket)
            last.assign(beginPacket, endPacket);
        else
            last.swap(recv);
    }
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11Init(const StreamBufRef & sb)
{
    // pkcs11 format:
    // <VER16> - proto ver
            
    if(2 > sb.last())
        throw std::underflow_error(NS_FuncName);

    protoVer = sb.readIntLE16();

    reply.reset();

    // reply format:
    // <CMD16> - cmd id
    // <ERR16> - err len
    // <VER16> - proto version
    // <DATA> library info struct
    reply.writeIntLE16(Pkcs11Op::Init);

    try
    {
        pkcs11 = PKCS11::loadLibrary(owner->pkcs11Library());
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        std::string error = err.what();
        reply.writeIntLE16(error.size());
        reply.write(error);
        owner->sendLtsmEvent(cid, reply.rawbuf());
        return false;
    }

    auto info = pkcs11->getLibraryInfo();

    // no errors
    reply.writeIntLE16(0);
    // proto ver
    reply.writeIntLE16(1);
    // library info
    reply.writeInt8(info->cryptokiVersion.major);
    reply.writeInt8(info->cryptokiVersion.minor);
    reply.write(info->manufacturerID, 32);
    reply.writeIntLE64(info->flags);
    reply.write(info->libraryDescription, 32);
    reply.writeInt8(info->libraryVersion.major);
    reply.writeInt8(info->libraryVersion.minor);

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11GetSlots(const StreamBufRef & sb)
{
    if(1 > sb.last())
        throw std::underflow_error(NS_FuncName);

    // pkcs11 format:
    // <ONLY8> - with token only
    bool tokenPresentOnly = sb.readInt8();

    reply.reset();

    // reply format:
    // <CMD16> - cmd id
    // <LEN16> - slots count
    // <ID64>  - slot id
    // <DATA>  - slot info struct
    // <DATA>  - token info struct
    reply.writeIntLE16(Pkcs11Op::GetSlots);

    auto slots = PKCS11::getSlots(tokenPresentOnly, pkcs11);
    reply.writeIntLE16(slots.size());

    PKCS11::SlotInfo slotInfo;
    PKCS11::TokenInfo tokenInfo;

    for(auto & slot: slots)
    {
        reply.writeIntLE64(slot.slotId());

        if(slot.getSlotInfo(slotInfo))
        {
            reply.writeInt8(1);
            reply.write(slotInfo.slotDescription, 64);
            reply.write(slotInfo.manufacturerID, 32);
            reply.writeIntLE64(slotInfo.flags);
            reply.writeInt8(slotInfo.hardwareVersion.major);
            reply.writeInt8(slotInfo.hardwareVersion.minor);
            reply.writeInt8(slotInfo.firmwareVersion.major);
            reply.writeInt8(slotInfo.firmwareVersion.minor);
        }
        else
        {
            reply.writeInt8(0);
        }

        if(slot.getTokenInfo(tokenInfo))
        {
            reply.writeInt8(1);
            reply.write(tokenInfo.label, 32);
            reply.write(tokenInfo.manufacturerID, 32);
            reply.write(tokenInfo.model, 16);
            reply.write(tokenInfo.serialNumber, 16);
            reply.writeIntLE64(tokenInfo.flags);
            reply.writeIntLE64(tokenInfo.ulMaxSessionCount);
            reply.writeIntLE64(tokenInfo.ulSessionCount);
            reply.writeIntLE64(tokenInfo.ulMaxRwSessionCount);
            reply.writeIntLE64(tokenInfo.ulRwSessionCount);
            reply.writeIntLE64(tokenInfo.ulMaxPinLen);
            reply.writeIntLE64(tokenInfo.ulMinPinLen);
            reply.writeIntLE64(tokenInfo.ulTotalPublicMemory);
            reply.writeIntLE64(tokenInfo.ulFreePublicMemory);
            reply.writeIntLE64(tokenInfo.ulTotalPrivateMemory);
            reply.writeIntLE64(tokenInfo.ulFreePrivateMemory);
            reply.writeInt8(tokenInfo.hardwareVersion.major);
            reply.writeInt8(tokenInfo.hardwareVersion.minor);
            reply.writeInt8(tokenInfo.firmwareVersion.major);
            reply.writeInt8(tokenInfo.firmwareVersion.minor);
            reply.write(tokenInfo.utcTime, 16);
        }
        else
        {
            reply.writeInt8(0);
        }
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11GetSlotMechanisms(const StreamBufRef & sb)
{
    if(8 > sb.last())
        throw std::underflow_error(NS_FuncName);

    // pkcs11 format:
    // <SLOT64> - slot id
    auto slotId = sb.readIntLE64();

    reply.reset();

    // reply format:
    // <CMD16> - cmd id
    // <LEN16> - mechanisms count
    // <ID64>  - mech id
    // <DATA>  - mech info struct
    // <LEN16> - mech name len, <DATA> mech name
    reply.writeIntLE16(Pkcs11Op::GetSlotMechanisms);

    const PKCS11::Slot slot(slotId, pkcs11);
    auto mechs = slot.getMechanisms();
    reply.writeIntLE16(mechs.size());

    for(auto & mech: mechs)
    {
        if(auto mechInfo = slot.getMechInfo(mech))
        {
            reply.writeIntLE64(mech);
            reply.writeIntLE64(mechInfo->ulMinKeySize);
            reply.writeIntLE64(mechInfo->ulMaxKeySize);
            reply.writeIntLE64(mechInfo->flags);

            std::string mechName = PKCS11::mechStringEx(mech);
            reply.writeIntLE16(mechName.size());
            reply.write(mechName);
        }
        else
        {
            reply.writeIntLE16(0);
        }
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11GetSlotCertificates(const StreamBufRef & sb)
{
    if(9 > sb.last())
        throw std::underflow_error(NS_FuncName);

    // pkcs11 format:
    // <SLOT64> - slot id
    auto slotId = sb.readIntLE64();

    // <PAIR8> - have public/private pair
    bool havePublicPrivateKeys = sb.readInt8();

    reply.reset();
    std::unique_ptr<const PKCS11::Session> sess;

    try
    {
        sess = std::make_unique<const PKCS11::Session>(slotId, false /* rwmode */, pkcs11);
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        // certs count
        reply.writeIntLE16(0);
        return false;
    }

    // reply format:
    // <CMD16> - cmd id
    // <LEN16> - objects count
    // <LEN16> - obj id len, <DATA> - obj id data
    // <LEN32> - cert len, <DATA> - cert data

    reply.writeIntLE16(Pkcs11Op::GetSlotCertificates);

    auto certs = sess->getCertificates(true /* havePulicPrivateKeys */);
    reply.writeIntLE16(certs.size());

    for(auto & handle: certs)
    {
        auto objInfo = sess->getObjectInfo(handle, { CKA_VALUE });

        auto rawId = objInfo.getId();
        reply.writeIntLE16(rawId.size());
        reply.write(rawId.data(), rawId.size());

        auto rawValue = objInfo.getRawData(CKA_VALUE);
        reply.writeIntLE32(rawValue.size());
        reply.write(rawValue.data(), rawValue.size());
    }

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11SignData(const StreamBufRef & sb)
{
    if(18 > sb.last())
        throw std::underflow_error(NS_FuncName);

    // pkcs11 format:
    // <SLOT64> - slot id
    // <MECH64> - mech type
    // <PIN16> - pin len, <DATA> pin data
    // <CERT16> - cert id len, <DATA> cert id data
    // <DATA32> - data len, <DATA> sign data
    auto slotId = sb.readIntLE64();
    auto mechType = sb.readIntLE64();

    auto pinLen = sb.readIntLE16();
    if(pinLen > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto pin = sb.readString(pinLen);

    auto certLen = sb.readIntLE16();
    if(certLen > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto certId = sb.read(certLen);

    if(4 > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto valLen = sb.readIntLE32();
    auto values = sb.read(valLen);

    reply.reset();
    std::unique_ptr<PKCS11::Session> sess;

    try
    {
        sess = std::make_unique<PKCS11::Session>(slotId, false /* rwmode */, pkcs11);
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        // certs count
        reply.writeIntLE32(0);
        return false;
    }

    // reply format:
    // <DATA32> - data len, <DATA> sign data

    sess->login(pin);
    auto sign = sess->signData(certId, values.data(), values.size(), mechType);

    reply.writeIntLE32(sign.size());
    reply.write(sign.data(), sign.size());

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}

bool LTSM::Channel::ConnectorClientPkcs11::pkcs11DecryptData(const StreamBufRef & sb)
{
    if(18 > sb.last())
        throw std::underflow_error(NS_FuncName);

    // pkcs11 format:
    // <SLOT64> - slot id
    // <MECH64> - mech type
    // <PIN16> - pin len, <DATA> pin data
    // <CERT16> - cert id len, <DATA> cert id data
    // <DATA32> - data len, <DATA> sign data
    auto slotId = sb.readIntLE64();
    auto mechType = sb.readIntLE64();

    auto pinLen = sb.readIntLE16();
    if(pinLen > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto pin = sb.readString(pinLen);

    auto certLen = sb.readIntLE16();
    if(certLen > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto certId = sb.read(certLen);

    if(4 > sb.last())
        throw std::underflow_error(NS_FuncName);

    auto valLen = sb.readIntLE32();
    auto values = sb.read(valLen);

    reply.reset();

    std::unique_ptr<PKCS11::Session> sess;

    try
    {
        sess = std::make_unique<PKCS11::Session>(slotId, false /* rwmode */, pkcs11);
    }
    catch(const std::exception & err)
    {
        Application::error("%s: exception: %s", __FUNCTION__, err.what());
        // certs count
        reply.writeIntLE32(0);
        return false;
    }

    // reply format:
    // <DATA32> - data len, <DATA> sign data

    sess->login(pin);
    auto sign = sess->decryptData(certId, values.data(), values.size(), mechType);

    reply.writeIntLE32(sign.size());
    reply.write(sign.data(), sign.size());

    owner->sendLtsmEvent(cid, reply.rawbuf());
    return true;
}
