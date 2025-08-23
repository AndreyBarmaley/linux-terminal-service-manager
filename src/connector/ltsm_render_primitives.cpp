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

#include "ltsm_render_primitives.h"

namespace LTSM::Connector
{
    XCB::Region RenderPrimitive::xcbRegion(void) const
    {
        return tupleRegionToXcbRegion(_region);
    }

    Color RenderColored::toColor(void) const
    {
        return tupleColorToColor(_color);
    }

    void RenderRect::renderTo(FrameBuffer & fb) const
    {
        XCB::Region section;

        if(XCB::Region::intersection(fb.region(), xcbRegion(), & section))
        {
            if(_fill)
            {
                fb.fillColor(section - fb.region().topLeft(), toColor());
            }
            else
            {
                fb.drawRect(section - fb.region().topLeft(), toColor());
            }
        }
    }

    void RenderText::renderTo(FrameBuffer & fb) const
    {
        const XCB::Region reg = xcbRegion();

        if(XCB::Region::intersects(fb.region(), reg))
        {
            fb.renderText(_text, toColor(), reg.topLeft() - fb.region().topLeft());
        }
    }
}
