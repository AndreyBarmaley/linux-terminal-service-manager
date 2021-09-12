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

#ifndef _LTSM_STREAMBUF_
#define _LTSM_STREAMBUF_

#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace LTSM
{
    /// @brief: byte array interface
    class ByteArray
    {
    public:
        virtual ~ByteArray() {}

        virtual size_t  size(void) const = 0;

        virtual uint8_t* data(void) = 0;
        virtual const uint8_t* data(void) const = 0;

        std::string     hexstring(const std::string & sep = ", ", bool prefix = true) const;
    };

    /// @brief: raw array wrapper
    template<typename T>
    struct RawPtr :  ByteArray, std::pair<T*, size_t>
    {
        RawPtr(T* ptr, size_t len) : std::pair<T *, size_t>(ptr, len) {}

        template<size_t N>
        RawPtr(T(&arr)[N]) : std::pair<T *, size_t>(arr, N) {}


        size_t size(void) const override
        {
            return std::pair<T*, size_t>::second * sizeof(T);
        }

        uint8_t* data(void) override
        {
            return (uint8_t*) std::pair<T*, size_t>::first;
        }

        const uint8_t* data(void) const override
        {
            return (const uint8_t*) std::pair<T*, size_t>::first;
        }
    };

    /// @brief: extend binary vector
    struct BinaryBuf : ByteArray, std::vector<uint8_t>
    {
        BinaryBuf() {}
        BinaryBuf(size_t len, uint8_t val = 0) : std::vector<uint8_t>(len, val) {}
        BinaryBuf(const_iterator it1, const_iterator it2) : std::vector<uint8_t>(it1, it2) {}
        BinaryBuf(const uint8_t* ptr, size_t len) : std::vector<uint8_t>(ptr, ptr + len) {}
        BinaryBuf(const std::vector<uint8_t> & v) : std::vector<uint8_t>(v) {}

        template<size_t N>
        BinaryBuf(uint8_t (&arr)[N]) : std::vector<uint8_t>(arr, arr + N) {}

        BinaryBuf(std::vector<uint8_t> && v) noexcept
        {
            swap(v);
        }

        BinaryBuf &     operator= (const std::vector<uint8_t> & v)
        {
            assign(v.begin(), v.end());
            return *this;
        }
        BinaryBuf &     operator= (std::vector<uint8_t> && v) noexcept
        {
            swap(v);
            return *this;
        }

        BinaryBuf &     append(const uint8_t*, size_t);
        BinaryBuf &     append(const std::vector<uint8_t> &);
        BinaryBuf &     append(const std::string &);
        std::string     tostring(void) const;
        BinaryBuf       copy(void) const;

        size_t          size(void) const override
        {
            return std::vector<uint8_t>::size();
        }

        uint8_t*        data(void) override
        {
            return std::vector<uint8_t>::data();
        }
        const uint8_t*  data(void) const override
        {
            return std::vector<uint8_t>::data();
        }
    };

    /// @brief: base stream interface
    class ByteOrderInterface
    {
    public:
	virtual ~ByteOrderInterface() {}

        virtual uint8_t getInt8(void) const = 0;
        virtual void    putInt8(uint8_t) = 0;

        uint16_t        getIntLE16(void) const;
        uint32_t        getIntLE32(void) const;
        uint64_t        getIntLE64(void) const;

        uint16_t        getIntBE16(void) const;
        uint32_t        getIntBE32(void) const;
        uint64_t        getIntBE64(void) const;

        void            putIntLE16(uint16_t);
        void            putIntLE32(uint32_t);
        void            putIntLE64(uint64_t);

        void            putIntBE16(uint16_t);
        void            putIntBE32(uint32_t);
        void            putIntBE64(uint64_t);
    };

    /// @brief: base Stream class
    class MemoryStream : protected ByteOrderInterface
    {
    public:
        /// @brief: get last count
        virtual size_t  last(void) const = 0;
        /// @brief: peek from front
        virtual uint8_t peek(void) const = 0;
        /// @brief: set endian mode (default: system)
        virtual bool    bigendian(void) const;

        inline uint8_t  readInt8(void) const { return getInt8(); }
        inline void     writeInt8(uint8_t v) { putInt8(v); }

        /// @brief: read uint16 (depends on current endian mode)
        uint16_t        readInt16(void) const;
        /// @brief: read uint32 (depends on current endian mode)
        uint32_t        readInt32(void) const;
        /// @brief: read uint64 (depends on current endian mode)
        uint64_t        readInt64(void) const;

        inline uint16_t readIntLE16(void) const { return getIntLE16(); }
        inline uint32_t readIntLE32(void) const { return getIntLE32(); }
        inline uint64_t readIntLE64(void) const { return getIntLE64(); }

        inline uint16_t readIntBE16(void) const { return getIntBE16(); }
        inline uint32_t readIntBE32(void) const { return getIntBE32(); }
        inline uint64_t readIntBE64(void) const { return getIntBE64(); };

        void            readIn(char*, size_t) const;
        void            readIn(uint8_t*, size_t) const;

        virtual BinaryBuf read(size_t = 0) const;
        void            skip(size_t) const;

        const MemoryStream & operator>>(uint8_t &) const;
        const MemoryStream & operator>>(uint16_t &) const;
        const MemoryStream & operator>>(uint32_t &) const;
        const MemoryStream & operator>>(uint64_t &) const;

        /// @brief: read all data from stream to string
        const MemoryStream & operator>>(std::string &) const;
        /// @brief: read all data from stream to vector
        const MemoryStream & operator>>(std::vector<uint8_t> &) const;

        /// @brief: fixed data to array
        template<typename T>
        const MemoryStream & operator>>(const RawPtr<T> & v) const
        {
            readIn(v.first, v.size());
            return *this;
        }

        template<size_t N>
        const MemoryStream & operator>>(uint8_t (&arr)[N]) const
        {
            readIn(arr, N);
            return *this;
        }

        /// @brief: write uint16 (depends on current endian mode)
        void            writeInt16(uint16_t);
        /// @brief: write uint32 (depends on current endian mode)
        void            writeInt32(uint32_t);
        /// @brief: write uint64 (depends on current endian mode)
        void            writeInt64(uint64_t);

        inline void     writeIntLE16(uint16_t v) { putIntLE16(v); }
        inline void     writeIntLE32(uint32_t v) { putIntLE32(v); }
        inline void     writeIntLE64(uint64_t v) { putIntLE64(v); }

        inline void     writeIntBE16(uint16_t v) { putIntBE16(v); }
        inline void     writeIntBE32(uint32_t v) { putIntBE32(v); }
        inline void     writeIntBE64(uint64_t v) { putIntBE64(v); }

        void            write(const char*, size_t);
        void            write(const uint8_t*, size_t);
        void            write(const std::string &);
        void            write(const std::vector<uint8_t> &);
        /// @brief: fill version
        void            write(size_t, uint8_t);

        MemoryStream &  operator<<(const uint8_t &);
        MemoryStream &  operator<<(const uint16_t &);
        MemoryStream &  operator<<(const uint32_t &);
        MemoryStream &  operator<<(const uint64_t &);
        MemoryStream &  operator<<(const std::string &);
        MemoryStream &  operator<<(const std::vector<uint8_t> &);

        /// @brief: fixed data from array
        template<typename T>
        MemoryStream & operator<<(const RawPtr<T> & v)
        {
            write(v.data(), v.size());
            return *this;
        }

        template<size_t N>
        MemoryStream & operator<<(const uint8_t (&arr)[N])
        {
            write(arr, N);
            return *this;
        }
    };

    /// @brief: read only StreamBuf
    class StreamBufRef : public MemoryStream
    {
        mutable std::vector<uint8_t>::const_iterator it1;
        std::vector<uint8_t>::const_iterator it2;

    protected:
        void            putInt8(uint8_t) override {}
        uint8_t         getInt8(void) const override;

    public:
        StreamBufRef() {}
        StreamBufRef(const std::vector<uint8_t> &);

        bool            bigendian(void) const override
        {
            return false;
        }
        void            setBuffer(const std::vector<uint8_t> &);

        BinaryBuf       read(size_t = 0) const override;
        size_t          last(void) const override;
        uint8_t         peek(void) const override;
    };

    /// @brief: read/write StreamBuf
    class StreamBuf : public MemoryStream
    {
        mutable BinaryBuf::iterator it;
        BinaryBuf       vec;

    protected:
	uint8_t         getInt8(void) const override;
        void            putInt8(uint8_t) override;

    public:
        StreamBuf(size_t reserve = 32);
        StreamBuf(const std::vector<uint8_t> &);

        bool            bigendian(void) const override { return false; }

        BinaryBuf       read(size_t = 0) const override;
        size_t          last(void) const override;
        uint8_t         peek(void) const override;

        size_t          tell(void) const;
        const BinaryBuf & rawbuf(void) const;
    };

} // _LTSM_STREAMBUF_

#endif