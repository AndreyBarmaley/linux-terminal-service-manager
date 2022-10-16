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

#include <unistd.h>

#include <sys/un.h>
#include <sys/socket.h>

#include <ctime>
#include <cstdio>
#include <memory>
#include <random>
#include <cstring>
#include <clocale>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <numeric>
#include <iterator>
#include <algorithm>
#include <stdexcept>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_application.h"

namespace LTSM
{
    std::vector<uint8_t> Tools::randomBytes(size_t bytesCount)
    {
        std::vector<uint8_t> rand(256);
        std::iota(rand.begin(), rand.end(), 0);

        std::vector<uint8_t> res;
        res.reserve(bytesCount);

        while(bytesCount)
        {
            std::shuffle(rand.begin(), rand.end(), std::mt19937{std::random_device{}()});
            auto size = std::min(bytesCount, rand.size());
            res.insert(res.end(), rand.begin(), std::next(rand.begin(), size));
            bytesCount -= size;
        }

        return res;
    }

    std::string Tools::randomHexString(size_t len)
    {
        auto buf = randomBytes( len );
        return buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2, "", false);
    }

    std::string Tools::prettyFuncName(std::string_view name)
    {
        size_t end = name.find('(');
        if(end == std::string::npos) end = name.size();

        size_t begin = 0;
        for(size_t it = end; it; --it)
        {
            if(name[it] == 0x20)
            {
                begin = it + 1;
                break;
            }
        }
        
        auto sub = name.substr(begin, end - begin);
        return std::string(sub.begin(), sub.end());
    }

    std::string Tools::getTimeZone(void)
    {
        const std::filesystem::path localtime{"/etc/localtime"};
        std::string str;

        if(auto env = std::getenv("TZ"))
        {
            str.assign(env);
        }
        else
        if(std::filesystem::is_symlink(localtime))
        {
            auto path = std::filesystem::read_symlink(localtime);
            auto tz = path.parent_path().filename() / path.filename();
            str.append(tz.native());
        }
        else
        {
            time_t ts;
            struct tm tt;
            char buf[16]{0};

            ::localtime_r(&ts, &tt);
            ::strftime(buf, sizeof(buf)-1, "%Z", &tt);
            str.assign(buf);
        }
            
        return str;
    }

    std::string Tools::getUsername(void)
    {
        auto ptr = std::getenv("USER");
        return std::string(ptr ? ptr : "");
    }

    std::string Tools::lower(std::string str)
    {
        if(! str.empty())
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);

        return str;
    }

    std::string Tools::join(const std::list<std::string> & cont, std::string_view sep)
    {
        return join(cont.begin(), cont.end(), sep);
    }

    std::string Tools::join(const std::vector<std::string> & cont, std::string_view sep)
    {
        return join(cont.begin(), cont.end(), sep);
    }

    std::string Tools::replace(const std::string & src, std::string_view pred, std::string_view val)
    {
        std::string res = src;
        size_t pos = std::string::npos;

        while(std::string::npos != (pos = res.find(pred))) res.replace(pos, pred.size(), val);

        return res;
    }

    std::string Tools::replace(const std::string & src, std::string_view pred, int val)
    {
        return replace(src, pred, std::to_string(val));
    }

    std::list<std::string> Tools::split(std::string_view str, std::string_view sep)
    {
        std::list<std::string> list;
        auto itbeg = str.begin();

        for(;;)
        {
            auto itend = std::search(itbeg, str.end(), sep.begin(), sep.end());
            list.emplace_back(itbeg, itend);

            if(itend >= str.end()) break;

            itbeg = itend;
            std::advance(itbeg, sep.size());
        }

        return list;
    }

    std::list<std::string> Tools::split(std::string_view str, int sep)
    {
        return split(str, std::string(1, static_cast<char>(sep)));
    }

    std::string Tools::runcmd(std::string_view cmd)
    {
        std::array<char, 128> buffer = {0};
        std::unique_ptr<FILE, decltype(pclose)*> pipe{popen(cmd.data(), "r"), pclose};
        std::string result;

        if(!pipe)
        {
            Application::error("popen failed: %s", cmd.data());
            return result;
        }

        while(!std::feof(pipe.get()))
        {
            if(std::fgets(buffer.data(), buffer.size(), pipe.get()))
                result.append(buffer.data());
        }

        if(result.size() && result.back() == '\n')
            result.erase(std::prev(result.end()));

        return result;
    }

    Tools::StringFormat::StringFormat(std::string_view str)
    {
        append(str);
    }

    Tools::StringFormat & Tools::StringFormat::arg(std::string_view val)
    {
        auto it1 = begin();
        auto it2 = end();

        while(true)
        {
            it1 = std::find(it1, end(), '%');

            if(it1 == end() || it1 + 1 == end())
            {
                cur++;
                return *this;
            }

            if(std::isdigit(*(it1 + 1)))
            {
                it2 = std::find_if(it1 + 1, end(), [](int ch)
                {
                    return ! std::isdigit(ch);
                });
                int argc = 0;

                try
                {
                    argc = std::stoi(substr(std::distance(begin(), it1 + 1), it2 - it1 - 1));
                }
                catch(const std::invalid_argument &)
                {
                    Application::error("format failed: `%s', arg: `%s'", this->c_str(), val.data());
                    return *this;
                }

                if(cur == argc) break;
            }

            it1++;
        }

        std::string res;
        res.reserve((size() + val.size()) * 2);
        res.append(substr(0, std::distance(begin(), it1))).append(val).append(substr(std::distance(begin(), it2)));
        std::swap(*this, res);
        return arg(val);
    }

    Tools::StringFormat & Tools::StringFormat::arg(int val)
    {
        return arg(std::to_string(val));
    }

    Tools::StringFormat & Tools::StringFormat::arg(double val, int prec)
    {
        if(prec)
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(prec) << val;
            return arg(os.str());
        }

        return arg(std::to_string(val));
    }

    Tools::StringFormat & Tools::StringFormat::replace(std::string_view id, std::string_view val)
    {
        std::string res(begin(), end());
        size_t pos = std::string::npos;

        while(std::string::npos != (pos = res.find(id))) res.replace(pos, id.size(), val);

        std::swap(*this, res);
        return *this;
    }

    Tools::StringFormat & Tools::StringFormat::replace(std::string_view id, int val)
    {
        return replace(id, std::to_string(val));
    }

    Tools::StringFormat & Tools::StringFormat::replace(std::string_view id, double val, int prec)
    {
        if(prec)
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(prec) << val;
            return replace(id, os.str());
        }

        return replace(id, std::to_string(val));
    }

    std::string Tools::hex(int value, int width)
    {
        std::ostringstream stream;
        stream << "0x" << std::setw(width) << std::setfill('0') << std::nouppercase << std::hex << value;
        return stream.str();
    }

    std::string Tools::escaped(std::string_view str, bool quote)
    {
        std::ostringstream os;

        // start quote
        if(quote)
            os << "\"";

        // variants: \\, \", \/, \t, \n, \r, \f, \b
        for(auto & ch : str)
        {
            switch(ch)
            {
                case '\\':
                    os << "\\\\";
                    break;

                case '"':
                    os << "\\\"";
                    break;

                case '/':
                    os << "\\/";
                    break;

                case '\t':
                    os << "\\t";
                    break;

                case '\n':
                    os << "\\n";
                    break;

                case '\r':
                    os << "\\r";
                    break;

                case '\f':
                    os << "\\f";
                    break;

                case '\b':
                    os << "\\b";
                    break;

                default:
                    os << ch;
                    break;
            }
        }

        // end quote
        if(quote)
            os << "\"";

        return os.str();
    }

    std::string Tools::unescaped(std::string str)
    {
        if(str.size() < 2)
            return str;

        // variants: \\, \", \/, \t, \n, \r, \f, \b
        for(auto it = str.begin(); it != str.end(); ++it)
        {
            auto itn = std::next(it);

            if(itn == str.end()) break;

            if(*it == '\\')
            {
                switch(*itn)
                {
                    case '\\':
                        str.erase(itn);
                        break;

                    case '"':
                        str.erase(itn);
                        *it = '"';
                        break;

                    case '/':
                        str.erase(itn);
                        *it = '/';
                        break;

                    case 't':
                        str.erase(itn);
                        *it = '\t';
                        break;

                    case 'n':
                        str.erase(itn);
                        *it = '\n';
                        break;

                    case 'r':
                        str.erase(itn);
                        *it = '\r';
                        break;

                    case 'f':
                        str.erase(itn);
                        *it = '\f';
                        break;

                    case 'b':
                        str.erase(itn);
                        *it = '\b';
                        break;

                    default:
                        break;
                }
            }
        }

        return str;
    }

    uint32_t Tools::crc32b(std::string_view str)
    {
        return crc32b((const uint8_t*) str.data(), str.size());
    }
    uint32_t Tools::crc32b(const uint8_t* ptr, size_t size)
    {
        return crc32b(ptr, size, 0xEDB88320);
    }

    uint32_t Tools::crc32b(const uint8_t* ptr, size_t size, uint32_t magic)
    {
        uint32_t res = std::accumulate(ptr, ptr + size, 0xFFFFFFFF, [=](uint32_t crc, int val)
        {
            crc ^= val;

            for(int bit = 0; bit < 8; ++bit)
            {
                uint32_t mask = crc & 1 ? 0xFFFFFFFF : 0;
                crc = (crc >> 1) ^ (magic & mask);
            }

            return crc;
        });
        return ~res;
    }

    bool Tools::checkUnixSocket(const std::filesystem::path & path)
    {
        // check present
        if(std::filesystem::is_socket(path))
        {
            int socket_fd = socket(PF_UNIX, SOCK_STREAM, 0);

            if(0 < socket_fd)
            {
                // check open
                struct sockaddr_un sockaddr;
                std::memset(&sockaddr, 0, sizeof(struct sockaddr_un));
                sockaddr.sun_family = AF_UNIX;
                const std::string & native = path.native();

                if(native.size() > sizeof(sockaddr.sun_path) - 1)
                    Application::warning("%s: unix path is long, truncated to size: %d", __FUNCTION__, sizeof(sockaddr.sun_path) - 1);

                std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);
                int res = connect(socket_fd, (struct sockaddr*) &sockaddr,  sizeof(struct sockaddr_un));
                close(socket_fd);
                return res == 0;
            }
        }

        return false;
    }

    // StreamBits
    bool Tools::StreamBits::empty(void) const
    {
        return vecbuf.empty() ||
                (vecbuf.size() == 1 && bitpos == 7);
    }
        
    const std::vector<uint8_t> & Tools::StreamBits::toVector(void) const
    {
        return vecbuf;
    }

    // StreamBitsPack
    Tools::StreamBitsPack::StreamBitsPack()
    {
        bitpos = 7;
        vecbuf.reserve(32);
    }

    void Tools::StreamBitsPack::pushBit(bool v)
    {
        if(bitpos == 7)
            vecbuf.push_back(0);

        uint8_t mask = 1 << bitpos;
        if(v) vecbuf.back() |= mask;

        if(bitpos == 0)
            bitpos = 7;
        else
            bitpos--;
    }

    void Tools::StreamBitsPack::pushAlign(void)
    {
        bitpos = 7;
    }

    void Tools::StreamBitsPack::pushValue(int val, size_t field)
    {
        // field 1: mask 0x0001, field 2: mask 0x0010, field 4: mask 0x1000
        size_t mask = 1 << (field - 1);

        while(mask)
        {
            pushBit(val & mask);
            mask >>= 1;
        }
    }

    // StreamBitsUnpack
    Tools::StreamBitsUnpack::StreamBitsUnpack(const std::vector<uint8_t> & v, size_t counts, size_t field)
    {
        // check size
        size_t bits = field * counts;
        size_t len = bits >> 3;
        if((len << 3) < bits) len++;

        if(len < v.size())
        {
            Application::error("%s: %s", __FUNCTION__, "incorrect data size");
            throw std::out_of_range(NS_FuncName);
        }

        vecbuf.assign(v.begin(), v.end());
        bitpos = (len << 3) - bits;
    }

    bool Tools::StreamBitsUnpack::popBit(void)
    {
        if(vecbuf.empty())
        {
            Application::error("%s: %s", __FUNCTION__, "empty data");
            throw std::invalid_argument(NS_FuncName);
        }

        uint8_t mask = 1 << bitpos;
        bool res = vecbuf.back() & mask;

        if(bitpos == 7)
        {
            vecbuf.pop_back();
            bitpos = 0;
        }
        else
        {
            bitpos++;
        }

        return res;
    }

    int Tools::StreamBitsUnpack::popValue(size_t field)
    {
        // field 1: mask 0x0001, field 2: mask 0x0010, field 4: mask 0x1000
        size_t mask1 = 1 << (field - 1);
        size_t mask2 = 1;
        int val = 0;

        while(mask1)
        {
            if(popBit())
                val |= mask2;

            mask1 >>= 1;
            mask2 <<= 1;
        }

        return val;
    }

    size_t Tools::maskShifted(size_t mask)
    {
        size_t res = 0;

        if(mask)
        {
            while((mask & 1) == 0)
            {
                mask = mask >> 1;
                res = res + 1;
            }
        }

        return res;
    }

    size_t Tools::maskMaxValue(uint32_t mask)
    {
        size_t res = 0;

        if(mask)
        {
            while((mask & 1) == 0)
                mask = mask >> 1;

            res = ~static_cast<size_t>(0) & mask;
        }

        return res;
    }
}
