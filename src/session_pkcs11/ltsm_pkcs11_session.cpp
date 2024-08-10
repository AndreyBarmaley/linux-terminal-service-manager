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
 ***************************************************************************/

#include <chrono>
#include <algorithm>
#include <filesystem>

#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>

#include "ltsm_tools.h"
#include "ltsm_pkcs11.h"
#include "ltsm_application.h"
#include "ltsm_pkcs11_session.h"

using namespace LTSM;
using namespace std::chrono_literals;

Pkcs11Client::Pkcs11Client(int displayNum, QObject* obj) : QThread(obj), templatePath("/var/run/ltsm/pkcs11/%{display}/sock")
{
    templatePath.replace(QString("%{display}"), QString::number(displayNum));
}

Pkcs11Client::~Pkcs11Client()
{
    shutdown = true;

    if(! wait(1000))
    {
        terminate();
        wait();
    }
}

void Pkcs11Client::run(void)
{
    // 1. wait socket
    std::error_code fserr;
    std::filesystem::path socketPath(templatePath.toStdString());
    int fd = -1;

    while(0 > fd)
    {
        if(shutdown)
        {
            emit pkcs11Shutdown();
            return;
        }

        if(! std::filesystem::is_socket(socketPath, fserr))
        {
            msleep(350);
            continue;
        }

        fd = UnixSocket::connect(socketPath);        
    }

    Application::debug("%s: connected, socket fd: %" PRId32, __FUNCTION__, fd);
    sock.setSocket(fd);

    // send initialize packet
    sock.sendIntLE16(Pkcs11Op::Init);
    // send proto ver
    sock.sendIntLE16(1);
    sock.sendFlush();

    // client reply
    auto cmd = sock.recvIntLE16();
    auto err = sock.recvIntLE16();

    if(cmd != Pkcs11Op::Init)
    {
        Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
        emit pkcs11Error("PKCS11 initialization failed");
        emit pkcs11Shutdown();
        return;
    }
        
    if(err)
    {
        auto str = sock.recvString(err);
        Application::error("%s: recv error: %s", __FUNCTION__, str.c_str());
        emit pkcs11Error(QString("PKCS11 error: %1").arg(QString(str.c_str())));
        emit pkcs11Shutdown();
        return;
    }

    // proto version
    auto ver = sock.recvIntLE16();
    // library info
    PKCS11::LibraryInfo info;
    info.cryptokiVersion.major = sock.recvInt8();
    info.cryptokiVersion.minor = sock.recvInt8();
    sock.recvData(& info.manufacturerID, 32);
    info.flags = sock.recvIntLE64();
    sock.recvData(& info.libraryDescription, 32);
    info.libraryVersion.major = sock.recvInt8();
    info.libraryVersion.minor = sock.recvInt8();

    auto updateTokensTime = Tools::TimePoint(std::chrono::seconds(1));

    while(true)
    {
        if(shutdown)
            break;

        if(updateTokensTime.check())
            updateTokens();

        msleep(250);
    }

    emit pkcs11Shutdown();
}

const std::list<Pkcs11Token> & Pkcs11Client::getTokens(void) const
{
    std::scoped_lock guard{ lock };
    return tokens;
}

bool Pkcs11Client::updateTokens(void)
{
    std::scoped_lock guard{ lock };

    sock.sendIntLE16(Pkcs11Op::GetSlots);
    sock.sendInt8(1 /* bool tokenPresentOnly */);
    sock.sendFlush();

    // client reply
    // <CMD16> - cmd id
    // <LEN16> - slots count
    // <ID64> - slot id
    // <DATA> slot info struct
    // <DATA> token info struct

    auto cmd = sock.recvIntLE16();

    if(cmd != Pkcs11Op::GetSlots)
    {
        Application::error("%s: %s: failed, cmd: 0x%" PRIx16, __FUNCTION__, "id", cmd);
        return false;
    }

    // slot counts
    uint16_t counts = sock.recvIntLE16();
    std::list<Pkcs11Token> newTokens;

    while(counts--)
    {
        auto slotId = sock.recvIntLE64();

        PKCS11::SlotInfo slotInfo;
        if(sock.recvInt8())
        {
            sock.recvData(slotInfo.slotDescription, 64);
            sock.recvData(slotInfo.manufacturerID, 32);
            slotInfo.flags = sock.recvIntLE64();
            slotInfo.hardwareVersion.major = sock.recvInt8();
            slotInfo.hardwareVersion.minor = sock.recvInt8();
            slotInfo.firmwareVersion.major = sock.recvInt8();
            slotInfo.firmwareVersion.minor = sock.recvInt8();
        }

        PKCS11::TokenInfo tokenInfo;
        if(sock.recvInt8())
        {
            sock.recvData(tokenInfo.label, 32);
            sock.recvData(tokenInfo.manufacturerID, 32);
            sock.recvData(tokenInfo.model, 16);
            sock.recvData(tokenInfo.serialNumber, 16);

            tokenInfo.flags = sock.recvIntLE64();
            tokenInfo.ulMaxSessionCount = sock.recvIntLE64();
            tokenInfo.ulSessionCount = sock.recvIntLE64();
            tokenInfo.ulMaxRwSessionCount = sock.recvIntLE64();
            tokenInfo.ulRwSessionCount = sock.recvIntLE64();
            tokenInfo.ulMaxPinLen = sock.recvIntLE64();
            tokenInfo.ulMinPinLen = sock.recvIntLE64();
            tokenInfo.ulTotalPublicMemory = sock.recvIntLE64();
            tokenInfo.ulFreePublicMemory = sock.recvIntLE64();
            tokenInfo.ulTotalPrivateMemory = sock.recvIntLE64();
            tokenInfo.ulFreePrivateMemory = sock.recvIntLE64();

            tokenInfo.hardwareVersion.major = sock.recvInt8();
            tokenInfo.hardwareVersion.minor = sock.recvInt8();
            tokenInfo.firmwareVersion.major = sock.recvInt8();
            tokenInfo.firmwareVersion.minor = sock.recvInt8();
            sock.recvData(tokenInfo.utcTime, 16);
        }

        newTokens.emplace_back(Pkcs11Token{ slotId, std::move(slotInfo), std::move(tokenInfo) });
    }

    newTokens.sort();

    std::list<Pkcs11Token> removedTokens, addedTokens;

    std::set_difference(tokens.begin(), tokens.end(),
                        newTokens.begin(), newTokens.end(),
                        std::back_inserter(removedTokens));

    std::set_difference(newTokens.begin(), newTokens.end(),
                        tokens.begin(), tokens.end(),
                        std::back_inserter(addedTokens));

    if(removedTokens.size() || addedTokens.size())
    {
        tokens.swap(newTokens);
        emit pkcs11TokensChanged();
    }

    return true;
}

std::list<Pkcs11Cert> Pkcs11Client::getCertificates(uint64_t slotId)
{
    std::scoped_lock guard{ lock };

    sock.sendIntLE16(Pkcs11Op::GetSlotCertificates);
    sock.sendIntLE64(slotId);
    sock.sendInt8(1 /* bool havePublicPrivateKeys */);
    sock.sendFlush();

    // certs counts
    uint16_t counts = sock.recvIntLE16();
    std::list<Pkcs11Cert> certs;

    while(counts--)
    {
        Pkcs11Cert cert;

        auto idLen = sock.recvIntLE16();
        auto id = sock.recvData(idLen);

        auto valueLen = sock.recvIntLE32();
        auto value = sock.recvData(valueLen);

        certs.emplace_back(Pkcs11Cert{ .objectId = std::move(id), .objectValue = std::move(value) });
    }

    return certs;
}

std::vector<uint8_t> Pkcs11Client::signData(uint64_t slotId, const std::vector<uint8_t> & certId, const void* data, size_t len)
{
    std::scoped_lock guard{ lock };

    sock.sendIntLE16(Pkcs11Op::SignData);
    sock.sendIntLE64(slotId);
    sock.sendIntLE16(certId.size());
    sock.sendData(certId);
    sock.sendIntLE32(len);
    sock.sendRaw(data, len);
    sock.sendFlush();

    return {};
}

std::vector<uint8_t> Pkcs11Client::decryptData(uint64_t slotId, const std::vector<uint8_t> & certId, const void* data, size_t len)
{
    std::scoped_lock guard{ lock };

    sock.sendIntLE16(Pkcs11Op::SignData);
    sock.sendIntLE64(slotId);
    sock.sendIntLE16(certId.size());
    sock.sendData(certId);
    sock.sendIntLE32(len);
    sock.sendRaw(data, len);
    sock.sendFlush();

    return {};
}
