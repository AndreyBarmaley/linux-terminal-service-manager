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

#include <stdexcept>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_streambuf.h"

namespace LTSM
{
    std::string ByteArray::hexstring(const std::string & sep, bool prefix) const
    {
        return Tools::buffer2hexstring<uint8_t>(data(), size(), 2);
    }

    /* BinaryBuf */
    BinaryBuf & BinaryBuf::append(const std::string & s)
    {
        insert(end(), s.begin(), s.end());
        return *this;
    }

    std::string BinaryBuf::tostring(void) const
    {
        return std::string(begin(), end());
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

    uint32_t BinaryBuf::crc32b(void) const
    {
        return Tools::crc32b(data(), size());
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
#endif
        return v;
    }

    uint32_t ByteOrderInterface::getIntLE32(void) const
    {
        uint32_t v;
        getRaw(& v, 4);
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        return swap32(v);
#endif
        return v;
    }

    uint64_t ByteOrderInterface::getIntLE64(void) const
    {
        uint64_t v;
        getRaw(& v, 8);
#if (__BYTE_ORDER__==__ORDER_BIG_ENDIAN__)
        return swap64(v);
#endif
        return v;
    }

    uint16_t ByteOrderInterface::getIntBE16(void) const
    {
        uint16_t v;
        getRaw(& v, 2);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap16(v);
#endif
        return v;
    }

    uint32_t ByteOrderInterface::getIntBE32(void) const
    {
        uint32_t v;
        getRaw(& v, 4);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap32(v);
#endif
        return v;
    }

    uint64_t ByteOrderInterface::getIntBE64(void) const
    {
        uint64_t v;
        getRaw(& v, 8);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return swap64(v);
#endif
        return v;
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

    MemoryStream & MemoryStream::write(const std::string & v)
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

    MemoryStream & MemoryStream::operator<<(const std::string & v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const std::vector<uint8_t> & v)
    {
        putRaw(v.data(), v.size());
        return *this;
    }

    /* StreamBufRef */
    StreamBufRef::StreamBufRef(const std::vector<uint8_t> & v) : it1(v.begin()), it2(v.end())
    {
    }

    void StreamBufRef::reset(const std::vector<uint8_t> & v)
    {
        it1 = v.begin();
        it2 = v.end();
    }

    void StreamBufRef::getRaw(void* ptr, size_t len) const
    {
        if(last() < len)
            throw std::out_of_range("StreamBufRef: getRaw");

        auto dst = static_cast<uint8_t*>(ptr);

        std::copy_n(it1, len, dst);
        it1 = std::next(it1, len);
    }

    void StreamBufRef::putRaw(const void* ptr, size_t len)
    {
        throw std::runtime_error("StreamBufRef: putRaw disabled");
    }

    BinaryBuf StreamBufRef::read(size_t len) const
    {
        if(last() < len)
            throw std::out_of_range(std::string("StreamBufRef: read len: ").append(std::to_string(len)));

        if(len == 0)
            len = last();

        auto it0 = it1;
        it1 = std::next(it1, len);

        return BinaryBuf(it0, it1);
    }

    void StreamBufRef::skip(size_t len) const
    {
        if(last() < len)
            throw std::out_of_range(std::string("StreamBufRef: skip len: ").append(std::to_string(len)));

        it1 = std::next(it1, len);
    }

    size_t StreamBufRef::last(void) const
    {
        return std::distance(it1, it2);
    }

    uint8_t StreamBufRef::peek(void) const
    {
        if(it1 == it2)
            throw std::out_of_range("StreamBufRef: peek");

        return *it1;
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

    void StreamBuf::reset(const std::vector<uint8_t> & v)
    {
        vec.assign(v.begin(), v.end());
        it = vec.begin();
    }

    void StreamBuf::getRaw(void* ptr, size_t len) const
    {
        if(last() < len)
            throw std::out_of_range("StreamBuf: getRaw");

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
            throw std::out_of_range(std::string("StreamBuf: read len: ").append(std::to_string(len)));

        if(len == 0)
            len = last();

        auto it0 = it;
        it = std::next(it, len);

        return BinaryBuf(it0, it);
    }

    void StreamBuf::skip(size_t len) const
    {
        if(len > last())
            throw std::out_of_range(std::string("StreamBuf: skip len: ").append(std::to_string(len)));

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
            throw std::out_of_range("StreamBuf: peek");

        return *it;
    }

    const BinaryBuf & StreamBuf::rawbuf(void) const
    {
        return vec;
    }
}
