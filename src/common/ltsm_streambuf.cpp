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

    /* ByteOrderInterface */
    uint16_t ByteOrderInterface::getIntLE16(void) const
    {
        uint16_t v1 = getInt8();
        uint16_t v2 = getInt8();
        return v1 | (v2 << 8);
    }

    uint32_t ByteOrderInterface::getIntLE32(void) const
    {
        uint32_t v1 = getIntLE16();
        uint32_t v2 = getIntLE16();
        return v1 | (v2 << 16);
    }

    uint64_t ByteOrderInterface::getIntLE64(void) const
    {
        uint64_t v1 = getIntLE32();
        uint64_t v2 = getIntLE32();
        return v1 | (v2 << 32);
    }

    uint16_t ByteOrderInterface::getIntBE16(void) const
    {
        uint16_t v = getInt8();
        return (v << 8) | getInt8();
    }

    uint32_t ByteOrderInterface::getIntBE32(void) const
    {
        uint32_t v = getIntBE16();
        return (v << 16) | getIntBE16();
    }

    uint64_t ByteOrderInterface::getIntBE64(void) const
    {
        uint64_t v = getIntBE32();
        return (v << 32) | getIntBE32();
    }

    void ByteOrderInterface::putIntLE16(uint16_t v)
    {
        putInt8(0x00FF & v);
        putInt8(0x00FF & (v >> 8));
    }

    void ByteOrderInterface::putIntLE32(uint32_t v)
    {
        putIntLE16(0x0000FFFF & v);
        putIntLE16(0x0000FFFF & (v >> 16));
    }

    void ByteOrderInterface::putIntLE64(uint64_t v)
    {
        putIntLE32(0xFFFFFFFF & v);
        putIntLE32(0xFFFFFFFF & (v >> 32));
    }

    void ByteOrderInterface::putIntBE16(uint16_t v)
    {
        putInt8(0x00FF & (v >> 8));
        putInt8(0x00FF & v);
    }

    void ByteOrderInterface::putIntBE32(uint32_t v)
    {
        putIntBE16(0x0000FFFF & (v >> 16));
        putIntBE16(0x0000FFFF & v);
    }

    void ByteOrderInterface::putIntBE64(uint64_t v)
    {
        putIntBE32(0xFFFFFFFF & (v >> 32));
        putIntBE32(0xFFFFFFFF & v);
    }

    /* MemoryStream */
    bool MemoryStream::bigendian(void) const
    {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
        return false;
#else
        return true;
#endif
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
        while(len--)
        {
            *ptr = readInt8();
            ptr++;
        }
    }

    void MemoryStream::readTo(uint8_t* ptr, size_t len) const
    {
        while(len--)
        {
            *ptr = readInt8();
            ptr++;
        }
    }

    BinaryBuf MemoryStream::read(size_t len) const
    {
        BinaryBuf v(len);

        for(auto & c : v)
            c = readInt8();

        return v;
    }

    void  MemoryStream::skip(size_t len) const
    {
        while(len--)
            readInt8();
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

    const MemoryStream & MemoryStream::operator>>(std::string & v) const
    {
        auto len = last();

        while(len--)
            v.push_back(readInt8());

        return *this;
    }

    const MemoryStream & MemoryStream::operator>>(std::vector<uint8_t> & v) const
    {
        auto len = last();

        while(len--)
            v.push_back(readInt8());

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

    void MemoryStream::write(const char* ptr, size_t len)
    {
        for(size_t pos = 0; pos < len; ++pos)
            writeInt8(ptr[pos]);
    }

    void MemoryStream::write(const uint8_t* ptr, size_t len)
    {
        for(size_t pos = 0; pos < len; ++pos)
            writeInt8(ptr[pos]);
    }

    void MemoryStream::write(const std::string & v)
    {
        for(auto & c : v)
            writeInt8(c);
    }

    void MemoryStream::write(size_t len, uint8_t c)
    {
        while(len--)
            writeInt8(c);
    }

    void MemoryStream::write(const std::vector<uint8_t> & v)
    {
        for(auto & c : v)
            writeInt8(c);
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
        write(v);
        return *this;
    }

    MemoryStream & MemoryStream::operator<<(const std::vector<uint8_t> & v)
    {
        write(v);
        return *this;
    }

    /* StreamBufRef */
    StreamBufRef::StreamBufRef(const std::vector<uint8_t> & v) : it1(v.begin()), it2(v.end())
    {
    }

    void StreamBufRef::setBuffer(const std::vector<uint8_t> & v)
    {
        it1 = v.begin();
        it2 = v.end();
    }

    uint8_t StreamBufRef::getInt8(void) const
    {
        if(it1 == it2)
            throw std::out_of_range(std::string("StreamBufRef: readInt8"));

        uint8_t v = *it1;
        it1++;
        return v;
    }

    BinaryBuf StreamBufRef::read(size_t len) const
    {
        if(len == 0)
            return BinaryBuf(it1, it2);

        if(len > last())
            throw std::out_of_range(std::string("StreamBufRef: read len: ").append(std::to_string(len)));

        return BinaryBuf(it1, std::next(it1, len));
    }

    size_t StreamBufRef::last(void) const
    {
        return std::distance(it1, it2);
    }

    uint8_t StreamBufRef::peek(void) const
    {
        if(it1 == it2)
            throw std::out_of_range(std::string("StreamBufRef: peek"));

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

    uint8_t StreamBuf::getInt8(void) const
    {
        if(it == vec.end())
            throw std::out_of_range(std::string("StreamBuf: read byte"));

        uint8_t v = *it;
        it++;
        return v;
    }

    void StreamBuf::putInt8(uint8_t v)
    {
        auto seek = std::distance(vec.begin(), it);
        vec.push_back(v);
        it = std::next(vec.begin(), seek);
    }

    BinaryBuf StreamBuf::read(size_t len) const
    {
        auto const_it = static_cast<BinaryBuf::const_iterator>(it);

        if(len == 0)
            return BinaryBuf(const_it, vec.end());

        if(len > last())
            throw std::out_of_range(std::string("StreamBuf: read len: ").append(std::to_string(len)));

        return BinaryBuf(const_it, std::next(const_it, len));
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
            throw std::out_of_range(std::string("StreamBuf: peek"));

        return *it;
    }

    const BinaryBuf & StreamBuf::rawbuf(void) const
    {
        return vec;
    }
}
