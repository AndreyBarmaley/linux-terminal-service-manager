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

#include <list>
#include <chrono>
#include <vector>
#include <string>
#include <iomanip>
#include <iterator>
#include <algorithm>

#include <tuple>
#include <memory>
#include <atomic>
#include <thread>
#include <utility>
#include <functional>

namespace LTSM
{
    namespace Tools
    {
        class StringFormat : public std::string
        {
            int             cur;

        public:
            StringFormat(const std::string &);

            StringFormat & arg(const std::string &);
            StringFormat & arg(const char*);
            StringFormat & arg(int);
            StringFormat & arg(double, int prec);

            StringFormat & replace(const char*, int);
            StringFormat & replace(const char*, const std::string &);
            StringFormat & replace(const char*, double, int prec);
        };

        struct StreamBitsPack
        {
            std::vector<uint8_t> vecbuf;
            size_t               bitpos;
        
            StreamBitsPack();
        
            void        pushBit(bool v);
            void        pushValue(int val, size_t field);
            void        pushAlign(void);
        
            bool        empty(void) const;
            const std::vector<uint8_t> & toVector(void) const;
        };

        std::list<std::string> split(const std::string & str, const std::string & sep);
        std::list<std::string> split(const std::string & str, int sep);

        std::string     join(const std::list<std::string> &);
        std::string     join(const std::list<std::string> &, const std::string & sep);

        std::string     lower(std::string);
        std::string     runcmd(const std::string &);

        std::string     escaped(const std::string &, bool quote);
        std::string     unescaped(std::string);

        std::string     replace(const std::string & src, const char* pred, const std::string & val);
        std::string     replace(const std::string & src, const char* pred, int val);

        std::string     getenv(const char*, const char* = nullptr);

        std::string     hex(int value, int width = 8);

        uint32_t        crc32b(const uint8_t* ptr, size_t size);
        uint32_t        crc32b(const uint8_t* ptr, size_t size, uint32_t magic);

        bool            checkUnixSocket(const std::string &);

        size_t		maskShifted(size_t mask);
        size_t		maskMaxValue(uint32_t mask);

	template<typename Int>
	std::string buffer2hexstring(const Int* data, size_t length, size_t width = 8, const std::string & sep = ",", bool prefix = true)
	{
    	    std::ostringstream os;
    	    for(size_t it = 0; it != length; ++it)
    	    {
                if(prefix)
        	    os << "0x";
                os << std::setw(width) << std::setfill('0') << std::uppercase << std::hex << static_cast<int>(data[it]);
        	if(sep.size() && it + 1 != length) os << sep;
    	    }

    	    return os.str();
	}

        // BaseSpinLock
        class SpinLock
        {
            std::atomic<bool> flag{0};

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
                        break;

                    while(flag.load(std::memory_order_relaxed))
                        std::this_thread::yield();
                }
            }

            void unlock(void) noexcept
            {
                flag.store(false, std::memory_order_release);
            }
        };


	// BaseTimer
	class BaseTimer
	{
	protected:
	    std::thread         thread;
	    std::atomic<bool>   processed;

	public:
	    BaseTimer() : processed(false) {}
            ~BaseTimer() { stop(true); }
    
	    std::thread::id 	getId(void) const
            {
                return thread.get_id();
            }

	    void		stop(bool wait = false)
            {
                processed = false;
                if(wait && thread.joinable()) thread.join();
            }

	    // usage:
	    // auto bt1 = BaseTimer::create<std::chrono::microseconds>(100, repeat, [=](){ func(param1, param2, param3); });
	    // auto bt2 = BaseTimer::create<std::chrono::seconds>(3, repeat, func, param1, param2, param3);
	    //
	    template <class TimeType = std::chrono::milliseconds, class Func>
	    static std::unique_ptr<BaseTimer> create(uint32_t delay, bool repeat, Func&& call)
	    {
    		auto ptr = std::unique_ptr<BaseTimer>(new BaseTimer());
    		ptr->thread = std::thread([delay, repeat, timer = ptr.get(), call = std::move(call)]()
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
				break;

                	    call();

                            if(repeat)
        	                start = std::chrono::steady_clock::now();
                            else
                	        timer->processed = false;
            		}
        	    }
    		});
    		return ptr;
	    }

	    template <class TimeType = std::chrono::milliseconds, class Func, class... Args>
	    static std::unique_ptr<BaseTimer> create(uint32_t delay, bool repeat, Func&& call, Args&&... args)
	    {
    		auto ptr = std::unique_ptr<BaseTimer>(new BaseTimer());
    		ptr->thread = std::thread([delay, repeat, timer = ptr.get(),
		    call = std::move(call), args = std::make_tuple(std::forward<Args>(args)...)]()
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
				break;

                	    std::apply(call, args);

                            if(repeat)
        	                start = std::chrono::steady_clock::now();
                            else
                	        timer->processed = false;
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
            	auto cur = std::chrono::steady_clock::now();
            	if(TimeType(delay) <= cur - now)
            	    return false;

        	std::this_thread::sleep_for(TimeType(pause));
	    }
	    return true;
        }
    }
}

#endif // _LTSM_TOOLS_
