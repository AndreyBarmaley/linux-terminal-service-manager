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
#include <mutex>
#include <atomic>
#include <vector>
#include <cinttypes>

#include <QThread>

#include "ltsm_sockets.h"
#include "ltsm_pkcs11_wrapper.h"

struct Pkcs11Token
{
    uint64_t slotId;

    LTSM::PKCS11::SlotInfo slotInfo;
    LTSM::PKCS11::TokenInfo tokenInfo;

    bool operator== ( const Pkcs11Token & st ) const
    {
        return tokenInfo.getModel() == st.tokenInfo.getModel() &&
               tokenInfo.getSerialNumber() == st.tokenInfo.getSerialNumber();
    }

    bool operator< ( const Pkcs11Token & st ) const
    {
        auto model1 = tokenInfo.getModel();
        auto model2 = st.tokenInfo.getModel();

        if( model1 == model2 )
        {
            return tokenInfo.getSerialNumber() < st.tokenInfo.getSerialNumber();
        }

        return model1 < model2;
    }
};

struct Pkcs11Mech
{
    uint64_t mechId;
    uint64_t minKey;
    uint64_t maxKey;
    uint64_t flags;
    std::string name;
};

struct Pkcs11Cert
{
    std::vector<uint8_t> objectId;
    std::vector<uint8_t> objectValue;
};

class Pkcs11Client : public QThread
{
    Q_OBJECT

    LTSM::SocketStream sock;
    QString templatePath;

    mutable std::mutex lock;
    std::atomic<bool> shutdown{false};

    std::list<Pkcs11Token> tokens;

public:
    Pkcs11Client( int displayNum, QObject * );
    ~Pkcs11Client();

    const std::list<Pkcs11Token> & getTokens( void ) const;
    std::list<Pkcs11Cert> getCertificates( uint64_t slotId );
    std::list<Pkcs11Mech> getMechanisms( uint64_t slotId );

    std::vector<uint8_t> signData( uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
                                   const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS );
    std::vector<uint8_t> decryptData( uint64_t slotId, const std::string & pin, const std::vector<uint8_t> & certId,
                                      const void* data, size_t len, uint64_t mechType = CKM_RSA_PKCS );

protected:
    void run( void ) override;

    bool updateTokens( void );

signals:
    void pkcs11Error( const QString & );
    void pkcs11Shutdown( void );
    void pkcs11TokensChanged( void );
};

#endif // LTSM_HELPER_PKCS11_H
