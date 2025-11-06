/***************************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#ifndef _LTSM_XCB_TYPES_
#define _LTSM_XCB_TYPES_

#include <list>
#include <utility>
#include <cinttypes>
#include <stdexcept>

#include "ltsm_compat.h"

namespace LTSM {
    struct xcb_error : public std::runtime_error {
        explicit xcb_error(std::string_view what) : std::runtime_error(view2string(what)) {}
    };

    namespace XCB {
        struct Point {
            int16_t x = -1;
            int16_t y = -1;

            Point(int16_t px, int16_t py) : x(px), y(py) {}

            Point() = default;
            ~Point() = default;

            inline bool isValid(void) const {
                return 0 <= x && 0 <= y;
            }

            inline Point operator+(const Point & pt) const {
                return Point(x + pt.x, y + pt.y);
            }
            inline Point operator-(const Point & pt) const {
                return Point(x - pt.x, y - pt.y);
            }

            inline bool operator==(const Point & pt) const {
                return pt.x == x && pt.y == y;
            }
            inline bool operator!=(const Point & pt) const {
                return pt.x != x || pt.y != y;
            }
        };

        struct Size {
            uint16_t width = 0;
            uint16_t height = 0;

            Size(uint16_t sw, uint16_t sh) : width(sw), height(sh) {}

            Size() = default;
            ~Size() = default;

            inline bool isEmpty(void) const {
                return width == 0 || height == 0;
            }

            void reset(void) {
                width = 0;
                height = 0;
            }

            inline bool operator==(const Size & sz) const {
                return sz.width == width && sz.height == height;
            }
            inline bool operator!=(const Size & sz) const {
                return sz.width != width || sz.height != height;
            }
        };

        struct PointIterator : Point {
            const Size limit;
            PointIterator(int16_t px, int16_t py, const Size & sz) : Point(px, py), limit(sz) {}

            PointIterator() = default;
            ~PointIterator() = default;

            PointIterator & operator++(void);
            PointIterator & operator--(void);

            bool isValid(void) const {
                return Point::isValid() && x < limit.width && y < limit.height;
            }

            bool isBeginLine(void) const;
            bool isEndLine(void) const;
        };

        struct Region : Point, Size {
            Region() = default;
            ~Region() = default;

            Region(const Point & pt, const Size & sz) : Point(pt), Size(sz) {}
            Region(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) : Point(rx, ry), Size(rw, rh) {}

            inline const Point & topLeft(void) const {
                return *this;
            }
            inline const Size & toSize(void) const {
                return *this;
            }

            PointIterator coordBegin(void) const {
                return PointIterator(0, 0, toSize());
            }

            inline bool operator== (const Region & rt) const {
                return rt.topLeft() == topLeft() && rt.toSize() == toSize();
            }
            inline bool operator!= (const Region & rt) const {
                return rt.topLeft() != topLeft() || rt.toSize() != toSize();
            }

            void reset(void);

            void assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void assign(const Region &);

            void join(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh);
            void join(const Region &);

            bool empty(void) const;
            bool invalid(void) const;

            Region intersected(const Region &) const;
            Region align(size_t) const;

            static bool intersects(const Region &, const Region &);
            static bool intersection(const Region &, const Region &, Region* res);

            std::list<Region> divideBlocks(const Size &) const;
            std::list<Region> divideCounts(uint16_t cols, uint16_t rows) const;
        };

        inline Region operator+ (const Region & reg, const Point & pt) {
            return Region(reg.topLeft() + pt, reg.toSize());
        }
        inline Region operator- (const Region & reg, const Point & pt) {
            return Region(reg.topLeft() - pt, reg.toSize());
        }

        struct HasherRegion {
            size_t operator()(const Region & reg) const {
                return std::hash<uint64_t>()((static_cast<uint64_t>(reg.x) << 48) | (static_cast<uint64_t>(reg.y) << 32) |
                                             (static_cast<uint64_t>(reg.width) << 16) | static_cast<uint64_t>(reg.height));
            }
        };

        struct RegionPixel : std::pair<Region, uint32_t> {
            RegionPixel(const Region & reg, uint32_t pixel) : std::pair<XCB::Region, uint32_t>(reg, pixel) {}

            RegionPixel() = default;

            const uint32_t & pixel(void) const {
                return second;
            }

            const Region & region(void) const {
                return first;
            }
        };
    }
}

#endif // _LTSM_XCB_TYPES_
