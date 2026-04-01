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

#ifndef LTSM_HELPER_PKCS11_H
#define LTSM_HELPER_PKCS11_H

#include <list>
#include <vector>
#include <cinttypes>

#include <QThread>

#include <boost/asio.hpp>
#include <boost/utility/base_from_member.hpp>

#include "ltsm_async_socket.h"
#include "ltsm_pkcs11_wrapper.h"
#include "ltsm_async_mutex.h"

struct Pkcs11Token {
    uint64_t slotId;

    LTSM::PKCS11::SlotInfo slotInfo;
    LTSM::PKCS11::TokenInfo tokenInfo;

    bool operator== (const Pkcs11Token & st) const {
        return tokenInfo.getModel() == st.tokenInfo.getModel() &&
               tokenInfo.getSerialNumber() == st.tokenInfo.getSerialNumber();
    }

    bool operator< (const Pkcs11Token & st) const {
        auto model1 = tokenInfo.getModel();
        auto model2 = st.tokenInfo.getModel();

        if(model1 == model2) {
            return tokenInfo.getSerialNumber() < st.tokenInfo.getSerialNumber();
        }

        return model1 < model2;
    }
};

struct Pkcs11Mech {
    uint64_t mechId;
    uint64_t minKey;
    uint64_t maxKey;
    uint64_t flags;
    std::string name;
};

struct Pkcs11Cert {
    std::vector<uint8_t> objectId;
    std::vector<uint8_t> objectValue;
};

using ListTokens = std::list<Pkcs11Token>;
using ListCertificates = std::list<Pkcs11Cert>;
using ListMechanisms = std::list<Pkcs11Mech>;
using binary_buf = std::vector<uint8_t>;

class Pkcs11Client : public QThread, protected boost::base_from_member<boost::asio::io_context>, protected LTSM::AsyncSocket<boost::asio::local::stream_protocol::socket> {
    Q_OBJECT

    boost::asio::io_context & ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    boost::asio::cancellation_signal update_tokens_;

    mutable LTSM::async_mutex send_lock_;

    QString templatePath;
    ListTokens tokens;

  public:
    Pkcs11Client(int displayNum, QObject*);
    ~Pkcs11Client();

    ListTokens getTokens(void) const;
    ListCertificates getCertificates(uint64_t slotId) const;
    ListMechanisms getMechanisms(uint64_t slotId) const;

    binary_buf signData(uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
                        const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS);
    binary_buf decryptData(uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
                           const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS);

  protected:
    void run(void) override;
    void stop(void);

    boost::asio::awaitable<void> remoteConnect(void);
    boost::asio::awaitable<bool> updateTokens(void);
    boost::asio::awaitable<void> updateTokensTimer(void);
    boost::asio::awaitable<ListCertificates> loadCertificates(uint64_t slotId) const;
    boost::asio::awaitable<ListMechanisms> loadMechanisms(uint64_t slotId) const;
    boost::asio::awaitable<binary_buf> loadSignData(uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
            const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS);
    boost::asio::awaitable<binary_buf> loadDecryptData(uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
            const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS);

  Q_SIGNALS:
    void pkcs11Error(const QString &);
    void pkcs11Shutdown(void);
    void pkcs11TokensChanged(void);
};

#endif // LTSM_HELPER_PKCS11_H
