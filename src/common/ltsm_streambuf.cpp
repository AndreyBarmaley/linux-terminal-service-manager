/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#include <cstring>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"

namespace LTSM
{
    bool ByteArray::operator== (const ByteArray & ba) const
    {
        return ba.size() == size() && 0 == std::memcmp(ba.data(), data(), size());
    }

    bool ByteArray::operator!= (const ByteArray & ba) const
    {
        return ba.size() != size() || 0 != std::memcmp(ba.data(), data(), size());
    }

    std::string ByteArray::hexString(std::string_view sep, bool prefix) const
    {
        return Tools::buffer2hexstring<uint8_t>(data(), size(), 2, sep, prefix);
    }

    std::string ByteArray::toString(void) const
    {
        return std::string(data(), data() + size());
    }

    uint32_t ByteArray::crc32b(void) const
    {
        return Tools::crc32b(data(), size());
    }

    /* BinaryBuf */
    BinaryBuf & BinaryBuf::append(std::string_view s)
    {
        insert(end(), s.begin(), s.end());
        return *this;
    }

    BinaryBuf & BinaryBuf::append(const uint8_t* ptr, size_t len)
    {
        insert(end(), ptr, ptr + len);
        return *this;
    }

    BinaryBuf & BinaryBuf::append(const std::vector<uint8_t> & b)
    {
        insert(end(), b.begin(), b.end());
        return *this;
    }

    BinaryBuf BinaryBuf::copy(void) const
    {
        return BinaryBuf(begin(), end());
    }

    size_t BinaryBuf::size(void) const
    {
        return std::vector<uint8_t>::size();
    }

    uint8_t* BinaryBuf::data(void)
    {
        return std::vector<uint8_t>::data();
    }

    const uint8_t* BinaryBuf::data(void) const
    {
        return std::vector<uint8_t>::data();
    }

    /* ByteOrderInterface */
    uint8_t ByteOrderInterface::getInt8(void) const
    {
        uint8_t v;
        getRaw(& v, 1);
        return v;
    }

    void ByteOrderInterface::putInt8(uint8_t v)
    {
        putRaw(& v, 1);
    }

    uint16_t ByteOrderInterface::getIntLE16(void) const
    {
        uint16_t v;
        getRaw(& v, 2);
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        return swap16(v);
#else
        return v;
#endif
    }

    uint32_t ByteOrderInterface::getIntLE32(void) const
    {
        uint32_t v;
        getRaw(& v, 4);
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        return swap32(v);
#else
        return v;
#endif
    }

    uint64_t ByteOrderInterface::getIntLE64(void) const
    {
        uint64_t v;
        getRaw(& v, 8);
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        return swap64(v);
#else
        return v;
#endif
    }

    uint16_t ByteOrderInterface::getIntBE16(void) const
    {
        uint16_t v;
        getRaw(& v, 2);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap16(v);
#else
        return v;
#endif
    }

    uint32_t ByteOrderInterface::getIntBE32(void) const
    {
        uint32_t v;
        getRaw(& v, 4);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap32(v);
#else
        return v;
#endif
    }

    uint64_t ByteOrderInterface::getIntBE64(void) const
    {
        uint64_t v;
        getRaw(& v, 8);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap64(v);
#else
        return v;
#endif
    }

    void ByteOrderInterface::putIntLE16(uint16_t v)
    {
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        v = swap16(v);
#endif
        putRaw(& v, 2);
    }

    void ByteOrderInterface::putIntLE32(uint32_t v)
    {
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        v = swap32(v);
#endif
        putRaw(& v, 4);
    }

    void ByteOrderInterface::putIntLE64(uint64_t v)
    {
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        v = swap64(v);
#endif
        putRaw(& v, 8);
    }

    void ByteOrderInterface::putIntBE16(uint16_t v)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        v = swap16(v);
#endif
        putRaw(& v, 2);
    }

    void ByteOrderInterface::putIntBE32(uint32_t v)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        v = swap32(v);
#endif
        putRaw(& v, 4);
    }

    void ByteOrderInterface::putIntBE64(uint64_t v)
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        v = swap64(v);
#endif
        putRaw(& v, 8);
    }

    /* MemoryStream */
    bool MemoryStream::bigendian(void) const
    {
        return big_endian;
    }

    uint16_t MemoryStream::readInt16(void) const
    {
        return bigendian() ? readIntBE16() : readIntLE16();
    }

    uint32_t MemoryStream::readInt32(void) const
    {
        return bigendian() ? readIntBE32() : readIntLE32();
    }

    uint64_t MemoryStream::readInt64(void) const
    {
        return bigendian() ? readIntBE64() : readIntLE64();
    }

    void MemoryStream::readTo(char* ptr, size_t len) const
    {
        getRaw(ptr, len);
    }

    void MemoryStream::readTo(uint8_t* ptr, size_t len) const
    {
        getRaw(ptr, len);
    }

    const MemoryStream & MemoryStream::operator>>(uint8_t & v) const
    {
        v = readInt8();
        return *this;
    }

    const MemoryStream & MemoryStream::operator>>(uint16_t & v) const
    {
        v = readInt16();
        return *this;
    }

    const MemoryStream & MemoryStream::operator>>(uint32_t & v) const
    {
        v = readInt32();
        return *this;
    }

    const MemoryStream & MemoryStream::operator>>(uint64_t & v) const
    {
        v = readInt64();
        return *this;
    }

    const MemoryStream & MemoryStream::operator>>(std::vector<uint8_t> & v) const
    {
        auto len = last();
        auto begin = v.size();
        v.resize(begin + len);
        getRaw(v.data() + begin, len);
        return *this;
    }

    void MemoryStream::writeInt16(uint16_t v)
    {
        if(bigendian())
            writeIntBE16(v);
        else
            writeIntLE16(v);
    }

    void MemoryStream::writeInt32(uint32_t v)
    {
        if(bigendian())
            writeIntBE32(v);
        else
            writeIntLE32(v);
    }

    void MemoryStream::writeInt64(uint64_t v)
    {
        if(bigendian())
            writeIntBE64(v);
        else
            writeIntLE64(v);
    }

    MemoryStream & MemoryStream::write(const char* ptr, size_t len)
    {
        putRaw(ptr, len);
        return *this;
    }

    MemoryStream & MemoryStream::write(const uint8_t* ptr, size_t len)
    {
        putRaw(ptr, len);
        return *this;
    }

    MemoryStream & MemoryStream::write(std::string_view v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    MemoryStream & MemoryStream::write(const std::vector<uint8_t> & v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    MemoryStream & MemoryStream::fill(size_t len, char c)
    {
        while(len--)
            writeInt8(c);

        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const uint8_t & v)
    {
        writeInt8(v);
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const uint16_t & v)
    {
        writeInt16(v);
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const uint32_t & v)
    {
        writeInt32(v);
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const uint64_t & v)
    {
        writeInt64(v);
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const std::string_view & v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const std::vector<uint8_t> & v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    std::string MemoryStream::readString(size_t len) const
    {
        if(len == 0)
            len = last();
        std::string str(len, 0x20);
        readTo(str.data(), str.size());
        return str;
    }

    /* StreamBufRef */
    StreamBufRef::StreamBufRef(const void* ptr, size_t len)
    {
        if(ptr)
        {
            it1 = (uint8_t*) ptr;
            it2 = it1 + len;
        }
    }

    StreamBufRef::StreamBufRef(StreamBufRef && sb) noexcept : it1(std::move(sb.it1)), it2(std::move(sb.it2))
    {
    }

    StreamBufRef & StreamBufRef::operator=(StreamBufRef && sb) noexcept
    {
        it1 = std::move(sb.it1);
        it2 = std::move(sb.it2);
        return *this;
    }

    void StreamBufRef::reset(const void* ptr, size_t len)
    {
        it1 = (uint8_t*) ptr;
        it2 = it1 + len;
    }

    void StreamBufRef::getRaw(void* ptr, size_t len) const
    {
        if(last() < len)
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        auto dst = static_cast<uint8_t*>(ptr);
        std::copy_n(it1, len, dst);
        it1 = std::next(it1, len);
    }

    void StreamBufRef::putRaw(const void* ptr, size_t len)
    {
        Application::error("%s: %s", __FUNCTION__, "disabled");
        throw streambuf_error(NS_FuncName);
    }

    BinaryBuf StreamBufRef::read(size_t len) const
    {
        if(last() < len)
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        if(len == 0)
            len = last();

        auto it0 = it1;
        it1 = std::next(it1, len);
        return BinaryBuf(it0, len);
    }

    void StreamBufRef::skip(size_t len) const
    {
        if(last() < len)
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        it1 = std::next(it1, len);
    }

    size_t StreamBufRef::last(void) const
    {
        return std::distance(it1, it2);
    }

    uint8_t StreamBufRef::peek(void) const
    {
        if(it1 == it2)
        {
            Application::error("%s: %s", __FUNCTION__, "end stream");
            throw std::out_of_range(NS_FuncName);
        }

        return *it1;
    }

    const uint8_t* StreamBufRef::data(void) const
    {
        return it1;
    }

    /* StreamBuf */
    StreamBuf::StreamBuf(size_t reserve)
    {
        vec.reserve(reserve);
        it = vec.begin();
    }

    StreamBuf::StreamBuf(const std::vector<uint8_t> & v)
    {
        vec.assign(v.begin(), v.end());
        it = vec.begin();
    }

    StreamBuf::StreamBuf(StreamBuf && sb) noexcept : it(std::move(sb.it)), vec(std::move(sb.vec))
    {
    }

    StreamBuf & StreamBuf::operator=(StreamBuf && sb) noexcept
    {
        it = std::move(sb.it);
        vec = std::move(sb.vec);
        return *this;
    }

    StreamBuf::StreamBuf(const StreamBuf & sb)
    {
        vec.assign(sb.vec.begin(), sb.vec.end());
        it = std::next(vec.begin(), sb.tell());
    }

    StreamBuf & StreamBuf::operator=(const StreamBuf & sb)
    {
        vec.assign(sb.vec.begin(), sb.vec.end());
        it = std::next(vec.begin(), sb.tell());
        return *this;
    }

    void StreamBuf::reset(const std::vector<uint8_t> & v)
    {
        vec.assign(v.begin(), v.end());
        it = vec.begin();
    }

    void StreamBuf::getRaw(void* ptr, size_t len) const
    {
        if(last() < len)
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        auto dst = static_cast<uint8_t*>(ptr);
        std::copy_n(it, len, dst);
        it = std::next(it, len);
    }

    void StreamBuf::putRaw(const void* ptr, size_t len)
    {
        auto offset = std::distance(vec.begin(), it);
        auto src = static_cast<const uint8_t*>(ptr);
        auto vsz = vec.size();
        vec.resize(vsz + len);
        auto dst = std::next(vec.begin(), vsz);
        std::copy_n(src, len, dst);
        it = std::next(vec.begin(), offset);
    }

    BinaryBuf StreamBuf::read(size_t len) const
    {
        if(len > last())
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        if(len == 0)
            len = last();

        auto it0 = it;
        it = std::next(it, len);
        return BinaryBuf(it0, it);
    }

    void StreamBuf::skip(size_t len) const
    {
        if(len > last())
        {
            Application::error("%s: incorrect len, last: %u, len: %u", __FUNCTION__, last(), len);
            throw std::invalid_argument(NS_FuncName);
        }

        it = std::next(it, len);
    }

    size_t StreamBuf::last(void) const
    {
        auto const_it = static_cast<BinaryBuf::const_iterator>(it);
        return std::distance(const_it, vec.end());
    }

    size_t StreamBuf::tell(void) const
    {
        auto const_it = static_cast<BinaryBuf::const_iterator>(it);
        return std::distance(vec.begin(), const_it);
    }

    uint8_t StreamBuf::peek(void) const
    {
        if(it == vec.end())
        {
            Application::error("%s: %s", __FUNCTION__, "end stream");
            throw std::out_of_range(NS_FuncName);
        }

        return *it;
    }

    BinaryBuf & StreamBuf::rawbuf(void)
    {
        return vec;
    }

    const BinaryBuf & StreamBuf::rawbuf(void) const
    {
        return vec;
    }

    void StreamBuf::shrink(void)
    {
        if(vec.size())
        {
            if(it == vec.end())
            {
                vec.clear();
                it = vec.begin();
            }
            else
            if(vec.size() > 10 * last())
            {
                std::vector<uint8_t> tmp(it, vec.end());
                vec.swap(tmp);
                it = vec.begin();
            }
        }
    }
}
