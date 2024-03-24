/***********************************************************************
 *   Copyright © 2024 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LTSM_FUSE_
#define _LTSM_FUSE_

#define LTSM_FUSE2SESSION_VERSION 20240304

namespace LTSM
{
    namespace FuseOp
    {
        enum
        {
            Init = 0xFF01,
            Quit = 0xFF02,
            GetAttr = 0xFF03,
            ReadDir = 0xFF04,
            Open = 0xFF05,
            Read = 0xFF06,
            Release = 0xFF07,
            //
            Access = 0xFF08,
            RmDir = 0xFF09,
            UnLink = 0xFF10,
            Rename = 0xFF11,
            Truncate = 0xFF12,
            Write = 0xFF13,
            Create = 0xFF14,
            //
            Lookup = 0xFF15
        };
    }
}

#endif // _LTSM_FUSE_
