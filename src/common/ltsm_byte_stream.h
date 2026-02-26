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

#ifndef _LTSM_BYTE_STREAM_
#define _LTSM_BYTE_STREAM_

#include <istream>
#include <ostream>
#include <cassert>
#include <vector>

#include <boost/endian.hpp>

namespace byte {
    class ostream : public std::ostream {
        std::ostream & st;

      protected:
        template<typename T>
        ostream & write_be(T val) {
            boost::endian::native_to_big_inplace(val);
            st.write(reinterpret_cast<const char*>(&val), sizeof(val));
            assert(st.good());
            return *this;
        }

        template<typename T>
        ostream & write_le(T val) {
            boost::endian::native_to_little_inplace(val);
            st.write(reinterpret_cast<const char*>(&val), sizeof(val));
            assert(st.good());
            return *this;
        }

      public:
        ostream(std::ostream & os) : st(os) {}

        inline ostream & write_be64(uint64_t val) {
            return write_be<uint64_t>(val);
        }
        inline ostream & write_le64(uint64_t val) {
            return write_le<uint64_t>(val);
        }
        inline ostream & write_be32(uint32_t val) {
            return write_be<uint32_t>(val);
        }
        inline ostream & write_le32(uint32_t val) {
            return write_le<uint32_t>(val);
        }
        inline ostream & write_be16(uint16_t val) {
            return write_be<uint16_t>(val);
        }
        inline ostream & write_le16(uint16_t val) {
            return write_le<uint16_t>(val);
        }
        inline ostream & write_byte(uint8_t val) {
            st.put(val);
            assert(st.good());
            return *this;
        }
        template<typename T>
        ostream & write_bytes(const std::vector<T> & buf) {
            st.write(reinterpret_cast<const char*>(buf.data()), buf.size());
            assert(st.good());
            return *this;
        }
        ostream & write_string(std::string_view str) {
            st.write(str.data(), str.size());
            assert(st.good());
            return *this;
        }
    };

    class istream : public std::istream {
        std::istream & st;

      protected:
        template<typename T>
        T read_be(void) {
            T val;
            st.read(reinterpret_cast<char*>(&val), sizeof(val));
            assert(st.gcount() == sizeof(val));
            return boost::endian::native_to_big(val);
        }

        template<typename T>
        T read_le(void) {
            T val;
            st.read(reinterpret_cast<char*>(&val), sizeof(val));
            assert(st.gcount() == sizeof(val));
            return boost::endian::native_to_little(val);
        }

      public:
        istream(std::istream & is) : st(is) {}

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
        inline uint8_t  read_byte(void) {
            auto val = st.get();
            assert(st.gcount() == 1);
            return val;
        }
        std::vector<uint8_t> read_bytes(size_t len) {
            std::vector<uint8_t> buf(len);
            st.read(reinterpret_cast<char*>(buf.data()), buf.size());
            assert(st.gcount() == buf.size());
            return buf;
        }
        std::string read_string(size_t len) {
            std::string buf(len, 0);
            st.read(buf.data(), buf.size());
            assert(st.gcount() == buf.size());
            return buf;
        }
    };
}

#endif
