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

#ifndef _LTSM_ZLIB_
#define _LTSM_ZLIB_

#include <vector>
#include <iterator>

#include <zlib.h>
#include "ltsm_tools.h"
#include "ltsm_application.h"

namespace LTSM::Tools {
    template<typename Cont>
    std::vector<uint8_t> zlibCompress(const Cont & cont) {
        auto data = std::data(cont);
        auto size = std::size(cont);

        if(data && size) {
            uLong dstsz = ::compressBound(size);
            std::vector<uint8_t> res(dstsz);
            int ret = ::compress(reinterpret_cast<Bytef*>(res.data()), & dstsz,
                                 reinterpret_cast<const Bytef*>(data), size);
            if(ret == Z_OK) {
                res.resize(dstsz);
                return res;
            } else {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "compress", ret);
                throw std::runtime_error(NS_FuncNameS);
            }
        }

        return {};
    }

    template<typename Cont>
    std::vector<uint8_t> zlibUncompress(const Cont & cont, size_t real = 0) {
        auto data = std::data(cont);
        auto size = std::size(cont);

        if(data && size) {
            uLong dstsz = real ? real : size * 7;
            std::vector<uint8_t> res(dstsz);
            int ret = Z_BUF_ERROR;

            while(Z_BUF_ERROR ==
                  (ret = ::uncompress(reinterpret_cast<Bytef*>(res.data()), &dstsz,
                                      reinterpret_cast<const Bytef*>(data), size))) {
                dstsz = res.size() * 2;
                res.resize(dstsz);
            }

            if(ret == Z_OK) {
                res.resize(dstsz);
                return res;
            } else {
                Application::error("{}: {} failed, error: {}", NS_FuncNameV, "uncompress", ret);
                throw std::runtime_error(NS_FuncNameS);
            }
        }

        return {};
    }
}

#endif // _LTSM_ZLIB_
