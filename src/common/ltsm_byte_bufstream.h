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

#ifndef _LTSM_BYTE_BUFSTREAM_
#define _LTSM_BYTE_BUFSTREAM_

#include <string>
#include <vector>
#include <cassert>

#include <boost/endian.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffered_read_stream.hpp>
#include <boost/asio/buffered_write_stream.hpp>

namespace byte {
    template<typename Sock>
    class wbuf_stream {
        boost::asio::buffered_write_stream<Sock> & st;

      protected:
        template<typename T>
        wbuf_stream & write_be(T val) {
            boost::endian::native_to_big_inplace(val);
            auto gcount = st.write_some(boost::asio::const_buffer(reinterpret_cast<const char*>(&val), sizeof(val)));
            assert(gcount == sizeof(val));
            return *this;
        }

        template<typename T>
        wbuf_stream & write_le(T val) {
            boost::endian::native_to_little_inplace(val);
            auto gcount = st.write_some(boost::asio::const_buffer(reinterpret_cast<const char*>(&val), sizeof(val)));
            assert(gcount == sizeof(val));
            return *this;
        }

      public:
        wbuf_stream(boost::asio::buffered_write_stream<Sock> & os) : st(os) {}

        inline wbuf_stream & write_be64(uint64_t val) {
            return write_be<uint64_t>(val);
        }
        inline wbuf_stream & write_le64(uint64_t val) {
            return write_le<uint64_t>(val);
        }
        inline wbuf_stream & write_be32(uint32_t val) {
            return write_be<uint32_t>(val);
        }
        inline wbuf_stream & write_le32(uint32_t val) {
            return write_le<uint32_t>(val);
        }
        inline wbuf_stream & write_be16(uint16_t val) {
            return write_be<uint16_t>(val);
        }
        inline wbuf_stream & write_le16(uint16_t val) {
            return write_le<uint16_t>(val);
        }
        inline wbuf_stream & write_byte(uint8_t val) {
            return write_bytes(reinterpret_cast<const char*>(&val), 1);
        }
        wbuf_stream & write_bytes(const char* buf, size_t len) {
            auto gcount = st.write_some(boost::asio::const_buffer(buf, len));
            assert(gcount == len);
            return *this;
        }
        template<typename T>
        wbuf_stream & write_bytes(const std::vector<T> & buf) {
            return write_bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
        }
        wbuf_stream & write_string(std::string_view str) {
            return write_bytes(str.data(), str.size());
        }
        void flush(void) {
            st.flush();
        }
    };

    template<typename Sock>
    class rbuf_stream {
        boost::asio::buffered_read_stream<Sock> & st;

      protected:
        template<typename T>
        T read_be(void) {
            T val;
            auto gcount = st.read_some(boost::asio::buffer(reinterpret_cast<char*>(&val), sizeof(val)));
            assert(gcount == sizeof(val));
            return boost::endian::native_to_big(val);
        }

        template<typename T>
        T read_le(void) {
            T val;
            auto gcount = st.read_some(boost::asio::buffer(reinterpret_cast<char*>(&val), sizeof(val)));
            assert(gcount == sizeof(val));
            return boost::endian::native_to_little(val);
        }

      public:
        rbuf_stream(boost::asio::buffered_read_stream<Sock> & is) : st(is) {}

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
        uint8_t read_byte(void) {
            uint8_t val;
            auto gcount = st.read_some(boost::asio::buffer(reinterpret_cast<char*>(&val), 1));
            assert(gcount == 1);
            return val;
        }
        std::vector<uint8_t> read_bytes(size_t len) {
            std::vector<uint8_t> buf(len);
            auto gcount = st.read_some(boost::asio::buffer(reinterpret_cast<char*>(buf.data()), buf.size()));
            assert(gcount == buf.size());
            return buf;
        }
        std::string read_string(size_t len) {
            std::string buf(len, 0);
            auto gcount = st.read_some(boost::asio::buffer(buf.data(), buf.size()));
            assert(gcount == buf.size());
            return buf;
        }
    };
}

#endif
