#include <exception>
#include <iostream>
#include <fstream>
#include <cassert>
//#include <iomanip>
#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_streambuf.h"

using namespace LTSM;

class RandomBuf : public BinaryBuf
{
public:
    RandomBuf(size_t len) : BinaryBuf(len)
    {
        std::ifstream ifs("/dev/urandom", std::ifstream::in | std::ifstream::binary);
        if(! ifs.read(reinterpret_cast<char*>(data()), size()))
            throw std::runtime_error("read failed");
    }
};

void testStreamBufInterface(const BinaryBuf & buf)
{
    std::cout << "== test StreamBufRef interface" << std::endl;

    StreamBufRef sb(buf.data(), buf.size());

    std::cout << "test ::last: ";
    assert(sb.last() == buf.size());
    std::cout << "passed" << std::endl;

    std::cout << "test ::peek: ";
    assert(sb.peek() == buf.front());
    std::cout << "passed" << std::endl;

    std::cout << "test ::readInt8: ";
    for(auto v : buf)
        assert(v == sb.readInt8());
    std::cout << "passed" << std::endl;

    if(true)
    {
        sb.reset(buf.data(), buf.size());
        BinaryBuf res(buf.size());

        std::cout << "test ::readTo: ";
        sb.readTo(res.data(), res.size());

        assert(sb.last() == 0);
        assert(res.crc32b() == buf.crc32b());
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        sb.reset(buf.data(), buf.size());

        std::cout << "test ::read/last: ";
        auto res1 = sb.read();

        assert(sb.last() == 0);
        assert(res1.crc32b() == buf.crc32b());
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        sb.reset(buf.data(), buf.size());

        std::cout << "test ::skip/last: ";
        size_t len = buf.size() / 2;
        sb.skip(len);

        assert(sb.last() == buf.size() - len);
        std::cout << "passed" << std::endl;
    }

    std::cout << "== test StreamBuf interface" << std::endl;

    if(true)
    {
        sb.reset(buf.data(), buf.size());
        StreamBuf sb2(buf.size());

        std::cout << "test ::readInt8/writeInt8: ";
        while(sb.last())
            sb2.writeInt8(sb.readInt8());

        assert(sb2.rawbuf().crc32b() == buf.crc32b());
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        sb.reset(buf.data(), buf.size());
        size_t bufsz = buf.size() - (buf.size() % 2);
        StreamBuf sb2(bufsz);

        std::cout << "test ::readInt16/writeInt16: ";
        while(2 < sb.last())
            sb2.writeInt16(sb.readInt16());

        assert(sb2.rawbuf().crc32b() == Tools::crc32b(buf.data(), bufsz));
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        sb.reset(buf.data(), buf.size());
        size_t bufsz = buf.size() - (buf.size() % 4);
        StreamBuf sb2(bufsz);

        std::cout << "test ::readInt32/writeInt32: ";
        while(4 < sb.last())
            sb2.writeInt32(sb.readInt32());

        assert(sb2.rawbuf().crc32b() == Tools::crc32b(buf.data(), bufsz));
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        sb.reset(buf.data(), buf.size());
        size_t bufsz = buf.size() - (buf.size() % 8);
        StreamBuf sb2(bufsz);

        std::cout << "test ::readInt64/writeInt64: ";
        while(8 < sb.last())
            sb2.writeInt64(sb.readInt64());

        assert(sb2.rawbuf().crc32b() == Tools::crc32b(buf.data(), bufsz));
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        StreamBuf sb2(buf);

        std::cout << "test ::read/last: ";
        auto res1 = sb2.read();

        assert(sb2.last() == 0);
        assert(res1.crc32b() == sb2.rawbuf().crc32b());
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        StreamBuf sb2(buf);

        std::cout << "test ::skip/tell/last: ";
        size_t len = buf.size() / 2;
        sb2.skip(len);

        assert(sb2.tell() == len);
        assert(sb2.last() == buf.size() - len);
        std::cout << "passed" << std::endl;
    }
}

void testRawPtrInterface(const BinaryBuf & buf)
{
    uint8_t tmp[100] = {};
    RawPtr ptr(tmp);

    size_t len = std::min(buf.size(), ptr.size());
    std::copy_n(buf.data(), len, ptr.data());

    std::cout << "== test RawPtr interface" << std::endl;

    std::cout << "test ::data/size: ";
    assert(Tools::crc32b(ptr.data(), len) == Tools::crc32b(buf.data(), len));
    std::cout << "passed" << std::endl;

    if(true)
    {
        StreamBuf sb;
        sb << ptr;

        std::cout << "test ::stream <<: ";
        assert(sb.last() == ptr.size());
        assert(Tools::crc32b(ptr.data(), len) == sb.rawbuf().crc32b());
        std::cout << "passed" << std::endl;
    }

    if(true)
    {
        StreamBufRef sb(buf.data(), buf.size());
        sb >> ptr;

        std::cout << "test ::stream >>: ";
        assert(Tools::crc32b(ptr.data(), len) == Tools::crc32b(buf.data(), len));
        std::cout << "passed" << std::endl;
    }
}

void testByteOrderInterface(const BinaryBuf & buf)
{
    StreamBuf sb;

    // LE/LE
    std::cout << "== test writeLE/readLE interface" << std::endl;
    sb.writeIntLE16(0x1122);
    std::cout << "test ::writeIntLE16/readIntLE16: ";
    assert(sb.readIntLE16() == 0x1122);
    std::cout << "passed" << std::endl;

    sb.writeIntLE32(0x11223344);
    std::cout << "test ::writeIntLE32/readIntLE32: ";
    assert(sb.readIntLE32() == 0x11223344);
    std::cout << "passed" << std::endl;

    sb.writeIntLE64(0x1122334455667788);
    std::cout << "test ::writeIntLE64/readIntLE64: ";
    assert(sb.readIntLE64() == 0x1122334455667788);
    std::cout << "passed" << std::endl;

    // BE/BE
    std::cout << "== test writeBE/readBE interface" << std::endl;
    sb.writeIntBE16(0x1122);
    std::cout << "test ::writeIntBE16/readIntBE16: ";
    assert(sb.readIntBE16() == 0x1122);
    std::cout << "passed" << std::endl;

    sb.writeIntBE32(0x11223344);
    std::cout << "test ::writeIntBE32/readIntBE32: ";
    assert(sb.readIntBE32() == 0x11223344);
    std::cout << "passed" << std::endl;

    sb.writeIntBE64(0x1122334455667788);
    std::cout << "test ::writeIntBE64/readIntBE64: ";
    assert(sb.readIntBE64() == 0x1122334455667788);
    std::cout << "passed" << std::endl;

    // LE/BE
    std::cout << "== test writeLE/readBE interface" << std::endl;
    sb.writeIntLE16(0x1122);
    std::cout << "test ::writeIntLE16/readIntBE16: ";
    assert(sb.readIntBE16() == ByteOrderInterface::swap16(0x1122));
    std::cout << "passed" << std::endl;

    sb.writeIntLE32(0x11223344);
    std::cout << "test ::writeIntLE32/readIntBE32: ";
    assert(sb.readIntBE32() == ByteOrderInterface::swap32(0x11223344));
    std::cout << "passed" << std::endl;

    sb.writeIntLE64(0x1122334455667788);
    std::cout << "test ::writeIntLE64/readIntBE64: ";
    assert(sb.readIntBE64() == ByteOrderInterface::swap64(0x1122334455667788));
    std::cout << "passed" << std::endl;

    // BE/LE
    std::cout << "== test writeBE/readLE interface" << std::endl;
    sb.writeIntBE16(0x1122);
    std::cout << "test ::writeIntBE16/readIntLE16: ";
    assert(sb.readIntLE16() == ByteOrderInterface::swap16(0x1122));
    std::cout << "passed" << std::endl;

    sb.writeIntBE32(0x11223344);
    std::cout << "test ::writeIntBE32/readIntLE32: ";
    assert(sb.readIntLE32() == ByteOrderInterface::swap32(0x11223344));
    std::cout << "passed" << std::endl;

    sb.writeIntBE64(0x1122334455667788);
    std::cout << "test ::writeIntBE64/readIntLE64: ";
    assert(sb.readIntLE64() == ByteOrderInterface::swap64(0x1122334455667788));
    std::cout << "passed" << std::endl;
}

int main()
{
    RandomBuf buf(335);

    std::cout << "fill random, buf size: " << buf.size() << ", crc32b: " << Tools::hex(buf.crc32b()) << std::endl;

    testStreamBufInterface(buf);
    testRawPtrInterface(buf);
    testByteOrderInterface(buf);

    return 0;
}
