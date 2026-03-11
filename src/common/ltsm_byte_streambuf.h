/***********************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_BYTE_STREAMBUF_
#define _LTSM_BYTE_STREAMBUF_

#include <string>
#include <vector>
#include <cassert>

#include <boost/endian.hpp>
#include <boost/asio/streambuf.hpp>

namespace byte {
    class streambuf {
        boost::asio::streambuf & sb_;

      protected:
        template<typename T>
        streambuf & write_be(T val) {
            auto buf = sb_.prepare(sizeof(T));
            assert(sizeof(T) <= boost::asio::buffer_size(buf));
            boost::endian::endian_store<T, sizeof(T), boost::endian::order::big>((unsigned char*) buf.data(), val);
            sb_.commit(sizeof(T));
            return *this;
        }

        template<typename T>
        streambuf & write_le(T val) {
            auto buf = sb_.prepare(sizeof(T));
            assert(sizeof(T) <= boost::asio::buffer_size(buf));
            boost::endian::endian_store<T, sizeof(T), boost::endian::order::little>((unsigned char*) buf.data(), val);
            sb_.commit(sizeof(T));
            return *this;
        }

        template<typename T>
        T read_be(void) {
            auto buf = sb_.data();
            assert(sizeof(T) <= boost::asio::buffer_size(buf));
            auto val = boost::endian::endian_load<T, sizeof(T), boost::endian::order::big>((const unsigned char*) buf.data());
            sb_.consume(sizeof(T));
            return val;
        }

        template<typename T>
        T read_le(void) {
            auto buf = sb_.data();
            assert(sizeof(T) <= boost::asio::buffer_size(buf));
            auto val = boost::endian::endian_load<T, sizeof(T), boost::endian::order::little>((const unsigned char*) buf.data());
            sb_.consume(sizeof(T));
            return val;
        }

      public:
        streambuf(boost::asio::streambuf & sb) : sb_(sb) {}

        inline streambuf & write_be64(uint64_t val) {
            return write_be<uint64_t>(val);
        }
        inline streambuf & write_le64(uint64_t val) {
            return write_le<uint64_t>(val);
        }
        inline streambuf & write_be32(uint32_t val) {
            return write_be<uint32_t>(val);
        }
        inline streambuf & write_le32(uint32_t val) {
            return write_le<uint32_t>(val);
        }
        inline streambuf & write_be16(uint16_t val) {
            return write_be<uint16_t>(val);
        }
        inline streambuf & write_le16(uint16_t val) {
            return write_le<uint16_t>(val);
        }

        streambuf & write_bytes(const uint8_t* ptr, size_t len) {
            if(len) {
                auto buf = sb_.prepare(len);
                assert(len <= boost::asio::buffer_size(buf));
                boost::asio::buffer_copy(buf, boost::asio::buffer(ptr, len));
                sb_.commit(len);
            }
            return *this;
        }

        streambuf & write_string(std::string_view val) {
            return write_bytes(reinterpret_cast<const uint8_t*>(val.data()), val.size());
        }

        streambuf & write_bytes(const std::vector<uint8_t> & val) {
            return write_bytes(val.data(), val.size());
        }

        inline uint64_t read_be64(void) {
            return read_be<uint64_t>();
        }
        inline uint64_t read_le64(void) {
            return read_le<uint64_t>();
        }
        inline uint32_t read_be32(void) {
            return read_be<uint32_t>();
        }
        inline uint32_t read_le32(void) {
            return read_le<uint32_t>();
        }
        inline uint16_t read_be16(void) {
            return read_be<uint16_t>();
        }
        inline uint16_t read_le16(void) {
            return read_le<uint16_t>();
        }

        void skip_bytes(size_t len) {
            sb_.consume(len);
        }

        template<typename Buffer>
        Buffer read_buffer(size_t len) {
            if(len) {
                auto buf = sb_.data();
                assert(len <= boost::asio::buffer_size(buf));
                Buffer res(len, 0);
                boost::asio::buffer_copy(boost::asio::buffer(res), buf);
                sb_.consume(len);
                return res;
            }
            return {};
        }

        std::string read_string(size_t len) {
            return read_buffer<std::string>(len);
        }

        std::vector<uint8_t> read_bytes(size_t len) {
            return read_buffer<std::vector<uint8_t>>(len);
        }
    };
}

#endif
