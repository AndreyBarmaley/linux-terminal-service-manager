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
#include <string_view>
#include <stdexcept>
#include <vector>
#include <utility>
#include <cstdint>

#define LTSM_STREAMBUF_VERSION 20240810

namespace LTSM
{
    /// @brief: byte array interface
    class ByteArray
    {
    public:
        virtual ~ByteArray() = default;

        virtual size_t size(void) const = 0;

        virtual uint8_t* data(void) = 0;
        virtual const uint8_t* data(void) const = 0;

        std::string hexString(std::string_view sep = ", ", bool prefix = true) const;
        std::string toString(void) const;

        uint32_t crc32b(void) const;

        bool operator== (const ByteArray &) const;
        bool operator!= (const ByteArray &) const;
    };

    /// @brief: raw array wrapper
    template<typename T>
    struct RawPtr : ByteArray, std::pair<T*, size_t>
    {
        RawPtr(T* ptr, size_t len) : std::pair<T*, size_t>(ptr, len) {}

        template<size_t N>
        explicit RawPtr(T(&arr)[N]) : std::pair<T*, size_t>(arr, N) {}


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
        BinaryBuf() = default;

        BinaryBuf(size_t len, uint8_t val = 0) : std::vector<uint8_t>(len, val) {}

        BinaryBuf(const_iterator it1, const_iterator it2) : std::vector<uint8_t>(it1, it2) {}

        BinaryBuf(const uint8_t* ptr, size_t len) : std::vector<uint8_t>(ptr, ptr + len) {}

        explicit BinaryBuf(const std::vector<uint8_t> & v) : std::vector<uint8_t>(v) {}

        explicit BinaryBuf(std::vector<uint8_t> && v) noexcept { swap(v); }

        template<size_t N>
        explicit BinaryBuf(uint8_t (&arr)[N]) : std::vector<uint8_t>(arr, arr + N) {}


        BinaryBuf & operator= (const std::vector<uint8_t> & v) { assign(v.begin(), v.end()); return *this; }

        BinaryBuf & operator= (const ByteArray & v) { assign(v.data(), v.data() + v.size()); return *this; }

        BinaryBuf & operator= (std::vector<uint8_t> && v) noexcept { swap(v); return *this; }

        BinaryBuf & append(const uint8_t*, size_t);
        BinaryBuf & append(const std::vector<uint8_t> &);
        BinaryBuf & append(std::string_view);
        BinaryBuf copy(void) const;

        size_t size(void) const override;
        uint8_t* data(void) override;
        const uint8_t* data(void) const override;
    };

    /// @brief: base stream interface
    class ByteOrderInterface
    {
    protected:
        virtual void getRaw(void* ptr, size_t len) const = 0;
        virtual void putRaw(const void* ptr, size_t len) = 0;

    public:
        virtual ~ByteOrderInterface() = default;

        uint8_t getInt8(void) const;
        void putInt8(uint8_t);

        uint16_t getIntLE16(void) const;
        uint32_t getIntLE32(void) const;
        uint64_t getIntLE64(void) const;

        uint16_t getIntBE16(void) const;
        uint32_t getIntBE32(void) const;
        uint64_t getIntBE64(void) const;

        void putIntLE16(uint16_t);
        void putIntLE32(uint32_t);
        void putIntLE64(uint64_t);

        void putIntBE16(uint16_t);
        void putIntBE32(uint32_t);
        void putIntBE64(uint64_t);

        static inline uint16_t swap16(uint16_t x) { return __builtin_bswap16(x); }

        static inline uint32_t swap32(uint32_t x) { return __builtin_bswap32(x); }

        static inline uint64_t swap64(uint64_t x) { return __builtin_bswap64(x); }
    };

    /// @brief: base Stream class
    class MemoryStream : protected ByteOrderInterface
    {
    public:
        /// @brief: get last count
        virtual size_t last(void) const = 0;
        /// @brief: peek from front
        virtual uint8_t peek(void) const = 0;
        /// @brief: set endian mode (default: system)
        virtual bool bigendian(void) const;

        inline uint8_t readInt8(void) const { return getInt8(); }

        inline void writeInt8(uint8_t v) { putInt8(v); }

        /// @brief: read uint16 (depends on current endian mode)
        uint16_t readInt16(void) const;
        /// @brief: read uint32 (depends on current endian mode)
        uint32_t readInt32(void) const;
        /// @brief: read uint64 (depends on current endian mode)
        uint64_t readInt64(void) const;

        inline uint16_t readIntLE16(void) const { return getIntLE16(); }

        inline uint32_t readIntLE32(void) const { return getIntLE32(); }

        inline uint64_t readIntLE64(void) const { return getIntLE64(); }

        inline uint16_t readIntBE16(void) const { return getIntBE16(); }

        inline uint32_t readIntBE32(void) const { return getIntBE32(); }

        inline uint64_t readIntBE64(void) const { return getIntBE64(); };

        void readTo(void*, size_t) const;

        virtual BinaryBuf read(size_t = 0) const = 0;
        std::string readString(size_t = 0) const;
        virtual void skip(size_t) const = 0;

        const MemoryStream & operator>>(uint8_t &) const;
        const MemoryStream & operator>>(uint16_t &) const;
        const MemoryStream & operator>>(uint32_t &) const;
        const MemoryStream & operator>>(uint64_t &) const;

        /// @brief: read all data from stream to vector
        const MemoryStream & operator>>(std::vector<uint8_t> &) const;

        /// @brief: fixed data to array
        template<typename T>
        const MemoryStream & operator>>(const RawPtr<T> & v) const
        {
            readTo(v.first, v.size());
            return *this;
        }

        template<size_t N>
        const MemoryStream & operator>>(uint8_t (&arr)[N]) const
        {
            readTo(arr, N);
            return *this;
        }

        /// @brief: write uint16 (depends on current endian mode)
        MemoryStream & writeInt16(uint16_t);
        /// @brief: write uint32 (depends on current endian mode)
        MemoryStream & writeInt32(uint32_t);
        /// @brief: write uint64 (depends on current endian mode)
        MemoryStream & writeInt64(uint64_t);

        inline MemoryStream & writeIntLE16(uint16_t v) { putIntLE16(v); return *this; }

        inline MemoryStream & writeIntLE32(uint32_t v) { putIntLE32(v); return *this; }

        inline MemoryStream & writeIntLE64(uint64_t v) { putIntLE64(v); return *this; }

        inline MemoryStream & writeIntBE16(uint16_t v) { putIntBE16(v); return *this; }

        inline MemoryStream & writeIntBE32(uint32_t v) { putIntBE32(v); return *this; }

        inline MemoryStream & writeIntBE64(uint64_t v) { putIntBE64(v); return *this; }

        MemoryStream & write(const void*, size_t);
        MemoryStream & write(std::string_view);
        MemoryStream & write(const std::vector<uint8_t> &);
        /// @brief: fill version
        MemoryStream & fill(size_t, char);

        MemoryStream & operator<<(const uint8_t &);
        MemoryStream & operator<<(const uint16_t &);
        MemoryStream & operator<<(const uint32_t &);
        MemoryStream & operator<<(const uint64_t &);
        MemoryStream & operator<<(const std::string_view &);
        MemoryStream & operator<<(const std::vector<uint8_t> &);

        /// @brief: fixed data from array
        template<typename T>
        MemoryStream & operator<<(const RawPtr<T> & v)
        {
            putRaw(v.data(), v.size());
            return *this;
        }

        template<size_t N>
        MemoryStream & operator<<(const uint8_t (&arr)[N])
        {
            putRaw(arr, N);
            return *this;
        }
    };

    struct streambuf_error : public std::runtime_error
    {
        explicit streambuf_error(std::string_view what) : std::runtime_error(what.data()) {}
    };

    /// @brief: read only StreamBuf
    class StreamBufRef : public MemoryStream
    {
        mutable const uint8_t* it1 = nullptr;
        const uint8_t* it2 = nullptr;

    protected:
        void getRaw(void* ptr, size_t len) const override;
        void putRaw(const void* ptr, size_t len) override;

    public:
        StreamBufRef() = default;
        ~StreamBufRef() = default;

        StreamBufRef(const void*, size_t);

        StreamBufRef(StreamBufRef &&) noexcept;
        StreamBufRef & operator=(StreamBufRef &&) noexcept;

        StreamBufRef(const StreamBufRef &) = delete;
        StreamBufRef & operator=(const StreamBufRef &) = delete;

        bool bigendian(void) const override { return false; }

        void reset(const void* ptr, size_t len);

        BinaryBuf read(size_t = 0) const override;
        size_t last(void) const override;
        uint8_t peek(void) const override;
        void skip(size_t) const override;

        const uint8_t* data(void) const;

        uint16_t peekIntLE16(void) const;
        uint16_t peekIntBE16(void) const;
        uint32_t peekIntLE32(void) const;
        uint32_t peekIntBE32(void) const;
        uint64_t peekIntLE64(void) const;
        uint64_t peekIntBE64(void) const;
    };

    /// @brief: read/write StreamBuf
    class StreamBuf : public MemoryStream
    {
        mutable BinaryBuf::iterator it;
        BinaryBuf vec;

    protected:
        void getRaw(void* ptr, size_t len) const override;
        void putRaw(const void* ptr, size_t len) override;

    public:
        ~StreamBuf() = default;

        explicit StreamBuf(size_t reserve = 256);
        explicit StreamBuf(const std::vector<uint8_t> &);
        explicit StreamBuf(std::vector<uint8_t> &&) noexcept;

        StreamBuf(const StreamBuf &);
        StreamBuf & operator=(const StreamBuf &);

        StreamBuf(StreamBuf &&) noexcept;
        StreamBuf & operator=(StreamBuf &&) noexcept;

        bool bigendian(void) const override { return false; }

        void reset(void);
        void reset(const std::vector<uint8_t> &);

        BinaryBuf read(size_t = 0) const override;
        size_t last(void) const override;
        uint8_t peek(void) const override;
        void skip(size_t) const override;

        size_t tell(void) const;
        const uint8_t* data(void) const;

        const BinaryBuf & rawbuf(void) const;
        BinaryBuf & rawbuf(void);

        void shrink(void);
    };

    /// @brief: descriptor stream
    class DescriptorStream : public ByteOrderInterface
    {
        int fd = 0;
        bool autoClose = true;

    protected:
        void getRaw(void* ptr, size_t len) const override;
        void putRaw(const void* ptr, size_t len) override;

    public:
        DescriptorStream() = default;

        explicit DescriptorStream(int fd0, bool autoclose);
        ~DescriptorStream();

        DescriptorStream(const DescriptorStream &) = delete;
        DescriptorStream & operator=(const DescriptorStream &) = delete;

        void setDescriptor(int fd0) { fd = fd0; }

        int getDescriptor(void) const { return fd; }

        void readTo(void* ptr, ssize_t len) const;
        void writeFrom(const void* ptr, ssize_t len) const;

        static void readFromTo(int fd, void* ptr, ssize_t len);
        static void writeFromTo(const void* ptr, ssize_t len, int fd);
    };

} // _LTSM_STREAMBUF_

#endif
