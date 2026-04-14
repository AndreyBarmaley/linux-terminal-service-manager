/***********************************************************************
 *   Copyright © 2026 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_BOOST_SOCKETS_
#define _LTSM_BOOST_SOCKETS_

#include "ltsm_sockets.h"

#include <boost/asio.hpp>

namespace LTSM {
    template <typename Stream>
    class BoostStream : public NetworkStream {
        mutable Stream stream_;

      public:
        explicit BoostStream(Stream && st) : stream_{std::move(st)} {}

        bool hasInput(void) const override {
            return NetworkStream::hasInput(stream_.native_handle());
        }

        size_t hasData(void) const override {
            return stream_.available();
        }

        uint8_t peekInt8(void) const override {
            uint8_t res;
            stream_.receive(boost::asio::buffer(&res, 1),
                boost::asio::ip::tcp::socket::message_peek);
            return res;
        }

#ifdef LTSM_WITH_GNUTLS
        void setupTLS(gnutls::session* sess) const override {
            sess->set_transport_ptr(reinterpret_cast<gnutls_transport_ptr_t>(stream_.native_handle()));
        }
#endif

        void sendRaw(const void* ptr, size_t len) override {
            boost::asio::write(stream_, boost::asio::const_buffer(ptr, len), boost::asio::transfer_all());
        }

        void recvRaw(void* ptr, size_t len) const override {
            boost::asio::read(stream_, boost::asio::buffer(ptr, len), boost::asio::transfer_all());
        }
    };

} // LTSM

#endif // _LTSM_BOOST_SOCKETS_
