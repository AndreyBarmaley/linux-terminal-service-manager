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

#include <algorithm>

#include "ltsm_tools.h"
#include "ltsm_xcb_types.h"

namespace LTSM {
    namespace XCB {
        bool PointIterator::isBeginLine(void) const {
            return x == 0;
        }

        bool PointIterator::isEndLine(void) const {
            return x == limit.width - 1;
        }

        PointIterator & PointIterator::operator++(void) {
            assertm(isValid(), "invalid iterator");

            x++;

            if(x < limit.width) {
                return *this;
            }

            if(y < limit.height) {
                y++;

                if(limit.height <= y) {
                    x = -1;
                    y = -1;
                    return *this;
                }

                x = 0;
            }

            return *this;
        }

        PointIterator & PointIterator::operator--(void) {
            assertm(isValid(), "invalid iterator");

            x--;

            if(0 <= x) {
                return *this;
            }

            if(y > 0) {
                y--;

                if(y < 0) {
                    x = -1;
                    y = -1;
                    return *this;
                }

                x = limit.width - 1;
            }

            return *this;
        }

        void Region::reset(void) {
            x = -1;
            y = -1;
            width = 0;
            height = 0;
        }

        void Region::assign(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) {
            x = rx;
            y = ry;
            width = rw;
            height = rh;
        }

        void Region::assign(const Region & reg) {
            *this = reg;
        }

        Region Region::align(size_t val) const {
            Region res(x, y, width, height);

            if(auto alignX = x % val) {
                res.x -= alignX;
                res.width += alignX;
            }

            if(auto alignY = y % val) {
                res.y -= alignY;
                res.height += alignY;
            }

            if(auto alignW = res.width % val) {
                res.width += val - alignW;
            }

            if(auto alignH = res.height % val) {
                res.height += val - alignH;
            }

            return res;
        }

        void Region::join(int16_t rx, int16_t ry, uint16_t rw, uint16_t rh) {
            join({rx, ry, rw, rh});
        }

        void Region::join(const Region & reg) {
            if(invalid()) {
                x = reg.x;
                y = reg.y;
                width = reg.width;
                height = reg.height;
            } else if(! reg.empty() && *this != reg) {
                /* Horizontal union */
                auto xm = std::min(x, reg.x);
                width = std::max(x + width, reg.x + reg.width) - xm;
                x = xm;
                /* Vertical union */
                auto ym = std::min(y, reg.y);
                height = std::max(y + height, reg.y + reg.height) - ym;
                y = ym;
            }
        }

        bool Region::empty(void) const {
            return width == 0 || height == 0;
        }

        bool Region::invalid(void) const {
            return x == -1 && y == -1 && empty();
        }

        Region Region::intersected(const Region & reg) const {
            Region res;
            intersection(*this, reg, & res);
            return res;
        }

        bool Region::intersects(const Region & reg1, const Region & reg2) {
            // check reg1.empty || reg2.empty
            if(reg1.empty() || reg2.empty()) {
                return false;
            }

            // horizontal intersection
            if(std::min(reg1.x + reg1.width, reg2.x + reg2.width) <= std::max(reg1.x, reg2.x)) {
                return false;
            }

            // vertical intersection
            if(std::min(reg1.y + reg1.height, reg2.y + reg2.height) <= std::max(reg1.y, reg2.y)) {
                return false;
            }

            return true;
        }

        bool Region::intersection(const Region & reg1, const Region & reg2, Region* res) {
            bool intersects = Region::intersects(reg1, reg2);

            if(! intersects) {
                return false;
            }

            if(! res) {
                return intersects;
            }

            // horizontal intersection
            res->x = std::max(reg1.x, reg2.x);
            res->width = std::min(reg1.x + reg1.width, reg2.x + reg2.width) - res->x;
            // vertical intersection
            res->y = std::max(reg1.y, reg2.y);
            res->height = std::min(reg1.y + reg1.height, reg2.y + reg2.height) - res->y;
            return ! res->empty();
        }

        std::list<Region> Region::divideCounts(uint16_t cols, uint16_t rows) const {
            uint16_t bw = width <= cols ? 1 : width / cols;
            uint16_t bh = height <= rows ? 1 : height / rows;
            return divideBlocks(Size(bw, bh));
        }

        std::list<Region> Region::divideBlocks(const Size & sz) const {
            std::list<Region> res;
            int cw = sz.width > width ? width : sz.width;
            int ch = sz.height > height ? height : sz.height;

            for(uint16_t yy = 0; yy < height; yy += ch) {
                for(uint16_t xx = 0; xx < width; xx += cw) {
                    uint16_t fixedw = std::min(width - xx, cw);
                    uint16_t fixedh = std::min(height - yy, ch);
                    res.emplace_back(x + xx, y + yy, fixedw, fixedh);
                }
            }

            return res;
        }
    }
}
