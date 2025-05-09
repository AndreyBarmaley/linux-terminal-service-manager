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

#ifndef _LTSM_TOOLS_
#define _LTSM_TOOLS_


#include <sys/types.h>

#ifdef __LINUX__
#include <grp.h>
#include <pwd.h>
#endif

#include <list>
#include <chrono>
#include <vector>
#include <string>
#include <utility>
#include <iomanip>
#include <iterator>
#include <algorithm>
#include <string_view>
#include <forward_list>

#include <tuple>
#include <memory>
#include <atomic>
#include <thread>
#include <utility>
#include <cinttypes>
#include <filesystem>
#include <functional>

#include <cassert>
// Use (void) to silence unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

#ifdef LTSM_WITH_GNUTLS
#include "gnutls/gnutls.h"
#endif

#ifdef __MINGW64__
    int getuid(void);
    int getgid(void);
    int getpid(void);
#endif

namespace LTSM
{
    class ByteArray;

#ifdef __LINUX__
    class UserInfo
    {
        struct passwd st = {};
        std::unique_ptr<char[]> buf;

    public:
        explicit UserInfo(const std::string & name);
        explicit UserInfo(uid_t uid);

        std::vector<gid_t> groups(void) const;

        inline const char* user(void) const { return st.pw_name; }

        inline const char* home(void) const { return st.pw_dir; }

        inline const char* shell(void) const { return st.pw_shell; }

        inline const char* gecos(void) const { return st.pw_gecos; }

        inline const uid_t & uid(void) const { return st.pw_uid; }

        inline const gid_t & gid(void) const { return st.pw_gid; }

        inline std::string runtime_dir(void) const { return std::string("/run/user/").append(std::to_string(st.pw_uid)); }
    };

    class GroupInfo
    {
        struct group st = {};
        std::unique_ptr<char[]> buf;

    public:
        explicit GroupInfo(const std::string & name);
        explicit GroupInfo(gid_t gid);

        std::forward_list<std::string> members(void) const;

        inline const char* group(void) const { return st.gr_name; }

        inline gid_t gid(void) const { return st.gr_gid; }
    };

    typedef std::unique_ptr<UserInfo> UserInfoPtr;
    typedef std::unique_ptr<GroupInfo> GroupInfoPtr;

    namespace Tools
    {
        UserInfoPtr getUidInfo(uid_t uid);
        UserInfoPtr getUserInfo(const std::string & user);

        std::string getUserLogin(uid_t);
        uid_t getUserUid(const std::string & user);

        std::string getUserHome(const std::string & user);
        std::forward_list<std::string> getSystemUsers(uid_t uidMin, uid_t uidMax);

        GroupInfoPtr getGidInfo(gid_t gid);
        GroupInfoPtr getGroupInfo(const std::string & group);
        gid_t getGroupGid(const std::string & group);

        std::string getHostname(void);
        bool checkUnixSocket(const std::filesystem::path &);
    }

#endif

    namespace Tools
    {
        uint32_t debugTypes(const std::list<std::string> &);

        bool binaryToFile(const void*, size_t len, const std::filesystem::path &);
        std::vector<uint8_t> fileToBinaryBuf(const std::filesystem::path &);

        std::list<std::string> readDir(const std::filesystem::path &, bool recurse);
        std::filesystem::path resolveSymLink(const std::filesystem::path &);

        std::string prettyFuncName(const std::string &);
        std::string randomHexString(size_t len);

        std::string fileToString(const std::filesystem::path &);
        std::vector<uint8_t> randomBytes(size_t bytesCount);

        std::string getTimeZone(void);

        std::vector<uint8_t> zlibCompress(const ByteArray &);
        std::vector<uint8_t> zlibUncompress(const ByteArray &, size_t real = 0);

        std::string base64Encode(const ByteArray &);
        std::vector<uint8_t> base64Decode(const std::string &);

        class StringFormat : public std::string
        {
            int cur = 1;

        public:
            explicit StringFormat(std::string_view);

            StringFormat & arg(std::string_view);
            StringFormat & arg(int);
            StringFormat & arg(double, int prec);

            StringFormat & replace(std::string_view, int);
            StringFormat & replace(std::string_view, std::string_view);
            StringFormat & replace(std::string_view, double, int prec);

            const std::string & to_string(void) const { return *this; }
        };

        struct StreamBits
        {
            std::vector<uint8_t> vecbuf;
            size_t bitpos = 0;

            bool empty(void) const;
            const std::vector<uint8_t> & toVector(void) const;
        };

        struct StreamBitsPack : StreamBits
        {
            explicit StreamBitsPack(size_t rez = 32);

            void pushBit(bool v);
            void pushValue(int val, size_t field);
            void pushAlign(void);
        };

        struct StreamBitsUnpack : StreamBits
        {
            StreamBitsUnpack(const std::vector<uint8_t> &, size_t counts, size_t field);

            bool popBit(void);
            int popValue(size_t field);
        };

        template<typename Iterator>
        Iterator nextToEnd(Iterator it1, size_t count, Iterator it2)
        {
            if(it1 != it2)
            {
                // check itbeg nexted
                for(auto num = 0; num < count; ++num)
                {
                    it1 = std::next(it1);

                    if(it1 == it2)
                    {
                        return it2;
                    }
                }
            }

            return it1;
        }

        std::list<std::string> split(std::string_view str, std::string_view sep);
        std::list<std::string> split(std::string_view str, int sep);

        template<typename... Args>
        std::string joinToString( Args... args )
        {
            std::ostringstream os;
            ( os << ... << args );
            return os.str();
        }

        template<typename Iterator>
        std::string join(Iterator it1, Iterator it2, std::string_view sep = "")
        {
            std::ostringstream os;

            for(auto it = it1; it != it2; ++it)
            {
                os << *it;

                if(std::next(it) != it2)
                {
                    os << sep;
                }
            }

            return os.str();
        }

        std::string join(const std::list<std::string> &, std::string_view sep = "");
        std::string join(const std::vector<std::string> &, std::string_view sep = "");

        std::string lower(std::string_view);
        std::string runcmd(const std::string &);

        std::string escaped(std::string_view, bool quote);
        std::string unescaped(std::string_view);

        std::string replace(std::string_view src, std::string_view pred, std::string_view val);
        std::string replace(std::string_view src, std::string_view pred, int val);

        std::string hex(int value, int width = 8);

        uint32_t crc32b(std::string_view);
        uint32_t crc32b(const uint8_t* ptr, size_t size);
        uint32_t crc32b(const uint8_t* ptr, size_t size, uint32_t magic);

        int maskShifted(uint32_t mask);
        int maskCountBits(uint32_t mask);
        uint32_t maskMaxValue(uint32_t mask);
        std::vector<uint32_t> maskUnpackBits(uint32_t mask);

        std::wstring string2wstring(std::string_view);
        std::string wstring2string(const std::wstring &);

        template<typename Iterator>
        std::string buffer2hexstring(Iterator it1, Iterator it2, size_t width = 8, std::string_view sep = ",", bool prefix = true)
        {
            std::ostringstream os;

            while(it1 != it2)
            {
                if(prefix)
                {
                    os << "0x";
                }

                os << std::setw(width) << std::setfill('0') << std::uppercase << std::hex << static_cast<int>(*it1);
                auto itn = std::next(it1);

                if(sep.size() && itn != it2) { os << sep; }

                it1 = itn;
            }

            return os.str();
        }

        // BaseSpinLock
        class SpinLock
        {
            std::atomic<bool> flag{false};

        public:
            bool tryLock(void) noexcept
            {
                return ! flag.load(std::memory_order_relaxed) &&
                       ! flag.exchange(true, std::memory_order_acquire);
            }

            void lock(void) noexcept
            {
                for(;;)
                {
                    if(! flag.exchange(true, std::memory_order_acquire))
                    {
                        break;
                    }

                    while(flag.load(std::memory_order_relaxed))
                    {
                        std::this_thread::yield();
                    }
                }
            }

            void unlock(void) noexcept
            {
                flag.store(false, std::memory_order_release);
            }
        };

        // Timeout
        template<typename TimeType = std::chrono::milliseconds>
        struct Timeout
        {
            std::chrono::steady_clock::time_point tp;
            TimeType dt;

            explicit Timeout(TimeType val) : tp(std::chrono::steady_clock::now()), dt(val)
            {
            }

            bool check(void)
            {
                auto now = std::chrono::steady_clock::now();

                if(dt < now - tp)
                {
                    tp = now;
                    return true;
                }

                return false;
            }
        };

        // BaseTimer
        class BaseTimer
        {
        protected:
            std::thread thread;
            std::atomic<bool> processed{false};

        public:
            BaseTimer() = default;
            virtual ~BaseTimer() { stop(true); }

            std::thread::id getId(void) const
            {
                return thread.get_id();
            }

            void stop(bool wait = false)
            {
                processed = false;

                if(wait && thread.joinable()) { thread.join(); }
            }

            // usage:
            // auto bt1 = BaseTimer::create<std::chrono::microseconds>(100, repeat, [=](){ func(param1, param2, param3); });
            // auto bt2 = BaseTimer::create<std::chrono::seconds>(3, repeat, func, param1, param2, param3);
            //
            template <class TimeType = std::chrono::milliseconds, class Func>
            static std::unique_ptr<BaseTimer> create(uint32_t delay, bool repeat, Func&& call)
            {
                auto ptr = std::make_unique<BaseTimer>();
                ptr->thread = std::thread([delay, repeat, timer = ptr.get(), call = std::forward<Func>(call)]()
                {
                    timer->processed = true;
                    auto start = std::chrono::steady_clock::now();

                    while(timer->processed)
                    {
                        std::this_thread::sleep_for(TimeType(1));
                        auto cur = std::chrono::steady_clock::now();

                        if(TimeType(delay) <= cur - start)
                        {
                            if(!timer->processed)
                            {
                                break;
                            }

                            call();

                            if(repeat)
                            {
                                start = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                timer->processed = false;
                            }
                        }
                    }
                });

                return ptr;
            }

            template <class TimeType = std::chrono::milliseconds, class Func, class... Args>
            static std::unique_ptr<BaseTimer> create(uint32_t delay, bool repeat, Func&& call, Args&&... args)
            {
                auto ptr = std::make_unique<BaseTimer>();
                ptr->thread = std::thread([delay, repeat, timer = ptr.get(),
                                           call = std::forward<Func>(call), args = std::make_tuple(std::forward<Args>(args)...)]()
                {
                    timer->processed = true;
                    auto start = std::chrono::steady_clock::now();

                    while(timer->processed)
                    {
                        std::this_thread::sleep_for(TimeType(1));

                        if(TimeType(delay) <= std::chrono::steady_clock::now() - start)
                        {
                            if(!timer->processed)
                            {
                                break;
                            }

                            std::apply(call, args);

                            if(repeat)
                            {
                                start = std::chrono::steady_clock::now();
                            }
                            else
                            {
                                timer->processed = false;
                            }
                        }
                    }
                });

                return ptr;
            }
        };

        //
        template <class TimeType, class Func>
        bool waitCallable(uint32_t delay, uint32_t pause, Func&& call)
        {
            auto now = std::chrono::steady_clock::now();

            while(call())
            {
                if(TimeType(delay) <= std::chrono::steady_clock::now() - now)
                {
                    return false;
                }

                std::this_thread::sleep_for(TimeType(pause));
            }

            return true;
        }

        template<typename TimeType = std::chrono::milliseconds>
        struct TimePoint
        {
            std::chrono::steady_clock::time_point tp;
            TimeType dt;

            explicit TimePoint(TimeType val) : tp(std::chrono::steady_clock::now()), dt(val)
            {
            }

            bool check(void)
            {
                auto now = std::chrono::steady_clock::now();

                if(dt < now - tp)
                {
                    tp = now;
                    return true;
                }

                return false;
            }
        };
    }

}
#define NS_FuncName LTSM::Tools::prettyFuncName(__PRETTY_FUNCTION__)

#endif // _LTSM_TOOLS_
