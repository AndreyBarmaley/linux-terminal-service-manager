/***********************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#ifndef _LTSM_RENDER_PRIMITIVES_
#define _LTSM_RENDER_PRIMITIVES_

#include <tuple>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "ltsm_framebuffer.h"
#include "ltsm_xcb_types.h"

namespace LTSM::Connector {
    enum class RenderType { RenderRect, RenderText };

    using TuplePosition = sdbus::Struct<int16_t, int16_t>;
    using TupleRegion = sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>;
    using TupleColor = sdbus::Struct<uint8_t, uint8_t, uint8_t>;

    inline Color tupleColorToColor(const TupleColor & tc) {
        uint8_t r, g, b;
        std::tie(r, g, b) = tc;
        return Color{r, g, b, 0};
    }

    inline XCB::Region tupleRegionToXcbRegion(const TupleRegion & tr) {
        int16_t x, y;
        uint16_t width, height;
        std::tie(x, y, width, height) = tr;
        return XCB::Region{x, y, width, height};
    }

    struct RenderPrimitive {
        const RenderType _type;
        TupleRegion _region;

        RenderPrimitive(const RenderType & rt, const TupleRegion & tr) : _type(rt), _region(tr) {}
        virtual ~RenderPrimitive() {}

        XCB::Region xcbRegion(void) const;
        virtual void renderTo(FrameBuffer &) const = 0;
    };

    struct RenderColored : RenderPrimitive {
        TupleColor _color;

        RenderColored(const RenderType & rt, const TupleRegion & tr, const TupleColor & col)
            : RenderPrimitive(rt, tr), _color(col) {}

        Color toColor(void) const;
    };

    struct RenderRect : RenderColored {
        bool _fill;

        RenderRect(const TupleRegion & tr, const TupleColor & col, bool f)
            : RenderColored(RenderType::RenderRect, tr, col), _fill(f) {}

        void renderTo(FrameBuffer &) const;
    };

    struct RenderText : RenderColored {
        std::string _text;

        RenderText(const std::string & str, const TupleRegion & tr, const TupleColor & col)
            : RenderColored(RenderType::RenderText, tr, col), _text(str) {}

        void renderTo(FrameBuffer &) const;
    };

    using RenderPrimitivePtr = std::unique_ptr<RenderPrimitive>;
}

#endif // _LTSM_RENDER_PRIMITIVES_
