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

#ifndef _LTSM_CONNECTOR_
#define _LTSM_CONNECTOR_

#include <gnutls/gnutls.h>

#include "zlib.h"

#include "ltsm_global.h"
#include "ltsm_dbus_proxy.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"

namespace LTSM
{
    enum class RenderType { RenderRect, RenderText };

    struct RenderPrimitive
    {
        const RenderType type;

        RenderPrimitive(const RenderType & val) : type(val) {}
        virtual ~RenderPrimitive() {}
    };

    struct RenderRect : RenderPrimitive
    {
        sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> region;
        sdbus::Struct<uint8_t, uint8_t, uint8_t>            color;
        bool                                                fill;

        RenderRect(const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rt, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & col, bool v)
            : RenderPrimitive(RenderType::RenderRect), region(rt), color(col), fill(v) {}
    };

    struct RenderText : RenderPrimitive
    {
        std::string	                                    text;
        sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> region;
        sdbus::Struct<uint8_t, uint8_t, uint8_t>            color;

        RenderText(const std::string & str, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rt, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & col)
            : RenderPrimitive(RenderType::RenderText), text(str), region(rt), color(col) {}
    };

    struct TLS
    {
    	gnutls_session_t		 session;
	gnutls_dh_params_t		 dhparams;

	TLS(int debug = 0);
	virtual ~TLS();

	std::string			sessionDescription(void) const;

	int				recvInt8(void);
	bool				sendInt8(uint8_t val);
	bool				sendRaw(const void* buf, size_t length);

	virtual bool			initSession(const std::string & priority, int mode = GNUTLS_SERVER);
    };

    struct AnonTLS : TLS
    {
    	gnutls_anon_server_credentials_t cred;

	AnonTLS(int debug = 0) : TLS(debug), cred(nullptr) {}
	~AnonTLS();

	bool				initSession(const std::string & priority, int mode = GNUTLS_SERVER) override;
    };

    struct x509TLS : TLS
    {
	gnutls_certificate_credentials_t cred;
	const std::string		 caFile, certFile, keyFile, crlFile;

	x509TLS(const std::string & ca, const std::string & cert, const std::string & key, const std::string & crl, int debug = 0)
	    : TLS(debug), cred(nullptr), caFile(ca), certFile(cert), keyFile(key), crlFile(crl) {}
	~x509TLS();

	bool				initSession(const std::string & priority, int mode = GNUTLS_SERVER) override;
    };

    class BaseStream
    {
    protected:
        FILE*                           fdin;
        FILE*                           fdout;
	bool				ioerr;
	std::array<char, 1492>		fdbuf;

    public:
        BaseStream();
        virtual ~BaseStream();

        BaseStream &                    sendIntBE16(uint16_t);
        BaseStream &                    sendIntBE32(uint32_t);

        BaseStream &                    sendIntLE16(uint16_t);
        BaseStream &                    sendIntLE32(uint32_t);

        virtual BaseStream &            sendInt8(uint8_t val);
        BaseStream &                    sendInt16(uint16_t val);
        BaseStream &                    sendInt32(uint32_t val);

        virtual BaseStream &            sendRaw(const void*, size_t);

        virtual bool                    hasInput(void) const;
        virtual void                    sendFlush(void) const;

        int		                recvIntBE16(void);
        int		                recvIntBE32(void);

        int		                recvIntLE16(void);
        int		                recvIntLE32(void);

        virtual int		        recvInt8(void);
        int		                recvInt16(void);
        int		                recvInt32(void);

        void                            recvSkip(size_t);

        BaseStream &                    sendString(const std::string &);
        std::string	                recvString(size_t);

        virtual int	                communication(void) = 0;
    };

    class TLS_Stream : public BaseStream
    {
    protected:
	std::unique_ptr<TLS>            tls;
	bool				handshake;

    public:
	TLS_Stream() : handshake(false) {}
	~TLS_Stream();

	bool				tlsInitAnonHandshake(const std::string & priority, int debug);
	bool				tlsInitX509Handshake(const std::string & priority, const std::string & caFile, const std::string & certFile, const std::string & keyFile, const std::string & crlFile, int debug);

        bool                            hasInput(void) const override;
        void                            sendFlush(void) const override;
        TLS_Stream &                    sendInt8(uint8_t val) override;
        TLS_Stream &                    sendRaw(const void*, size_t) override;
        int		                recvInt8(void) override;
    };

    struct zlibStream : z_stream
    {
	std::vector<uint8_t> outbuf;

        zlibStream()
        {
            zalloc = 0;
            zfree = 0;
            opaque = 0;
            total_in = 0;
            total_out = 0;
            avail_in = 0;
            next_in = 0;
            avail_out = 0;
            next_out = 0;
            data_type = Z_BINARY;
        }

        ~zlibStream()
        {
            deflateEnd(this);
        }

        void                            pushInt8(uint8_t val);
        void                            pushRaw(const uint8_t*, size_t);

	std::vector<uint8_t>		syncFlush(bool finish = false);
    };

    class ZlibOutStream : public TLS_Stream
    {
    protected:
	std::unique_ptr<zlibStream>	zlibStreamPtr;
	bool                            deflateStarted;

    public:
	ZlibOutStream() : deflateStarted(false) {}

	void 				zlibDeflateStart(size_t reserve);
	std::vector<uint8_t>		zlibDeflateStop(void);

        ZlibOutStream &                 sendInt8(uint8_t val) override;
        ZlibOutStream &                 sendRaw(const void*, size_t) override;
    };

    namespace Connector
    {
        /* Connector::SignalProxy */
        class SignalProxy : public sdbus::ProxyInterfaces<Manager::Service_proxy>
        {
        protected:
            sdbus::IConnection*         _conn;
            const JsonObject*           _config;
            int                         _display;
            std::string		        _conntype;
            std::string			_remoteaddr;
            bool                        _xcbDisableMessages;
            int                         _encodingThreads;

            std::list<std::unique_ptr<RenderPrimitive>>
                                        _renderPrimitives;

            std::unique_ptr<XCB::RootDisplay>
        				_xcbDisplay;
	    std::unique_ptr<XCB::SelectionOwner>
					_xcbSelectionOwner;
            XCB::SHM                    _shmInfo;
            XCB::Damage                 _damageInfo;

        private:
            // dbus virtual signals
            void                        onLoginFailure(const int32_t & display, const std::string & msg) override {}
            void                        onHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass) override {}
            void                        onHelperAutoLogin(const int32_t& display, const std::string& login, const std::string& pass) override {}
            void                        onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}
	    void			onSessionSleeped(const int32_t& display) override {}
	    void			onSessionParamsChanged(const int32_t& display) override {}
	    void			onDisplayRemoved(const int32_t& display) override {}

        protected:
            // dbus virtual signals
            void                        onLoginSuccess(const int32_t & display, const std::string & userName) override;
            void                        onDebugLevel(const std::string & level) override;
	    void			onPingConnector(const int32_t & display) override;
            void			onClearRenderPrimitives(const int32_t & display) override;
            void			onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill) override;
            void			onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color) override;

            bool                        xcbConnect(int display);

        public:
            SignalProxy(sdbus::IConnection*, const JsonObject &, const char* conntype);
        };

        /* Connector::Service */
        class Service : public ApplicationJsonConfig
        {
            std::string                 _type;

        public:
            Service(int argc, const char** argv);

            int    		        start(void);
        };
    }
}

#endif // _LTSM_CONNECTOR_
