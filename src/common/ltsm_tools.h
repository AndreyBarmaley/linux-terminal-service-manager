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

        struct StreamBits
        {
            std::vector<uint8_t> &	data;
            size_t			seek;

            StreamBits(std::vector<uint8_t> & v, size_t offset = 0) : data(v), seek(offset)
            {
            }

            void        pushBitBE(bool v);
            void        pushBitLE(bool v);
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
        size_t		maskMaxValue(size_t mask);

	template<typename Int>
	std::string vector2hexstring(const std::vector<Int> & vec, size_t width = 8, const std::string & sep = ",")
	{
    	    std::ostringstream os;
    	    for(auto it = vec.begin(); it != vec.end(); ++it)
    	    {
        	os << "0x" << std::setw(width) << std::setfill('0') << std::uppercase << std::hex << static_cast<int>(*it);
        	if(sep.size() && std::next(it) != vec.end()) os << sep;
    	    }

    	    return os.str();
	}

	// BaseTimer
	class BaseTimer
	{
	protected:
	    std::thread         thread;
	    std::atomic<bool>   processed;

	public:
	    BaseTimer() : processed(false) {}
            ~BaseTimer();
    
	    std::thread::id 	getId(void) const;
	    bool		isRunning(void) const;

	    void		stop(void);
	    void		join(void);

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
        	    auto start = std::chrono::system_clock::now();
        	    while(timer->processed)
        	    {
            		std::this_thread::sleep_for(TimeType(1));
            		auto cur = std::chrono::system_clock::now();
            		if(TimeType(delay) <= cur - start)
            		{
                	    call();

                            if(repeat)
        	                start = std::chrono::system_clock::now();
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
        	    auto start = std::chrono::system_clock::now();
        	    while(timer->processed)
        	    {
            		std::this_thread::sleep_for(TimeType(1));
            		auto cur = std::chrono::system_clock::now();
            		if(TimeType(delay) <= cur - start)
            		{
                	    std::apply(call, args);

                            if(repeat)
        	                start = std::chrono::system_clock::now();
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
            auto now = std::chrono::system_clock::now();
    	    while(call())
    	    {
            	auto cur = std::chrono::system_clock::now();
            	if(TimeType(delay) <= cur - now)
            	    return false;

        	std::this_thread::sleep_for(TimeType(pause));
	    }
	    return true;
        }
    }
}

#endif // _LTSM_TOOLS_
