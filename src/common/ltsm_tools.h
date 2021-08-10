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
#include <iterator>
#include <algorithm>

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


        class FrequencyTime
        {
        protected:
            mutable std::chrono::time_point<std::chrono::system_clock> tp;

            template<typename TimeType>
            bool finishedTime(size_t val) const
            {
                auto cur = std::chrono::system_clock::now();
                if(TimeType(val) <= cur - tp)
                {
                    tp = cur;
                    return true;
                }

                return false;
            }

        public:
            FrequencyTime() : tp(std::chrono::system_clock::now()) {}

            void        reset(void) { tp = std::chrono::system_clock::now(); }
            bool        finishedMicroSeconds(int val) const { return finishedTime<std::chrono::microseconds>(val); }
            bool        finishedMilliSeconds(int val) const { return finishedTime<std::chrono::milliseconds>(val); }
            bool        finishedSeconds(int val) const { return finishedTime<std::chrono::seconds>(val); }
            bool        finishedMinutes(int val) const { return finishedTime<std::chrono::minutes>(val); }
            bool        finishedHours(int val) const { return finishedTime<std::chrono::hours>(val); }
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
    }
}

#endif // _LTSM_TOOLS_
