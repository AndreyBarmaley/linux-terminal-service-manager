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

#include <cstdio>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <iterator>
#include <stdexcept>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_application.h"

namespace LTSM
{
    std::string Tools::lower(std::string str)
    {
        if(! str.empty())
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);

        return str;
    }

    std::string Tools::join(const std::list<std::string> & list)
    {
        std::ostringstream os;
        std::copy(list.begin(), list.end(), std::ostream_iterator<std::string>(os));
        return os.str();
    }

    std::string Tools::join(const std::list<std::string> & list, const std::string & sep)
    {
        std::ostringstream os;

        for(auto it = list.begin(); it != list.end(); ++it)
        {
            os << *it;

            if(std::next(it) != list.end())
                os << sep;
        }

        return os.str();
    }

    std::string Tools::replace(const std::string & src, const char* pred, const std::string & val)
    {
        std::string res = src;
        size_t pos = std::string::npos;

        while(std::string::npos != (pos = res.find(pred))) res.replace(pos, std::strlen(pred), val);

        return res;
    }

    std::string Tools::replace(const std::string & src, const char* pred, int val)
    {
        return replace(src, pred, std::to_string(val));
    }

    std::list<std::string> Tools::split(const std::string & str, const std::string & sep)
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

    std::list<std::string> Tools::split(const std::string & str, int sep)
    {
        return split(str, std::string(1, sep));
    }

    std::string Tools::runcmd(const std::string & cmd)
    {
        char buffer[128];
        std::string result;
        std::shared_ptr<FILE> pipe(popen(cmd.c_str(), "r"), pclose);

        if(!pipe)
        {
            Application::error("popen failed: %s", cmd.c_str());
            return result;
        }

        while(!std::feof(pipe.get()))
        {
            if(std::fgets(buffer, sizeof(buffer), pipe.get()))
                result.append(buffer);
        }

	if(result.size() && result.back() == '\n')
	    result.erase(std::prev(result.end()));

        return result;
    }

    Tools::StringFormat::StringFormat(const std::string & str) : cur(1)
    {
        append(str);
    }

    Tools::StringFormat & Tools::StringFormat::arg(const char* val)
    {
        return arg(val ? std::string(val) : std::string("(null)"));
    }

    Tools::StringFormat & Tools::StringFormat::arg(const std::string & val)
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
                    Application::error("format failed: `%s', arg: `%s'", this->c_str(), val.c_str());
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

    Tools::StringFormat & Tools::StringFormat::replace(const char* id, const std::string & val)
    {
        std::string res = *this;
        size_t pos = std::string::npos;

        while(std::string::npos != (pos = res.find(id))) res.replace(pos, std::strlen(id), val);

        std::swap(*this, res);
        return *this;
    }

    Tools::StringFormat & Tools::StringFormat::replace(const char* id, int val)
    {
        return replace(id, std::to_string(val));
    }

    Tools::StringFormat & Tools::StringFormat::replace(const char* id, double val, int prec)
    {
        if(prec)
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(prec) << val;
            return replace(id, os.str());
        }

        return replace(id, std::to_string(val));
    }

    std::string Tools::getenv(const char* name, const char* def)
    {
        std::string res(def ? def : "");
        const char* env = std::getenv(name);

        if(env) res.assign(env);

        return res;
    }

    std::string Tools::hex(int value, int width)
    {
        std::ostringstream stream;
        stream << "0x" << std::setw(width) << std::setfill('0') << std::nouppercase << std::hex << value;
        return stream.str();
    }

    std::string Tools::escaped(const std::string & str, bool quote)
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
                case '\\': os << "\\\\"; break;
                case '"':  os << "\\\""; break;
                case '/':  os << "\\/"; break;
                case '\t': os << "\\t"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\f': os << "\\f"; break;
                case '\b': os << "\\b"; break;
                default: os << ch; break;
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
                    case '\\': str.erase(itn); break;
                    case '"': str.erase(itn); *it = '"'; break;
                    case '/': str.erase(itn); *it = '/'; break;
                    case 't': str.erase(itn); *it = '\t'; break;
                    case 'n': str.erase(itn); *it = '\n'; break;
                    case 'r': str.erase(itn); *it = '\r'; break;
                    case 'f': str.erase(itn); *it = '\f'; break;
                    case 'b': str.erase(itn); *it = '\b'; break;
                    default: break;
                }
            }
        }
        
        return str;
    }
/*
    template<typename InputIterator>
    InputIterator   random_n(InputIterator first, InputIterator last)
    {
        auto dist = std::distance(first, last);
        InputIterator res = first;

        if(1 < dist)
        {
            auto randomIndex = std::rand() / (RAND_MAX + 1.0) * (dist + 1);
            std::advance(res, randomIndex);
        }
        return res;
    }

    std::string Tools::randomHexString(size_t width)
    {
	const char charsets[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabctefghijklmnopqrstuvwxyz";
	
	std::string res;
	res.reserve(width);

	while(width--)
	{
	    auto beg = std::begin(charsets);
	    auto end = std::end(charsets);
	    auto it = random_n(beg, end);
	    res.push_back(it != end ? *it : 'X');
	}

	return res;
    }
*/
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

    bool Tools::checkUnixSocket(const std::string & path)
    {
        // check present
	if(std::filesystem::is_socket(path))
        {
            int socket_fd = socket(PF_UNIX, SOCK_STREAM, 0);

            if(0 < socket_fd)
            {
                // check open
                struct sockaddr_un address;
                std::memset(&address, 0, sizeof(struct sockaddr_un));
                address.sun_family = AF_UNIX;
                std::strcpy(address.sun_path, path.c_str());

                int res = connect(socket_fd, (struct sockaddr*) &address,  sizeof(struct sockaddr_un));
                close(socket_fd);
                return res == 0;
            }
        }

        return false;
    }

    // StreamBitsPack
    Tools::StreamBitsPack::StreamBitsPack() : bitpos(7)
    {
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

    bool Tools::StreamBitsPack::empty(void) const
    {
        return vecbuf.empty() ||
                (vecbuf.size() == 1 && bitpos == 7);
    }

    const std::vector<uint8_t> & Tools::StreamBitsPack::toVector(void) const
    {
        return vecbuf;
    }
/*
    #include <zlib.h>
    std::vector<uint8_t> Tools::zlibCompress(const uint8_t* ptr, size_t size)
    {
        std::vector<uint8_t> res;

        if(ptr && size)
        {
            res.resize(compressBound(size));
            uLong dstsz = res.size();
            int ret = ::compress(reinterpret_cast<Bytef*>(res.data()), &dstsz,
                                 reinterpret_cast<const Bytef*>(ptr), size);

            if(ret == Z_OK)
                res.resize(dstsz);
            else
            {
                res.clear();
                Application::error("zlib error code: %d", ret);
            }
        }

        return res;
    }

    std::vector<uint8_t> Tools::zlibUncompress(const uint8_t* ptr, size_t size, size_t realsz)
    {
        std::vector<uint8_t> res;

        if(ptr && size)
        {
            res.resize(realsz ? realsz : size * 7);
            uLong dstsz = res.size();
            int ret = Z_BUF_ERROR;

            while(Z_BUF_ERROR ==
                  (ret = ::uncompress(reinterpret_cast<Bytef*>(res.data()), &dstsz,
                                      reinterpret_cast<const Bytef*>(ptr), size)))
            {
                dstsz = res.size() * 2;
                res.resize(dstsz);
            }

            if(ret == Z_OK)
                res.resize(dstsz);
            else
            {
                res.clear();
                Application::error("zlib error code: %d", ret);
            }
        }

        return res;
    }
*/

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
