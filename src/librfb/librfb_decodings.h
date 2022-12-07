/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
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

#ifndef _LIBRFB_DECODINGS_
#define _LIBRFB_DECODINGS_

#include "ltsm_librfb.h"

namespace LTSM
{
    namespace RFB
    {
        class ClientDecoder;

        class DecodingBase
        {
        protected:
            const int           type = 0;
            int                 debug = 0;

        public:
            DecodingBase(int v);
            virtual ~DecodingBase() = default;

            virtual void        updateRegion(ClientDecoder &, const XCB::Region &) = 0;

            int                 getType(void) const;
            void                setDebug(int);
        };

        /// DecodingRaw
        class DecodingRaw : public DecodingBase
        {
        public:
            void                updateRegion(ClientDecoder &, const XCB::Region &) override;

            DecodingRaw() : DecodingBase(ENCODING_RAW) {}
        };

        /// DecodingRRE
        class DecodingRRE : public DecodingBase
        {
        public:
            void                updateRegion(ClientDecoder &, const XCB::Region &) override;

            DecodingRRE(bool co) : DecodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool                isCoRRE(void) const { return getType() == ENCODING_CORRE; }
        };

        /// DecodingHexTile
        class DecodingHexTile : public DecodingBase
        {
            int                 bgColor = -1;
            int                 fgColor = -1;

        protected:
            void                updateRegionColors(ClientDecoder &, const XCB::Region &);

        public:
            void                updateRegion(ClientDecoder &, const XCB::Region &) override;

            DecodingHexTile(bool zlib) : DecodingBase(zlib ? ENCODING_ZLIBHEX : ENCODING_HEXTILE) {}

            bool                isZlibHex(void) const { return getType() == ENCODING_ZLIBHEX; }
        };

        /// DecodingTRLE
        class DecodingTRLE : public DecodingBase
        {
        protected:
            void                updateSubRegion(ClientDecoder &, const XCB::Region &);

        public:
            void                updateRegion(ClientDecoder &, const XCB::Region &) override;

            DecodingTRLE(bool zlib) : DecodingBase(zlib ? ENCODING_ZRLE : ENCODING_TRLE) {}

            bool                isZRLE(void) const { return getType() == ENCODING_ZRLE; }
        };

        /// DecodingZlib
        class DecodingZlib : public DecodingBase
        {
        public:
            void                updateRegion(ClientDecoder &, const XCB::Region &) override;
        
            DecodingZlib() : DecodingBase(ENCODING_ZLIB) {}
        };
    }
}

#endif // _LIBRFB_DECODINGS_
