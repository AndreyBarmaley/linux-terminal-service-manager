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
#include <future>
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

Pkcs11Client::Pkcs11Client(int displayNum, QObject* obj) : QThread(obj),
    AsyncSocket<asio::local::stream_protocol::socket>(member.get_executor()),
    ioc_{member},
    work_guard_{asio::make_work_guard(ioc_)},
    templatePath{"/var/run/ltsm/pkcs11/%{display}/sock"} {
    templatePath.replace(QString("%{display}"), QString::number(displayNum));
}

Pkcs11Client::~Pkcs11Client() {
    stop();

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
    const int attempts = 7;
    const auto path = templatePath.toStdString();

    for(int it = 1; it <= attempts; it++) {
        try {
            co_await socket().async_connect(path, asio::use_awaitable);
            break;
        } catch(const system::system_error& err) {
            if(it == attempts) {
                Application::error("{}: {} failed, path: {}, attempts: {}, error: {}", __FUNCTION__, "connect", path, attempts, err.code().message());
                co_return;
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
    auto ver = co_await async_recv_le16();

    if(ver != 1) {
        Application::error("{}: {}: failed, ver: {:#04x}", __FUNCTION__, "version", ver);
        Q_EMIT pkcs11Error("PKCS11 initialization failed");
        stop();
        co_return;
    }

    Application::debug(DebugType::Pkcs11, "{}: proto version: {}", __FUNCTION__, ver);

    // library info
    PKCS11::LibraryInfo info;
    info.cryptokiVersion.major = co_await async_recv_byte();
    info.cryptokiVersion.minor = co_await async_recv_byte();

    assert(sizeof(info.manufacturerID) == 32);
    assert(sizeof(info.libraryDescription) == 32);

    co_await async_recv_buf(info.manufacturerID, sizeof(info.manufacturerID));
    info.flags = co_await async_recv_le64();
    co_await async_recv_buf(info.libraryDescription, sizeof(info.libraryDescription));

    info.libraryVersion.major = co_await async_recv_byte();
    info.libraryVersion.minor = co_await async_recv_byte();

    Application::debug(DebugType::Pkcs11, "{}: cryptoki version: {}.{}",
         __FUNCTION__, static_cast<uint16_t>(info.cryptokiVersion.major), static_cast<uint16_t>(info.cryptokiVersion.minor));

    Application::debug(DebugType::Pkcs11, "{}: library version: {}.{}",
         __FUNCTION__, static_cast<uint16_t>(info.libraryVersion.major), static_cast<uint16_t>(info.libraryVersion.minor));

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
            co_await send_lock_.async_lock(asio::use_awaitable);
            auto success = co_await updateTokens();
            send_lock_.unlock();
            if(success) {
                Q_EMIT pkcs11TokensChanged();
            }
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
    send_lock_.unlock();
}

asio::awaitable<bool> Pkcs11Client::updateTokens(void) {
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

    ListTokens newTokens;
    Application::debug(DebugType::Pkcs11, "{}: tokens counts: {}", __FUNCTION__, counts);

    while(counts--) {
        auto slotId = co_await async_recv_le64();

        PKCS11::SlotInfo slotInfo;
        auto slotValid = co_await async_recv_byte();

        assert(sizeof(slotInfo.slotDescription) == 64);
        assert(sizeof(slotInfo.manufacturerID) == 32);

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

        assert(sizeof(tokenInfo.label) == 32);
        assert(sizeof(tokenInfo.manufacturerID) == 32);
        assert(sizeof(tokenInfo.model) == 16);
        assert(sizeof(tokenInfo.serialNumber) == 16);
        assert(sizeof(tokenInfo.utcTime) == 16);

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
    ListTokens removedTokens, addedTokens;
    std::set_difference(tokens.begin(), tokens.end(),
                        newTokens.begin(), newTokens.end(),
                        std::back_inserter(removedTokens));
    std::set_difference(newTokens.begin(), newTokens.end(),
                        tokens.begin(), tokens.end(),
                        std::back_inserter(addedTokens));

    if(removedTokens.size() || addedTokens.size()) {
        tokens.swap(newTokens);
        co_return true;
    }

    co_return false;
}

ListTokens Pkcs11Client::getTokens(void) const {

    std::promise<ListTokens> promise;
    auto res = promise.get_future();

    asio::co_spawn(ioc_, [&promise, this]() -> asio::awaitable<void> {
        co_await send_lock_.async_lock(asio::use_awaitable);
        auto tokens = this->tokens;
        send_lock_.unlock();
        promise.set_value(std::move(tokens));
        co_return;
    }, asio::detached);

    return res.get();
}

ListCertificates Pkcs11Client::getCertificates(uint64_t slotId) const {

    std::promise<ListCertificates> promise;
    auto res = promise.get_future();

    asio::co_spawn(ioc_, [&promise, &slotId, this]() -> asio::awaitable<void> {
        co_await send_lock_.async_lock(asio::use_awaitable);
        auto certs = co_await this->loadCertificates(slotId);
        send_lock_.unlock();
        promise.set_value(std::move(certs));
        co_return;
    }, asio::detached);

    return res.get();
}

asio::awaitable<ListCertificates> Pkcs11Client::loadCertificates(uint64_t slotId) const {

    co_await async_send_le16(Pkcs11Op::GetSlotCertificates);
    co_await async_send_le64(slotId);
    co_await async_send_byte(1 /* bool havePublicPrivateKeys */);

    ListCertificates certs;

    // client reply
    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::GetSlotCertificates) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return certs;
    }

    // certs counts
    auto counts = co_await async_recv_le16();
    Application::debug(DebugType::Pkcs11, "{}: certs counts: {}", __FUNCTION__, counts);

    while(counts--) {
        auto idLen = co_await async_recv_le16();
        auto id = co_await async_recv_buf<binary_buf>(idLen);
        auto valueLen = co_await async_recv_le32();
        auto value = co_await async_recv_buf<binary_buf>(valueLen);

        certs.emplace_back(Pkcs11Cert{ .objectId = std::move(id), .objectValue = std::move(value) });
    }

    co_return certs;
}

ListMechanisms Pkcs11Client::getMechanisms(uint64_t slotId) const {

    std::promise<ListMechanisms> promise;
    auto res = promise.get_future();

    asio::co_spawn(ioc_, [&promise, &slotId, this]() -> asio::awaitable<void> {
        co_await send_lock_.async_lock(asio::use_awaitable);
        auto mechs = co_await this->loadMechanisms(slotId);
        send_lock_.unlock();
        promise.set_value(std::move(mechs));
        co_return;
    }, asio::detached);

    return res.get();
}

asio::awaitable<ListMechanisms> Pkcs11Client::loadMechanisms(uint64_t slotId) const {

    co_await async_send_le16(Pkcs11Op::GetSlotMechanisms);
    co_await async_send_le64(slotId);

    ListMechanisms res;

    // client reply
    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::GetSlotMechanisms) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return res;
    }

    // certs counts
    auto counts = co_await async_recv_le16();
    Application::debug(DebugType::Pkcs11, "{}: mechs counts: {}", __FUNCTION__, counts);

    while(counts--) {
        auto id = co_await async_recv_le64();
        auto min = co_await async_recv_le64();
        auto max = co_await async_recv_le64();
        auto flags = co_await async_recv_le64();
        auto len = co_await async_recv_le16();
        auto name = co_await async_recv_buf<std::string>(len);
        res.emplace_back(Pkcs11Mech{ .mechId = id, .minKey = min, .maxKey = max, .flags = flags, .name = name });
    }

    co_return res;
}

binary_buf Pkcs11Client::signData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {

    std::promise<binary_buf> promise;
    auto res = promise.get_future();

    asio::co_spawn(ioc_, [&promise, &slotId, &pin, &certId, &data, &len, &mechType, this]() -> asio::awaitable<void> {
        co_await send_lock_.async_lock(asio::use_awaitable);
        auto buf = co_await this->loadSignData(slotId, pin, certId, data, len, mechType);
        send_lock_.unlock();
        promise.set_value(std::move(buf));
        co_return;
    }, asio::detached);

    return res.get();
}

asio::awaitable<binary_buf> Pkcs11Client::loadSignData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {

    co_await async_send_le16(Pkcs11Op::SignData);
    co_await async_send_le64(slotId);
    co_await async_send_le64(mechType);
    co_await async_send_le16(pin.size());
    co_await async_send_buf(asio::buffer(pin));
    co_await async_send_le16(certId.size());
    co_await async_send_buf(asio::buffer(certId));
    co_await async_send_le32(len);
    co_await async_send_buf(asio::buffer(data, len));

    // client reply
    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::SignData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return binary_buf{};
    }

    // sign result length
    auto length = co_await async_recv_le32();

    if(length) {
        co_return co_await async_recv_buf<binary_buf>(length);
    }

    co_return binary_buf{};
}

std::vector<uint8_t> Pkcs11Client::decryptData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {

    std::promise<binary_buf> promise;
    auto res = promise.get_future();

    asio::co_spawn(ioc_, [&promise, &slotId, &pin, &certId, &data, &len, &mechType, this]() -> asio::awaitable<void> {
        co_await send_lock_.async_lock(asio::use_awaitable);
        auto buf = co_await this->loadDecryptData(slotId, pin, certId, data, len, mechType);
        send_lock_.unlock();
        promise.set_value(std::move(buf));
        co_return;
    }, asio::detached);

    return res.get();
}

asio::awaitable<binary_buf> Pkcs11Client::loadDecryptData(uint64_t slotId, const std::string & pin,
        const std::vector<uint8_t> & certId, const void* data, size_t len, uint64_t mechType) {

    co_await async_send_le16(Pkcs11Op::DecryptData);
    co_await async_send_le64(slotId);
    co_await async_send_le64(mechType);
    co_await async_send_le16(pin.size());
    co_await async_send_buf(asio::buffer(pin));
    co_await async_send_le16(certId.size());
    co_await async_send_buf(asio::buffer(certId));
    co_await async_send_le32(len);
    co_await async_send_buf(asio::buffer(data, len));

    // client reply
    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::DecryptData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return binary_buf{};
    }

    // decrypt result length
    auto length = co_await async_recv_le32();

    if(length) {
        co_return co_await async_recv_buf<binary_buf>(length);
    }

    co_return binary_buf{};
}
