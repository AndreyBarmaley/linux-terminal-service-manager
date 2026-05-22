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

#include <vector>
#include <functional>

#include "ltsm_librfb.h"
#include "ltsm_sockets.h"

#ifdef LTSM_WITH_BOOST
#include <boost/asio/post.hpp>
#endif

#ifdef LTSM_DECODING_TJPG
#include "turbojpeg.h"
#endif

namespace LTSM {
    namespace RFB {
        using PostDecoderJobCb = std::function<BinaryBuf(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &)>;

        /// DecoderStream
        class DecoderStream : public NetworkStream {
          public:
            int recvPixel(void);
            int recvCPixel(void);
            size_t recvRunLength(void);
            void recvRegionUpdatePixels(const XCB::Region &);

            virtual const PixelFormat & serverFormat(void) const = 0;
            virtual const PixelFormat & clientFormat(void) const = 0;

            virtual void setPixel(const XCB::Point &, uint32_t pixel) = 0;
            virtual void fillPixel(const XCB::Region &, uint32_t pixel) = 0;

            virtual void updateRawPixels(const XCB::Region &, std::vector<uint8_t>&& buf, uint32_t pitch, const PixelFormat &) = 0;

            virtual XCB::Size clientSize(void) const = 0;
            virtual int clientPrefferedVideoEncoding(void) const {
                return 0;
            }
            virtual int clientPrefferedAudioEncoding(void) const {
                return 0;
            }

            virtual void postDecoderJob(PostDecoderJobCb &&, std::vector<uint8_t> &&, const XCB::Region &, uint32_t pitch, const PixelFormat &) = 0;
            virtual void waitDecoderJobs(void) = 0;
        };

        /// DecoderWrapper
        class DecoderWrapper : public DecoderStream {
            std::vector<uint8_t> buf_;
            StreamBufRef sb_;

            DecoderStream* owner_ = nullptr;

          public:
            DecoderWrapper(std::vector<uint8_t> && buf, DecoderStream* st) : buf_(std::move(buf)), owner_(st) {
                sb_.reset(buf_.data(), buf_.size());
            }

            DecoderWrapper(const DecoderWrapper &) = delete;
            DecoderWrapper & operator=(const DecoderWrapper &) = delete;

#ifdef LTSM_WITH_GNUTLS
            void setupTLS(gnutls::session* ses) const override {
                throw rfb_error(NS_FuncNameS + " usupported");
            }

#endif
            bool hasInput(void) const override {
                return sb_.last();
            }

            size_t hasData(void) const override {
                return sb_.last();
            }

            void sendRaw(const void* ptr, size_t len) override {
                throw rfb_error(NS_FuncNameS + " usupported");
            }

            void recvRaw(void* ptr, size_t len) const override {
                sb_.readTo(ptr, len);
            }

            const PixelFormat & serverFormat(void) const override {
                return owner_->serverFormat();
            }

            const PixelFormat & clientFormat(void) const override {
                return owner_->clientFormat();
            }

            void setPixel(const XCB::Point & pt, uint32_t pixel) override {
                owner_->setPixel(pt, pixel);
            }

            void fillPixel(const XCB::Region & rt, uint32_t pixel) override {
                owner_->fillPixel(rt, pixel);
            }

            void updateRawPixels(const XCB::Region & wrt, std::vector<uint8_t>&& buf, uint32_t pitch,
                                 const PixelFormat & pf) override {
                owner_->updateRawPixels(wrt, std::move(buf), pitch, pf);
            }

            void postDecoderJob(RFB::PostDecoderJobCb && cb, std::vector<uint8_t> && buf, const XCB::Region & reg, uint32_t pitch, const PixelFormat & pf) override {
                owner_->postDecoderJob(std::move(cb), std::move(buf), reg, pitch, pf);
            }

            void waitDecoderJobs(void) override {
                owner_->waitDecoderJobs();
            }

            XCB::Size clientSize(void) const override {
                return owner_->clientSize();
            }

            int clientPrefferedVideoEncoding(void) const override {
                return owner_->clientPrefferedVideoEncoding();
            }

            int clientPrefferedAudioEncoding(void) const override {
                return owner_->clientPrefferedAudioEncoding();
            }
        };

        /// DecodingBase
        class DecodingBase {
            const int type_ = 0;

          public:
            DecodingBase(int v);
            virtual ~DecodingBase() = default;

            virtual void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) { /* empty */ }
            virtual void updateRegionStream(DecoderStream &, const XCB::Region &) { /* empty */ }
            virtual void resizedEvent(const XCB::Size &) { /* empty */ }

            int type(void) const;
        };

        /// DecodingRaw
        class DecodingRaw : public DecodingBase {
          public:
            void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) override;

            DecodingRaw() : DecodingBase(ENCODING_RAW) {}
        };

        /// DecodingRRE
        class DecodingRRE : public DecodingBase {
          public:
            void updateRegionStream(DecoderStream &, const XCB::Region &) override;

            DecodingRRE(bool co) : DecodingBase(co ? ENCODING_CORRE : ENCODING_RRE) {}

            bool isCoRRE(void) const {
                return type() == ENCODING_CORRE;
            }
        };

        /// DecodingHexTile
        class DecodingHexTile : public DecodingBase {
            int bgColor = -1;
            int fgColor = -1;

          protected:
            void updateRegionStreamColors(DecoderStream &, const XCB::Region &);

          public:
            void updateRegionStream(DecoderStream &, const XCB::Region &) override;

            DecodingHexTile(bool zlib) : DecodingBase(zlib ? ENCODING_ZLIBHEX : ENCODING_HEXTILE) {}

            bool isZlibHex(void) const {
                return type() == ENCODING_ZLIBHEX;
            }
        };

        /// DecodingTRLE
        class DecodingTRLE : public DecodingBase {
            std::unique_ptr<ZLib::InflateBase> zlib;

          protected:
            void updateSubRegion(DecoderStream &, const XCB::Region &);

          public:
            void updateRegionStream(DecoderStream &, const XCB::Region &) override;

            DecodingTRLE(bool zlib);

            bool isZRLE(void) const {
                return type() == ENCODING_ZRLE;
            }
        };

        /// DecodingZlib
        class DecodingZlib : public DecodingBase {
            std::unique_ptr<ZLib::InflateBase> zlib;

          public:
            void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) override;

            DecodingZlib();
        };

#ifdef LTSM_DECODING
#ifdef LTSM_DECODING_LZ4
        /// DecodingLZ4
        class DecodingLZ4 : public DecodingBase {

          protected:
            std::vector<uint8_t> decodeLZ4(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) override;

            DecodingLZ4() : DecodingBase(ENCODING_LTSM_LZ4) {}
        };
#endif
#ifdef LTSM_DECODING_TJPG
        /// DecodingTJPG
        class DecodingTJPG : public DecodingBase {

            // TJPF: RGBX, BGRX, XRGB, XBGR
            // from lowest to highest memory address
            const int pixfmt = TJPF_BGRX;

          protected:
            std::vector<uint8_t> decodeJPG(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) override;

            DecodingTJPG() : DecodingBase(ENCODING_LTSM_TJPG) {}
        };
#endif
#ifdef LTSM_DECODING_QOI
        /// DecodingQOI
        class DecodingQOI : public DecodingBase {

          protected:
            std::vector<uint8_t> decodeBGRx(const std::vector<uint8_t> &, const XCB::Region &, uint32_t pitch, const PixelFormat &) const;

          public:
            void updateRegionBuf(BinaryBuf &&, DecoderStream &, const XCB::Region &) override;

            DecodingQOI() : DecodingBase(ENCODING_LTSM_QOI) {}
        };
#endif
#endif
    }
}

#endif // _LIBRFB_DECODINGS_
