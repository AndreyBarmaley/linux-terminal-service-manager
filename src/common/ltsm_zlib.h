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

#include <span>
#include <vector>
#include <iterator>

#include "ltsm_compat.h"
#include <zlib.h>

namespace LTSM {
    struct zlib_error : public std::runtime_error {
        explicit zlib_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };
}

namespace LTSM::Tools {
    std::vector<uint8_t> zlibCompress(std::span<const uint8_t> cont);
    std::vector<uint8_t> zlibUncompress(std::span<const uint8_t> cont, size_t real = 0);
}

namespace LTSM::ZLib {
    /// @brief: zlib compress input stream only
    class InflateBase {
      protected:
        z_stream zs{};

      public:
        InflateBase();
        virtual ~InflateBase();

        InflateBase(const InflateBase &) = delete;
        InflateBase & operator=(const InflateBase &) = delete;

        InflateBase(InflateBase &&) noexcept = default;
        InflateBase & operator=(InflateBase &&) noexcept = default;

        std::vector<uint8_t> inflateData(const std::vector<uint8_t> & buf);
        void reset(void);

      protected:
        /// flushPolicy: Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FINISH, Z_BLOCK or Z_TREES
        std::vector<uint8_t> inflateData(const void* buf, size_t len, int flushPolicy = Z_NO_FLUSH);
    };
}

#endif // _LTSM_ZLIB_
