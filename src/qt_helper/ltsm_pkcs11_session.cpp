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
#include <thread>
#include <algorithm>
#include <filesystem>

#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>

#include <boost/asio/cancellation_signal.hpp>

#include "ltsm_tools.h"
#include "ltsm_pkcs11.h"
#include "ltsm_application.h"
#include "ltsm_pkcs11_session.h"

using namespace LTSM;
using namespace std::chrono_literals;
using namespace boost;

Pkcs11Client::Pkcs11Client(int displayNum, QObject * obj) : QThread(obj),
    AsyncSocket<asio::local::stream_protocol::socket>(ioc_.get_executor()),
    work_guard_{asio::make_work_guard(ioc_)},
    send_guard_{ioc_.get_executor()},
    templatePath{"/var/run/ltsm/pkcs11/%{display}/sock"} {
    templatePath.replace(QString("%{display}"), QString::number(displayNum));
}

Pkcs11Client::~Pkcs11Client() {
    stop();

    shutdown = true;
    if(! wait(1000)) {
        terminate();
        wait();
    }
}

void Pkcs11Client::stop(void) {
    update_tokens_.emit(asio::cancellation_type::terminal);

    system::error_code ec;
    socket().cancel(ec);
    socket().close(ec);

    work_guard_.reset();
}

void Pkcs11Client::run(void) {
    asio::co_spawn(ioc_, remoteConnect(), asio::detached);

    ioc_.run();
    Q_EMIT pkcs11Shutdown();
}

asio::awaitable<void> Pkcs11Client::remoteConnect(void) {
    const int attempts = 5;
    const auto path = templatePath.toStdString();

    for(int it = 1; it <= attempts; it++) {
        try {
            co_await socket().async_connect(path, asio::use_awaitable);
            co_return;
        } catch(const system::system_error& ec) {
            if(it == attempts) {
                throw;
            }
        }

        asio::steady_timer timer{ioc_, 300ms};
        co_await timer.async_wait(asio::use_awaitable);
    }

    Application::debug(DebugType::Pkcs11, "{}: connected, path: {}", __FUNCTION__, path);
    uint16_t cmd, err;

    try {
        co_await async_send_le16(Pkcs11Op::Init);
        // send proto ver
        co_await async_send_le16(1);

        // client reply
        cmd = co_await async_recv_le16();
        err = co_await async_recv_le16();
    } catch(const std::exception & exp) {
        Application::error("{}: exception: {}", NS_FuncNameV, "PKCS11 initialization failed");
        Q_EMIT pkcs11Error("PKCS11 initialization failed");
        stop();
        co_return;
    }

    if(cmd != Pkcs11Op::Init) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        Q_EMIT pkcs11Error("PKCS11 initialization failed");
        stop();
        co_return;
    }

    if(err) {
        auto str = co_await async_recv_buf<std::string>(err);
        Application::error("{}: recv error: {}", __FUNCTION__, str);
        Q_EMIT pkcs11Error(QString("PKCS11 error: %1").arg(str.c_str()));
        stop();
        co_return;
    }

    // proto version
    [[maybe_unused]] auto ver = co_await async_recv_le16();
    if(ver != 1) {
        Application::error("{}: {}: failed, ver: {:#04x}", __FUNCTION__, "version", ver);
        Q_EMIT pkcs11Error("PKCS11 initialization failed");
        stop();
        co_return;
    }

    // library info
    PKCS11::LibraryInfo info;
    info.cryptokiVersion.major = co_await async_recv_byte();
    info.cryptokiVersion.minor = co_await async_recv_byte();

    co_await async_recv_buf(info.manufacturerID, sizeof(info.manufacturerID));
    info.flags = co_await async_recv_le64();
    co_await async_recv_buf(info.libraryDescription, sizeof(info.libraryDescription));

    info.libraryVersion.major = co_await async_recv_byte();
    info.libraryVersion.minor = co_await async_recv_byte();

    // update tokens timer
    asio::co_spawn(ioc_, updateTokensTimer(), asio::bind_cancellation_slot(update_tokens_.slot(), asio::detached));

    co_return;
}

asio::awaitable<void> Pkcs11Client::updateTokensTimer(void) {
    bool success = false;
    try {
        for(;;) {
            asio::steady_timer timer(ioc_, std::chrono::seconds(1));
            co_await timer.async_wait(asio::use_awaitable);
            co_await updateTokens();
        }
    } catch (const boost::system::system_error& err) {
        auto ec = err.code();
        if(ec != asio::error::operation_aborted) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "timer", ec.value(), ec.message());
        }
    } catch (const std::exception& err) {
        Application::error("{}: exception: `{}'", __FUNCTION__, err.what());
        asio::co_spawn(ioc_, [this]() -> asio::awaitable<void> { stop(); co_return; }, asio::detached);
    }
}

asio::awaitable<void> Pkcs11Client::updateTokens(void) {
    co_await async_send_le16(Pkcs11Op::GetSlots);
    co_await async_send_byte(1 /* bool tokenPresentOnly */);

    // client reply
    // <CMD16> - cmd id
    // <LEN16> - slots count
    // <ID64> - slot id
    // <DATA> slot info struct
    // <DATA> token info struct
    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::GetSlots) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        throw pkcs11_error(NS_FuncNameS);
    }

    // slot counts
    uint16_t counts = co_await async_recv_le16();
    std::list<Pkcs11Token> newTokens;

    while(counts--) {
        auto slotId = co_await async_recv_le64();

        PKCS11::SlotInfo slotInfo;
        auto slotValid = co_await async_recv_byte();

        if(slotValid) {
            co_await async_recv_buf(slotInfo.slotDescription, sizeof(slotInfo.slotDescription));
            co_await async_recv_buf(slotInfo.manufacturerID, sizeof(slotInfo.manufacturerID));
            slotInfo.flags = co_await async_recv_le64();
            slotInfo.hardwareVersion.major = co_await async_recv_byte();
            slotInfo.hardwareVersion.minor = co_await async_recv_byte();
            slotInfo.firmwareVersion.major = co_await async_recv_byte();
            slotInfo.firmwareVersion.minor = co_await async_recv_byte();
        }

        PKCS11::TokenInfo tokenInfo;
        auto tokenValid = co_await async_recv_byte();

        if(tokenValid) {
            co_await async_recv_buf(tokenInfo.label, sizeof(tokenInfo.label));
            co_await async_recv_buf(tokenInfo.manufacturerID, sizeof(tokenInfo.manufacturerID));
            co_await async_recv_buf(tokenInfo.model, sizeof(tokenInfo.model));
            co_await async_recv_buf(tokenInfo.serialNumber, sizeof(tokenInfo.serialNumber));
            tokenInfo.flags = co_await async_recv_le64();
            tokenInfo.ulMaxSessionCount = co_await async_recv_le64();
            tokenInfo.ulSessionCount = co_await async_recv_le64();
            tokenInfo.ulMaxRwSessionCount = co_await async_recv_le64();
            tokenInfo.ulRwSessionCount = co_await async_recv_le64();
            tokenInfo.ulMaxPinLen = co_await async_recv_le64();
            tokenInfo.ulMinPinLen = co_await async_recv_le64();
            tokenInfo.ulTotalPublicMemory = co_await async_recv_le64();
            tokenInfo.ulFreePublicMemory = co_await async_recv_le64();
            tokenInfo.ulTotalPrivateMemory = co_await async_recv_le64();
            tokenInfo.ulFreePrivateMemory = co_await async_recv_le64();
            tokenInfo.hardwareVersion.major = co_await async_recv_byte();
            tokenInfo.hardwareVersion.minor = co_await async_recv_byte();
            tokenInfo.firmwareVersion.major = co_await async_recv_byte();
            tokenInfo.firmwareVersion.minor = co_await async_recv_byte();
            co_await async_recv_buf(tokenInfo.utcTime, sizeof(tokenInfo.utcTime));
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

    if(removedTokens.size() || addedTokens.size()) {
        tokens.swap(newTokens);
        Q_EMIT pkcs11TokensChanged();
    }

    co_return;
}

// FIXME
std::list<Pkcs11Token> Pkcs11Client::getTokens(void) const {
    std::scoped_lock guard{ lock };
    return tokens;
}


std::list<Pkcs11Cert> Pkcs11Client::getCertificates(uint64_t slotId) {
    std::scoped_lock guard{ lock };
    sock.sendIntLE16(Pkcs11Op::GetSlotCertificates);
    sock.sendIntLE64(slotId);
    sock.sendInt8(1 /* bool havePublicPrivateKeys */);
    sock.sendFlush();
    // client reply
    auto cmd = sock.recvIntLE16();

    if(cmd != Pkcs11Op::GetSlotCertificates) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        return {};
    }

    // certs counts
    uint16_t counts = sock.recvIntLE16();
    std::list<Pkcs11Cert> certs;

    while(counts--) {
        auto idLen = sock.recvIntLE16();
        auto id = sock.recvData(idLen);
        auto valueLen = sock.recvIntLE32();
        auto value = sock.recvData(valueLen);
        certs.emplace_back(Pkcs11Cert{ .objectId = std::move(id), .objectValue = std::move(value) });
    }

    return certs;
}

std::list<Pkcs11Mech> Pkcs11Client::getMechanisms(uint64_t slotId) {
    std::scoped_lock guard{ lock };
    sock.sendIntLE16(Pkcs11Op::GetSlotMechanisms);
    sock.sendIntLE64(slotId);
    sock.sendFlush();
    // client reply
    auto cmd = sock.recvIntLE16();

    if(cmd != Pkcs11Op::GetSlotMechanisms) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        return {};
    }

    // certs counts
    uint16_t counts = sock.recvIntLE16();
    std::list<Pkcs11Mech> res;

    while(counts--) {
        auto id = sock.recvIntLE64();
        auto min = sock.recvIntLE64();
        auto max = sock.recvIntLE64();
        auto flags = sock.recvIntLE64();
        auto len = sock.recvIntLE16();
        auto name = sock.recvString(len);
        res.emplace_back(Pkcs11Mech{ .mechId = id, .minKey = min, .maxKey = max, .flags = flags, .name = name });
    }

    return res;
}

std::vector<uint8_t> Pkcs11Client::signData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {
    std::scoped_lock guard{ lock };
    sock.sendIntLE16(Pkcs11Op::SignData);
    sock.sendIntLE64(slotId);
    sock.sendIntLE64(mechType);
    sock.sendIntLE16(pin.size());
    sock.sendString(pin);
    sock.sendIntLE16(certId.size());
    sock.sendData(certId);
    sock.sendIntLE32(len);
    sock.sendRaw(data, len);
    sock.sendFlush();
    // client reply
    auto cmd = sock.recvIntLE16();

    if(cmd != Pkcs11Op::SignData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        return {};
    }

    // sign result length
    uint32_t length = sock.recvIntLE32();

    if(length) {
        return sock.recvData(length);
    }

    return {};
}

std::vector<uint8_t> Pkcs11Client::decryptData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {
    std::scoped_lock guard{ lock };
    sock.sendIntLE16(Pkcs11Op::DecryptData);
    sock.sendIntLE64(slotId);
    sock.sendIntLE64(mechType);
    sock.sendIntLE16(pin.size());
    sock.sendString(pin);
    sock.sendIntLE16(certId.size());
    sock.sendData(certId);
    sock.sendIntLE32(len);
    sock.sendRaw(data, len);
    sock.sendFlush();
    // client reply
    auto cmd = sock.recvIntLE16();

    if(cmd != Pkcs11Op::DecryptData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        return {};
    }

    // decrypt result length
    uint32_t length = sock.recvIntLE32();

    if(length) {
        return sock.recvData(length);
    }

    return {};
}
