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

#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

#include <sys/un.h>
#include <sys/socket.h>

#include <zlib.h>

#ifdef LTSM_WITH_GNUTLS
#include "gnutls/x509.h"
#include <gnutls/gnutls.h>
#endif

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
#include "ltsm_sockets.h"
#include "ltsm_streambuf.h"
#include "ltsm_application.h"

namespace LTSM
{
    //// UserInfo
    UserInfo::UserInfo(std::string_view name)
    {
        auto buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
        buf = std::make_unique<char[]>(buflen);
        struct passwd* res = nullptr;

        if(int ret = getpwnam_r(name.data(), & st, buf.get(), buflen, & res); ret != 0)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getpwnam_r", strerror(errno), errno);
            throw std::runtime_error(__FUNCTION__);
        }
    }

    UserInfo::UserInfo(uid_t uid)
    {
        auto buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
        buf = std::make_unique<char[]>(buflen);
        struct passwd* res = nullptr;

        if(int ret = getpwuid_r(uid, & st, buf.get(), buflen, & res); ret != 0)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getpwuid_r", strerror(errno), errno);
            throw std::runtime_error(__FUNCTION__);
        }
    }

    std::vector<gid_t> UserInfo::groups(void) const
    {
        int ngroups = 0;
        getgrouplist(st.pw_name, st.pw_gid, nullptr, & ngroups);

        if(0 < ngroups)
        {
            std::vector<gid_t> res(ngroups, st.pw_gid);
            getgrouplist(st.pw_name, st.pw_gid, res.data(), & ngroups);
            res.resize(ngroups);
            return res;
        }

        return {};
    }

    /// GroupInfo
    GroupInfo::GroupInfo(gid_t gid)
    {
        auto buflen = sysconf(_SC_GETGR_R_SIZE_MAX);
        buf = std::make_unique<char[]>(buflen);
        struct group* res = nullptr;

        if(int ret = getgrgid_r(gid, & st, buf.get(), buflen, & res); ret != 0)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getgrgid_r", strerror(errno), errno);
            throw std::runtime_error(__FUNCTION__);
        }
    }

    GroupInfo::GroupInfo(std::string_view name)
    {
        auto buflen = sysconf(_SC_GETGR_R_SIZE_MAX);
        buf = std::make_unique<char[]>(buflen);
        struct group* res = nullptr;

        if(int ret = getgrnam_r(name.data(), & st, buf.get(), buflen, & res); ret != 0)
        {
            Application::warning("%s: %s failed, error: %s, code: %d", __FUNCTION__, "getgrnam_r", strerror(errno), errno);
            throw std::runtime_error(__FUNCTION__);
        }
    }

    std::forward_list<std::string> GroupInfo::members(void) const
    {
        if(auto ptr = st.gr_mem)
        {
            std::forward_list<std::string> res;

            while(const char* memb = *ptr)
            {
                res.emplace_front(memb);
                ptr++;
            }
        }

        return {};
    }

    UserInfoPtr Tools::getUidInfo(uid_t uid)
    {
        try
        {
            return std::make_unique<UserInfo>(uid);
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: uid not found: %d", __FUNCTION__, (int) uid);
        return nullptr;
    }

    UserInfoPtr Tools::getUserInfo(std::string_view user)
    {
        try
        {
            return std::make_unique<UserInfo>(user);
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: user not found: `%s'", __FUNCTION__, user.data());
        return nullptr;
    }

    uid_t Tools::getUserUid(std::string_view user)
    {
        try
        {
            return UserInfo(user).uid();
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: user not found: `%s'", __FUNCTION__, user.data());
        return 0;
    }

    std::string Tools::getUserLogin(uid_t uid)
    {
        try
        {
            return UserInfo(uid).user();
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: uid not found: %d", __FUNCTION__, (int) uid);
        return "";
    }

    std::string Tools::getUserHome(std::string_view user)
    {
        try
        {
            return UserInfo(user).home();
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: user not found: `%s'", __FUNCTION__, user.data());
        return "";
    }

    GroupInfoPtr Tools::getGidInfo(gid_t gid)
    {
        try
        {
            return std::make_unique<GroupInfo>(gid);
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: gid not found: %d", __FUNCTION__, (int) gid);
        return nullptr;
    }

    GroupInfoPtr Tools::getGroupInfo(std::string_view group)
    {
        try
        {
            return std::make_unique<GroupInfo>(group);
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: group not found: `%s'", __FUNCTION__, group.data());
        return nullptr;
    }

    gid_t Tools::getGroupGid(std::string_view group)
    {
        try
        {
            return GroupInfo(group).gid();
        }
        catch(const std::exception &)
        {
        }

        Application::warning("%s: group not found: `%s'", __FUNCTION__, group.data());
        return 0;
    }

    std::forward_list<std::string> Tools::getSystemUsers(uid_t uidMin, uid_t uidMax)
    {
        if(uidMin > uidMax)
        {
            std::swap(uidMin, uidMax);
        }

        struct passwd st = {};

        auto buflen = sysconf(_SC_GETPW_R_SIZE_MAX);

        auto buf = std::make_unique<char[]>(buflen);

        struct passwd* res = nullptr;

        std::forward_list<std::string> logins;

        setpwent();

        while(0 == getpwent_r(& st, buf.get(), buflen, & res))
        {
            if(! res)
            {
                break;
            }

            if(uidMin <= res->pw_uid && res->pw_uid <= uidMax)
            {
                logins.emplace_front(res->pw_name);
            }
        }

        endpwent();
        return logins;
    }

    std::list<std::string> Tools::readDir(const std::string & path, bool recurse)
    {
        std::list<std::string> res;
        std::error_code err;

        for(auto const & entry : std::filesystem::directory_iterator{path, err})
        {
            if(recurse && entry.is_directory())
            {
                res.splice(res.end(), readDir(entry.path(), true));
            }

            res.emplace_back(entry.path().native());
        }

        return res;
    }

    std::filesystem::path Tools::resolveSymLink(const std::filesystem::path & path)
    {
        std::error_code err;
        return std::filesystem::exists(path, err) && std::filesystem::is_symlink(path, err) ?
               resolveSymLink(std::filesystem::read_symlink(path, err)) : path;
    }

    std::vector<uint8_t> Tools::zlibCompress(const ByteArray & arr)
    {
        std::vector<uint8_t> res;

        if(arr.data() && arr.size())
        {
            res.resize(::compressBound(arr.size()));
            uLong dstsz = res.size();
            int ret = ::compress(reinterpret_cast<Bytef*>(res.data()), & dstsz,
                                 reinterpret_cast<const Bytef*>(arr.data()), arr.size());

            if(ret == Z_OK)
            {
                res.resize(dstsz);
            }
            else
            {
                res.clear();
                Application::error("%s: %s failed, error: %d", __FUNCTION__, "compress", ret);
            }
        }

        return res;
    }

    std::vector<uint8_t> Tools::zlibUncompress(const ByteArray & arr, size_t real)
    {
        std::vector<uint8_t> res;

        if(arr.data() && arr.size())
        {
            res.resize(real ? real : arr.size() * 7);
            uLong dstsz = res.size();
            int ret = Z_BUF_ERROR;

            while(Z_BUF_ERROR ==
                    (ret = ::uncompress(reinterpret_cast<Bytef*>(res.data()), &dstsz,
                                        reinterpret_cast<const Bytef*>(arr.data()), arr.size())))
            {
                dstsz = res.size() * 2;
                res.resize(dstsz);
            }

            if(ret == Z_OK)
            {
                res.resize(dstsz);
            }
            else
            {
                res.clear();
                Application::error("%s: %s failed, error: %d", __FUNCTION__, "uncompress", ret);
            }
        }

        return res;
    }

    char base64EncodeChar(char v)
    {
        // 0 <=> 25
        if(v <= ('Z' - 'A'))
        {
            return v + 'A';
        }

        // 26 <=> 51
        if(v <= (26 + ('z' - 'a')))
        {
            return v + 'a' - 26;
        }

        // 52 <=> 61
        if(v <= (52 + ('9' - '0')))
        {
            return v + '0' - 52;
        }

        if(v == 62)
        {
            return '+';
        }

        if(v == 63)
        {
            return '/';
        }

        return 0;
    }

    uint8_t base64DecodeChar(char v)
    {
        if(v == '+')
        {
            return 62;
        }

        if(v == '/')
        {
            return 63;
        }

        if('0' <= v && v <= '9')
        {
            return v - '0' + 52;
        }

        if('A' <= v && v <= 'Z')
        {
            return v - 'A';
        }

        if('a' <= v && v <= 'z')
        {
            return v - 'a' + 26;
        }

        return 0;
    }

    std::string Tools::base64Encode(const ByteArray & arr)
    {
        size_t len = 4 * arr.size() / 3 + 1;

        std::string res;
        res.reserve(len);

        auto beg = arr.data();
        auto end = beg + arr.size();

        while(beg < end)
        {
            auto next1 = beg + 1;
            auto next2 = beg + 2;

            uint32_t b1 = *beg;
            uint32_t b2 = next1 < end ? *next1 : 0;
            uint32_t b3 = next2 < end ? *next2 : 0;

            uint32_t triple = (b1 << 16) | (b2 << 8) | b3;

            res.push_back(base64EncodeChar(0x3F & (triple >> 18)));
            res.push_back(base64EncodeChar(0x3F & (triple >> 12)));
            res.push_back(next1 < end ? base64EncodeChar(0x3F & (triple >> 6)) : '=');
            res.push_back(next2 < end ? base64EncodeChar(0x3F & triple) : '=');

            beg = next2 + 1;
        }

        return res;
    }

    std::vector<uint8_t> Tools::base64Decode(const std::string & str)
    {
        std::vector<uint8_t> res;

        if(0 < str.length() && 0 == (str.length() % 4))
        {
            size_t len = 3 * str.length() / 4;

            if(str[str.length() - 1] == '=') { len--; }

            if(str[str.length() - 2] == '=') { len--; }

            res.reserve(len);

            for(size_t ii = 0; ii < str.length(); ii += 4)
            {
                uint32_t sxtet_a = base64DecodeChar(str[ii]);
                uint32_t sxtet_b = base64DecodeChar(str[ii + 1]);
                uint32_t sxtet_c = base64DecodeChar(str[ii + 2]);
                uint32_t sxtet_d = base64DecodeChar(str[ii + 3]);

                uint32_t triple = (sxtet_a << 18) + (sxtet_b << 12) + (sxtet_c << 6) + sxtet_d;

                if(res.size() < len) { res.push_back((triple >> 16) & 0xFF); }

                if(res.size() < len) { res.push_back((triple >> 8) & 0xFF); }

                if(res.size() < len) { res.push_back(triple & 0xFF); }
            }
        }
        else
        {
            Application::error("%s: %s failed", __FUNCTION__, "base64");
        }

        return res;
    }

    std::string Tools::convertBinary2JsonString(const ByteArray & buf)
    {
        auto zip = zlibCompress(buf);

        StreamBuf sb;
        sb.writeIntBE32(buf.size());
        sb.write(zip);

        return base64Encode(sb.rawbuf());
    }

    std::vector<uint8_t> Tools::convertJsonString2Binary(const std::string & content)
    {
        auto buf = Tools::base64Decode(content);
        StreamBufRef sb(buf.data(), buf.size());

        if(4 < sb.last())
        {
            uint32_t real = sb.readIntBE32();
            return Tools::zlibUncompress(RawPtr(sb.data(), sb.last()), real);
        }
        else
        {
            Application::error("%s: decode failed, streambuf size: %u, base64 size: %u", __FUNCTION__, sb.last(), content.size());
        }

        return std::vector<uint8_t>();
    }

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
        return buffer2hexstring(buf.begin(), buf.end(), 2, "", false);
    }

    std::string Tools::prettyFuncName(std::string_view name)
    {
        size_t end = name.find('(');

        if(end == std::string::npos) { end = name.size(); }

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

    std::string Tools::fileToString(const std::filesystem::path & file)
    {
        std::string str;
        std::error_code err;

        if(std::filesystem::exists(file, err))
        {
            std::ifstream ifs(file, std::ios::binary);

            if(ifs.is_open())
            {
                auto fsz = std::filesystem::file_size(file);
                str.resize(fsz);
                ifs.read(str.data(), str.size());
                ifs.close();
            }
            else
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "read", file.c_str());
            }
        }
        else
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), file.c_str(), getuid());
        }

        return str;
    }

    std::string Tools::getHostname(void)
    {
        std::array<char, 256> buf = {};

        if(0 != gethostname(buf.data(), buf.size() - 1))
        {
            Application::warning( "%s: %s failed, error: %s, code: %d", __FUNCTION__, "gethostname", strerror(errno), errno);
            return "localhost";
        }

        return std::string(buf.data());
    }

    std::string Tools::getTimeZone(void)
    {
        const std::filesystem::path localtime{"/etc/localtime"};
        std::string str;
        std::error_code err;

        if(auto env = std::getenv("TZ"))
        {
            str.assign(env);
        }
        else if(std::filesystem::is_symlink(localtime, err))
        {
            auto path = std::filesystem::read_symlink(localtime, err);

            if(! err)
            {
                auto tz = path.parent_path().filename() / path.filename();
                str.append(tz.native());
            }
        }
        else
        {
            time_t ts;
            struct tm tt;

            char buf[16];
            std::fill_n(buf, sizeof(buf), 0);

            ::localtime_r(&ts, &tt);
            ::strftime(buf, sizeof(buf)-1, "%Z", &tt);
            str.assign(buf);
        }

        return str;
    }

    std::string Tools::lower(std::string str)
    {
        if(! str.empty())
        {
            std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        }

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

        while(std::string::npos != (pos = res.find(pred))) { res.replace(pos, pred.size(), val); }

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

            if(itend >= str.end()) { break; }

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
        std::array<char, 128> buffer;
        std::fill(buffer.begin(), buffer.end(), 0);
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
            {
                result.append(buffer.data());
            }
        }

        if(result.size() && result.back() == '\n')
        {
            result.pop_back();
        }

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

                if(cur == argc) { break; }
            }

            it1++;
        }

        assign(joinToString(substr(0, std::distance(begin(), it1)), val, substr(std::distance(begin(), it2))));
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

        while(std::string::npos != (pos = res.find(id))) { res.replace(pos, id.size(), val); }

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
        {
            os << "\"";
        }

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
        {
            os << "\"";
        }

        return os.str();
    }

    std::string Tools::unescaped(std::string str)
    {
        if(str.size() < 2)
        {
            return str;
        }

        // variants: \\, \", \/, \t, \n, \r, \f, \b
        for(auto it = str.begin(); it != str.end(); ++it)
        {
            auto itn = std::next(it);

            if(itn == str.end()) { break; }

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
        std::error_code err;

        // check present
        if(std::filesystem::is_socket(path, err))
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
                {
                    Application::warning("%s: unix path is long, truncated to size: %d", __FUNCTION__, sizeof(sockaddr.sun_path) - 1);
                }

                std::copy_n(native.begin(), std::min(native.size(), sizeof(sockaddr.sun_path) - 1), sockaddr.sun_path);
                int res = connect(socket_fd, (struct sockaddr*) &sockaddr, sizeof(struct sockaddr_un));
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
    Tools::StreamBitsPack::StreamBitsPack(size_t rez)
    {
        bitpos = 7;
        vecbuf.reserve(rez);
    }

    void Tools::StreamBitsPack::pushBit(bool v)
    {
        if(bitpos == 7)
        {
            vecbuf.push_back(0);
        }

        if(v)
        {
            const uint8_t mask = 1 << bitpos;
            vecbuf.back() |= mask;
        }

        if(bitpos == 0)
        {
            bitpos = 7;
        }
        else
        {
            bitpos--;
        }
    }

    void Tools::StreamBitsPack::pushAlign(void)
    {
        bitpos = 7;
    }

    void Tools::StreamBitsPack::pushValue(int val, size_t field)
    {
        // field 1: mask 0x0001, field 2: mask 0x0010, field 4: mask 0x1000
        size_t mask = 1ul << (field - 1);

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

        if((len << 3) < bits) { len++; }

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
            {
                val |= mask2;
            }

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
            {
                mask = mask >> 1;
            }

            res = ~static_cast<size_t>(0) & mask;
        }

        return res;
    }

    bool Tools::binaryToFile(const void* buf, size_t len, std::string_view file)
    {
        std::ofstream ofs(file.data(), std::ofstream::out | std::ios::binary | std::ofstream::trunc);

        if(ofs.is_open())
        {
            ofs.write((const char*) buf, len);
            ofs.close();
            return true;
        }
        else
        {
            Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "write", file.data());
        }

        return false;
    }

    std::vector<uint8_t> Tools::fileToBinaryBuf(const std::filesystem::path & file)
    {
        std::vector<uint8_t> buf;
        std::error_code err;

        if(std::filesystem::exists(file, err))
        {
            std::ifstream ifs(file, std::ios::binary);

            if(ifs.is_open())
            {
                auto fsz = std::filesystem::file_size(file);
                buf.resize(fsz);
                ifs.read((char*) buf.data(), buf.size());
                ifs.close();
            }
            else
            {
                Application::error("%s: %s failed, path: `%s'", __FUNCTION__, "read", file.c_str());
            }
        }
        else
        {
            Application::error("%s: %s, path: `%s', uid: %d", __FUNCTION__, (err ? err.message().c_str() : "not found"), file.c_str(), getuid());
        }

        return buf;
    }

#if defined(LTSM_ENCODING_FFMPEG) || defined(LTSM_DECODING_FFMPEG)
    bool Tools::AV_PixelFormatEnumToMasks(AVPixelFormat format, int* bpp, uint32_t* rmask, uint32_t* gmask, uint32_t* bmask, uint32_t* amask, bool debug)
    {
        switch(format)
        {
            case AV_PIX_FMT_RGB24:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGB24"); }

                *bpp = 24;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                std::swap(*rmask, *bmask);
#endif
                return true;

            case AV_PIX_FMT_BGR24:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGR24"); }

                *bpp = 24;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                std::swap(*rmask, *bmask);
#endif
                return true;

            case AV_PIX_FMT_RGB0:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGB0"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#else
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0;
#endif
                return true;

            case AV_PIX_FMT_0BGR:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_0BGR"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0;
#else
                *bpp = 32;
                *amask = 0;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_BGR0:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGR0"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#else
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0;
#endif
                return true;

            case AV_PIX_FMT_0RGB:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_0RGB"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0;
#else
                *bpp = 32;
                *amask = 0;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_RGBA:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_RGBA"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0xFF000000;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#else
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_ABGR:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_ABGR"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *rmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *bmask = 0x0000FF00;
                *amask = 0x000000FF;
#else
                *bpp = 32;
                *amask = 0xFF000000;
                *bmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *rmask = 0x000000FF;
#endif
                return true;

            case AV_PIX_FMT_BGRA:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_BGRA"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *amask = 0xFF000000;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#else
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0x000000FF;
#endif
                return true;


            case AV_PIX_FMT_ARGB:
                if(debug) { Application::info("%s: %s", __FUNCTION__, "AV_PIX_FMT_ARGB"); }

#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                *bpp = 32;
                *bmask = 0xFF000000;
                *gmask = 0x00FF0000;
                *rmask = 0x0000FF00;
                *amask = 0x000000FF;
#else
                *bpp = 32;
                *amask = 0xFF000000;
                *rmask = 0x00FF0000;
                *gmask = 0x0000FF00;
                *bmask = 0x000000FF;
#endif
                return true;

            default:
                break;
        }

        return false;
    }

    AVPixelFormat Tools::AV_PixelFormatEnumFromMasks(int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask, bool debug)
    {
        if(debug)
        {
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
            bool bigEndian = false;
#else
            bool bigEndian = true;
#endif
            Application::info("%s: pixel format, bpp: %d, rmask: 0x%08" PRIx32 ", gmask: 0x%08" PRIx32 ", bmask: 0x%08" PRIx32 ", amask: 0x%08" PRIx32 ", be: %d",
                              __FUNCTION__, bpp, rmask, gmask, bmask, amask, (int) bigEndian);
        }

        if(24 == bpp)
        {
            if(amask == 0 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGR24;

#else
                return AV_PIX_FMT_RGB24;
#endif

            if(amask == 0 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGB24;

#else
                return AV_PIX_FMT_BGR24;
#endif
        }
        else if(32 == bpp)
        {
            if(rmask == 0xFF000000 && gmask == 0x00FF0000 && bmask == 0x0000FF00 && amask == 0)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_0BGR;

#else
                return AV_PIX_FMT_RGB0;
#endif

            if(amask == 0 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGB0;

#else
                return AV_PIX_FMT_0BGR;
#endif

            if(bmask == 0xFF000000 && gmask == 0x00FF0000 && rmask == 0x0000FF00 && amask == 0)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_0RGB;

#else
                return AV_PIX_FMT_BGR0;
#endif

            if(amask == 0 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGR0;

#else
                return AV_PIX_FMT_0RGB;
#endif

            if(rmask == 0xFF000000 && gmask == 0x00FF0000 && bmask == 0x0000FF00 && amask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_ABGR;

#else
                return AV_PIX_FMT_RGBA;
#endif

            if(amask == 0xFF000000 && bmask == 0x00FF0000 && gmask == 0x0000FF00 && rmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_RGBA;

#else
                return AV_PIX_FMT_ABGR;
#endif

            if(bmask == 0xFF000000 && gmask == 0x00FF0000 && rmask == 0x0000FF00 && amask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_ARGB;

#else
                return AV_PIX_FMT_BGRA;
#endif

            if(amask == 0xFF000000 && rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF)
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                return AV_PIX_FMT_BGRA;

#else
                return AV_PIX_FMT_ARGB;
#endif
        }

        Application::error("%s: unsupported pixel format, bpp: %d, rmask: 0x%08" PRIx32 ", gmask: 0x%08" PRIx32 ", bmask: 0x%08" PRIx32 ", amask: 0x%08" PRIx32,
                           __FUNCTION__, bpp, rmask, gmask, bmask, amask);

        return AV_PIX_FMT_NONE;
    }

#endif

} // LTSM
