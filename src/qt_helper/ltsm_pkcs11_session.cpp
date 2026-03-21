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
        co_await async_send_values(
            endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::Init)),
            endian::native_to_little(static_cast<uint16_t>(1 /* proto version */)));

        // client reply: cmd16, err16
        co_await async_recv_values(cmd, err);
        endian::little_to_native_inplace(cmd);
        endian::little_to_native_inplace(err);
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
    assert(sizeof(info.manufacturerID) == 32);
    assert(sizeof(info.libraryDescription) == 32);

    co_await async_recv_values(
        info.cryptokiVersion.major,
        info.cryptokiVersion.minor,
        asio::buffer(info.manufacturerID, sizeof(info.manufacturerID)),
        info.flags,
        asio::buffer(info.libraryDescription, sizeof(info.libraryDescription)),
        info.libraryVersion.major,
        info.libraryVersion.minor
    );

    endian::little_to_native_inplace(info.flags);

    Application::debug(DebugType::Pkcs11, "{}: cryptoki version: {}.{}",
                       __FUNCTION__, info.cryptokiVersion.major, info.cryptokiVersion.minor);

    Application::debug(DebugType::Pkcs11, "{}: library version: {}.{}",
                       __FUNCTION__, info.libraryVersion.major, info.libraryVersion.minor);

    // update tokens timer
    asio::co_spawn(ioc_, updateTokensTimer(), asio::bind_cancellation_slot(update_tokens_.slot(), asio::detached));

    co_return;
}

asio::awaitable<void> Pkcs11Client::updateTokensTimer(void) {
    bool success = false;

    try {
        for(;;) {
            asio::steady_timer timer(ioc_, std::chrono::milliseconds(450));
            co_await timer.async_wait(asio::use_awaitable);
            co_await send_lock_.async_lock(asio::use_awaitable);
            auto success = co_await updateTokens();
            send_lock_.unlock();

            if(success) {
                Q_EMIT pkcs11TokensChanged();
            }
        }
    } catch(const boost::system::system_error& err) {
        auto ec = err.code();

        if(ec != asio::error::operation_aborted) {
            Application::error("{}: {} failed, code: {}, error: {}", __FUNCTION__, "timer", ec.value(), ec.message());
        }
    } catch(const std::exception& err) {
        Application::error("{}: exception: `{}'", __FUNCTION__, err.what());
        asio::co_spawn(ioc_, [this]() -> asio::awaitable<void> { stop(); co_return; }, asio::detached);
    }

    send_lock_.unlock();
}

asio::awaitable<bool> Pkcs11Client::updateTokens(void) {
    co_await async_send_values(
        endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::GetSlots)),
        static_cast<uint8_t>(1 /* bool tokenPresentOnly */));

    // client reply
    // <CMD16> - cmd id
    // <LEN16> - slots count
    uint16_t cmd, counts;
    co_await async_recv_values(cmd, counts);

    endian::little_to_native_inplace(cmd);
    endian::little_to_native_inplace(counts);

//    auto cmd = co_await async_recv_le16();

    if(cmd != Pkcs11Op::GetSlots) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        throw pkcs11_error(NS_FuncNameS);
    }

    // slot counts
//    uint16_t counts = co_await async_recv_le16();

    ListTokens newTokens;
    Application::debug(DebugType::Pkcs11, "{}: tokens counts: {}", __FUNCTION__, counts);

    while(counts--) {
        // <ID64> - slot id
        // <ID8> - valid slot
        // <DATA> slot info struct
        // <ID8> - valid token
        // <DATA> token info struct

        uint64_t slotId; uint8_t slotValid;
        co_await async_recv_values(slotId, slotValid);

        endian::little_to_native_inplace(slotId);
        PKCS11::SlotInfo slotInfo;

        if(slotValid) {
            static_assert(sizeof(slotInfo.slotDescription) == 64);
            static_assert(sizeof(slotInfo.manufacturerID) == 32);
            static_assert(sizeof(slotInfo.flags) == sizeof(uint64_t));

            co_await async_recv_values(
                asio::buffer(slotInfo.slotDescription, sizeof(slotInfo.slotDescription)),
                asio::buffer(slotInfo.manufacturerID, sizeof(slotInfo.manufacturerID)),
                slotInfo.flags,
                slotInfo.hardwareVersion.major,
                slotInfo.hardwareVersion.minor,
                slotInfo.firmwareVersion.major,
                slotInfo.firmwareVersion.minor
            );

            endian::little_to_native_inplace(slotInfo.flags);
        }

        PKCS11::TokenInfo tokenInfo;
        auto tokenValid = co_await async_recv_byte();

        if(tokenValid) {
            assert(sizeof(tokenInfo.label) == 32);
            assert(sizeof(tokenInfo.manufacturerID) == 32);
            assert(sizeof(tokenInfo.model) == 16);
            assert(sizeof(tokenInfo.serialNumber) == 16);
            assert(sizeof(tokenInfo.utcTime) == 16);

            co_await async_recv_values(
                asio::buffer(tokenInfo.label, sizeof(tokenInfo.label)),
                asio::buffer(tokenInfo.manufacturerID, sizeof(tokenInfo.manufacturerID)),
                asio::buffer(tokenInfo.model, sizeof(tokenInfo.model)),
                asio::buffer(tokenInfo.serialNumber, sizeof(tokenInfo.serialNumber)),
                tokenInfo.flags,
                tokenInfo.ulMaxSessionCount,
                tokenInfo.ulSessionCount,
                tokenInfo.ulMaxRwSessionCount,
                tokenInfo.ulRwSessionCount,
                tokenInfo.ulMaxPinLen,
                tokenInfo.ulMinPinLen,
                tokenInfo.ulTotalPublicMemory,
                tokenInfo.ulFreePublicMemory,
                tokenInfo.ulTotalPrivateMemory,
                tokenInfo.ulFreePrivateMemory,
                tokenInfo.hardwareVersion.major,
                tokenInfo.hardwareVersion.minor,
                tokenInfo.firmwareVersion.major,
                tokenInfo.firmwareVersion.minor,
                asio::buffer(tokenInfo.utcTime, sizeof(tokenInfo.utcTime))
            );

            endian::little_to_native_inplace(tokenInfo.flags);
            endian::little_to_native_inplace(tokenInfo.ulMaxSessionCount);
            endian::little_to_native_inplace(tokenInfo.ulSessionCount);
            endian::little_to_native_inplace(tokenInfo.ulMaxRwSessionCount);
            endian::little_to_native_inplace(tokenInfo.ulRwSessionCount);
            endian::little_to_native_inplace(tokenInfo.ulMaxPinLen);
            endian::little_to_native_inplace(tokenInfo.ulMinPinLen);
            endian::little_to_native_inplace(tokenInfo.ulTotalPublicMemory);
            endian::little_to_native_inplace(tokenInfo.ulFreePublicMemory);
            endian::little_to_native_inplace(tokenInfo.ulTotalPrivateMemory);
            endian::little_to_native_inplace(tokenInfo.ulFreePrivateMemory);
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

    co_await async_send_values(
        endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::GetSlotCertificates)),
        endian::native_to_little(slotId),
        static_cast<uint8_t>(1 /* bool havePublicPrivateKeys */));

    ListCertificates certs;

    // client reply: cmd16, counts16
    uint16_t cmd, counts;
    co_await async_recv_values(cmd, counts);

    endian::little_to_native_inplace(cmd);
    endian::little_to_native_inplace(counts);

    if(cmd != Pkcs11Op::GetSlotCertificates) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return certs;
    }

    Application::debug(DebugType::Pkcs11, "{}: certs counts: {}", __FUNCTION__, counts);

    while(counts--) {
        // <LEN16> - cert id len
        // <DATA> - cert id data
        // <LEN32> - cert data len
        // <DATA> - cert data
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

    co_await async_send_values(
        endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::GetSlotMechanisms)),
        endian::native_to_little(slotId));

    ListMechanisms res;

    // client reply: cmd16, counts16
    uint16_t cmd, counts;
    co_await async_recv_values(cmd, counts);

    endian::little_to_native_inplace(cmd);
    endian::little_to_native_inplace(counts);

    if(cmd != Pkcs11Op::GetSlotMechanisms) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return res;
    }

    Application::debug(DebugType::Pkcs11, "{}: mechs counts: {}", __FUNCTION__, counts);

    while(counts--) {
        uint64_t id, min, max, flags;
        uint16_t len;

        co_await async_recv_values(id, min, max, flags, len);
        endian::little_to_native_inplace(id);
        endian::little_to_native_inplace(min);
        endian::little_to_native_inplace(max);
        endian::little_to_native_inplace(flags);
        endian::little_to_native_inplace(len);

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

    co_await async_send_values(
        endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::SignData)),
        endian::native_to_little(slotId),
        endian::native_to_little(mechType),
        endian::native_to_little(static_cast<uint16_t>(pin.size())), pin,
        endian::native_to_little(static_cast<uint16_t>(certId.size())), certId,
        endian::native_to_little(static_cast<uint32_t>(len)), asio::const_buffer(data, len));

    // client reply: cmd16, length32
    uint16_t cmd; uint32_t length;
    co_await async_recv_values(cmd, length);

    endian::little_to_native_inplace(cmd);
    endian::little_to_native_inplace(length);

    if(cmd != Pkcs11Op::SignData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return binary_buf{};
    }

    if(length) {
        // sign result
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

    co_await async_send_values(
        endian::native_to_little(static_cast<uint16_t>(Pkcs11Op::DecryptData)),
        endian::native_to_little(slotId),
        endian::native_to_little(mechType),
        endian::native_to_little(static_cast<uint16_t>(pin.size())), pin,
        endian::native_to_little(static_cast<uint16_t>(certId.size())), certId,
        endian::native_to_little(static_cast<uint32_t>(len)), asio::const_buffer(data, len));

    // client reply: cmd16, length32
    uint16_t cmd; uint32_t length;
    co_await async_recv_values(cmd, length);

    endian::little_to_native_inplace(cmd);
    endian::little_to_native_inplace(length);

    if(cmd != Pkcs11Op::DecryptData) {
        Application::error("{}: {}: failed, cmd: {:#04x}", __FUNCTION__, "id", cmd);
        co_return binary_buf{};
    }

    if(length) {
        // decrypt result
        co_return co_await async_recv_buf<binary_buf>(length);
    }

    co_return binary_buf{};
}
