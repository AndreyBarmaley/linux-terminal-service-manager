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

#include <list>
#include <thread>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"

namespace LTSM {
    namespace RFB {
        /// DecoderStream
        class DecoderStream : public NetworkStream {
          public:
            int recvPixel(void);
            int recvCPixel(void);
            size_t recvRunLength(void);
            size_t recvZlibData(ZLib::InflateStream*, bool uint16sz = false);
            void recvRegionUpdatePixels(const XCB::Region &);

            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual const PixelFormat & clientFormat(void) const = 0;

            virtual void setPixel(const XCB::Point &, uint32_t pixel) = 0;
            virtual void fillPixel(const XCB::Region &, uint32_t pixel) = 0;

            virtual void updateRawPixels(const XCB::Region &, const void*, uint32_t pitch, const PixelFormat &) = 0;
            virtual void updateRawPixels2(const XCB::Region &, const void*, uint8_t depth, uint32_t pitch, uint32_t sdlFormat) = 0;

            virtual XCB::Size clientSize(void) const = 0;
            virtual std::string clientPrefferedEncoding(void) const {
                return "";
            }
        };

        /// DecoderWrapper
        class DecoderWrapper : public DecoderStream {
          protected:
            NetworkStream* stream = nullptr;
            DecoderStream* owner = nullptr;

          public:
            DecoderWrapper(NetworkStream* ns, DecoderStream* st) : stream(ns), owner(st) {}

            DecoderWrapper(const DecoderWrapper &) = delete;
            DecoderWrapper & operator=(const DecoderWrapper &) = delete;

#ifdef LTSM_WITH_GNUTLS
            void setupTLS(gnutls::session* ses) const override {
                stream->setupTLS(ses);
            }

#endif

            bool hasInput(void) const override {
                return stream->hasInput();
            }

            size_t hasData(void) const override {
                return stream->hasData();
            }

            uint8_t peekInt8(void) const override {
                return stream->peekInt8();
            }

            void sendRaw(const void* ptr, size_t len) override {
                stream->sendRaw(ptr, len);
            }

            void recvRaw(void* ptr, size_t len) const override {
                stream->recvRaw(ptr, len);
            }

            const PixelFormat & serverFormat(void) const override {
                return owner->serverFormat();
            }

            const PixelFormat & clientFormat(void) const override {
                return owner->clientFormat();
            }

            void setPixel(const XCB::Point & pt, uint32_t pixel) override {
                owner->setPixel(pt, pixel);
            }

            void fillPixel(const XCB::Region & rt, uint32_t pixel) override {
                owner->fillPixel(rt, pixel);
            }

            void updateRawPixels(const XCB::Region & wrt, const void* data, uint32_t pitch,
                                 const PixelFormat & pf) override {
                owner->updateRawPixels(wrt, data, pitch, pf);
            }

            void updateRawPixels2(const XCB::Region & wrt, const void* data, uint8_t depth,
                                  uint32_t pitch, uint32_t sdlFormat) override {
                owner->updateRawPixels2(wrt, data, depth, pitch, sdlFormat);
            }

            XCB::Size clientSize(void) const override {
                return owner->clientSize();
            }

            std::string clientPrefferedEncoding(void) const override {
                return owner->clientPrefferedEncoding();
            }
        };

        /// DecodingBase
        class DecodingBase {
          protected:
            const int type = 0;
            int threads = 4;

          public:
            DecodingBase(int v);
            virtual ~DecodingBase() = default;

            virtual void updateRegion(DecoderStream &, const XCB::Region &) = 0;
            virtual void resizedEvent(const XCB::Size &) { /* empty */ }

            virtual void waitUpdateComplete(void) { /* empty */ }

            int getType(void) const;
            void setThreads(int);
        };

        /// DecodingRaw
        class DecodingRaw : public DecodingBase {
          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;

            DecodingRaw() : DecodingBase(ENCODING_RAW) {}
        };

        /// DecodingRRE
        class DecodingRRE : public DecodingBase {
          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;

            DecodingRRE(bool co) : DecodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool isCoRRE(void) const {
                return getType() == ENCODING_CORRE;
            }
        };

        /// DecodingHexTile
        class DecodingHexTile : public DecodingBase {
            int bgColor = -1;
            int fgColor = -1;

          protected:
            void updateRegionColors(DecoderStream &, const XCB::Region &);

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;

            DecodingHexTile(bool zlib) : DecodingBase(zlib ? ENCODING_ZLIBHEX : ENCODING_HEXTILE) {}

            bool isZlibHex(void) const {
                return getType() == ENCODING_ZLIBHEX;
            }
        };

        /// DecodingTRLE
        class DecodingTRLE : public DecodingBase {
            std::unique_ptr<ZLib::InflateStream> zlib;

          protected:
            void updateSubRegion(DecoderStream &, const XCB::Region &);

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;

            DecodingTRLE(bool zlib);

            bool isZRLE(void) const {
                return getType() == ENCODING_ZRLE;
            }
        };

        /// DecodingZlib
        class DecodingZlib : public DecodingBase {
            std::unique_ptr<ZLib::InflateStream> zlib;

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;

            DecodingZlib();
        };

#ifdef LTSM_DECODING

#ifdef LTSM_DECODING_LZ4
        /// DecodingLZ4
        class DecodingLZ4 : public DecodingBase {
            std::list<std::thread> jobs;

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;
            void waitUpdateComplete(void) override;

            DecodingLZ4() : DecodingBase(ENCODING_LTSM_LZ4) {}
        };
#endif
#ifdef LTSM_DECODING_TJPG
        /// DecodingTJPG
        class DecodingTJPG : public DecodingBase {
            std::list<std::thread> jobs;

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;
            void waitUpdateComplete(void) override;

            DecodingTJPG() : DecodingBase(ENCODING_LTSM_TJPG) {}
        };
#endif
#ifdef LTSM_DECODING_QOI
        /// DecodingQOI
        class DecodingQOI : public DecodingBase {
            std::list<std::thread> jobs;

          protected:
            BinaryBuf decodeBGRx(const std::vector<uint8_t> &, const XCB::Size & rsz, const PixelFormat &, uint32_t pitch) const;

          public:
            void updateRegion(DecoderStream &, const XCB::Region &) override;
            void waitUpdateComplete(void) override;

            DecodingQOI() : DecodingBase(ENCODING_LTSM_QOI) {}
        };
#endif
#endif
    }
}

#endif // _LIBRFB_DECODINGS_
