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

#ifndef LTSM_HELPER_WINDOW_H
#define LTSM_HELPER_WINDOW_H

#include <QMainWindow>
#include <QTimerEvent>
#include <QMouseEvent>
#include <QKeyEvent>

#include "ltsm_service_proxy.h"
#include "ltsm_application.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_sockets.h"

#ifdef LTSM_PKCS11_AUTH
#include "ltsm_ldap_wrapper.h"
#endif

namespace Ui
{
    class LoginWindow;
}

class Pkcs11Client;
struct Pkcs11Token;

namespace LTSM::LoginHelper
{
    class LoginWindow : public QMainWindow, protected LTSM::XCB::RootDisplay
    {
        Q_OBJECT

    public:
        explicit LoginWindow( QWidget * parent = 0 );
        ~LoginWindow();

    protected slots:
        void loginClicked( void );
        void domainIndexChanged( int );
        // void                tokenChanged(const QString &);
        void usernameIndexChanged( int );
        // void                usernameChanged(const QString &);
        void passwordChanged( const QString & );
        void loginFailureCallback( int, const QString & );
        void loginSuccessCallback( int, const QString & );
        void setLoginPasswordCallback( int, const QString &, const QString &, const bool & );
        void widgetCenteredCallback( int );
        void widgetTimezoneCallback( int, const QString & );
        void sessionChangedCallback( int );
        void shutdownConnectorCallback( int );
        void pkcs11ListennerCallback( int, int );
        void setLabelError( const QString & );
        void setLabelInfo( const QString & );
        void reloadUsersList( void );

        void tokensChanged( void );

    protected:
        virtual void showEvent( QShowEvent * ) override;
        virtual void timerEvent( QTimerEvent * ) override;
        virtual void mouseMoveEvent( QMouseEvent * ) override;
        virtual void mousePressEvent( QMouseEvent * ) override;
        virtual void mouseReleaseEvent( QMouseEvent * ) override;
        virtual void keyPressEvent( QKeyEvent * ) override;

        virtual void sendAuthenticateLoginPass( int displayNum, const QString & user, const QString & pass ) = 0;
        virtual void sendAuthenticateToken( int displayNum, const QString & user ) = 0;
        virtual QString getEncryptionInfo( int displayNum ) = 0;
        virtual int getServiceVersion( void ) = 0;
        virtual void widgetStartedAction( int displayNum ) = 0;
        virtual bool isAutoComplete( int displayNum ) = 0;
        virtual QString getTitle( int displayNum ) = 0;
        virtual QString getDateFormat( int displayNum ) = 0;
        virtual QStringList getUsersList( int displayNum ) = 0;

        // xkb client interface
        void xcbXkbGroupChangedEvent( int group ) override;

        void switchLoginMode( void );

    private:
        Ui::LoginWindow * ui;
        QString dateFormat;
        QString prefferedLogin;

        std::string tokenCheck, tokenSerial, tokenCert;
        int displayNum;
        int timerOneSec;
        int timer300ms;
        int timerReloadUsers;
        int labelPause;
        bool loginAutoComplete;
        bool initArguments;
        bool tokenAuthMode;
        QScopedPointer<QPoint> titleBarPressed;

#ifdef LTSM_PKCS11_AUTH
        QScopedPointer<LTSM::LdapWrapper> ldap;
        QScopedPointer<Pkcs11Client> pkcs11;
#endif
    };

    class ManagerServiceProxy : public LoginWindow, public LTSM::Application, public sdbus::ProxyInterfaces<LTSM::Manager::Service_proxy>
    {
    private:
        // dbus virtual signals
        void onSessionReconnect( const std::string & removeAddr, const std::string & connType ) override {}

        void onDisplayRemoved( const int32_t & display ) override {}

        void onCreateChannel( const int32_t & display, const std::string &, const std::string &,
                              const std::string &, const std::string &, const std::string & ) override {}

        void onDestroyChannel( const int32_t & display, const uint8_t & channel ) override {}

        void onCreateListener( const int32_t & display, const std::string &, const std::string &,
                               const std::string &, const std::string &, const std::string &, const uint8_t &, const uint32_t & ) override {}

        void onDestroyListener( const int32_t & display, const std::string &, const std::string & ) override {}

        void onTransferAllow( const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                              const std::string & dstdir ) override {}

        void onDebugLevel( const int32_t & display, const std::string & level ) override {}

        void onDebugChannel( const int32_t & display, const uint8_t & channel, const bool & debug ) override {}

        void onPingConnector( const int32_t & display ) override {}

        void onClearRenderPrimitives( const int32_t & display ) override {}

        void onAddRenderRect( const int32_t & display,
                              const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect,
                              const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill ) override {}

        void onAddRenderText( const int32_t & display, const std::string & text,
                              const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color ) override {}

        void onHelperWidgetStarted( const int32_t & display ) override {}

        void onSendBellSignal( const int32_t & display ) override {}

        int start(void) override { return 0; }

    protected:
        // dbus virtual signals
        void onLoginFailure( const int32_t & display, const std::string & msg ) override;
        void onLoginSuccess( const int32_t & display, const std::string & userName,
                             const uint32_t & userUid ) override;
        void onHelperSetLoginPassword( const int32_t & display, const std::string & login,
                                       const std::string & pass, const bool & autologin ) override;
        void onHelperWidgetCentered( const int32_t & display ) override;
        void onHelperWidgetTimezone( const int32_t & display, const std::string & ) override;
        void onHelperPkcs11ListennerStarted( const int32_t & display, const int32_t & connectorId ) override;
        void onSessionChanged( const int32_t & display ) override;
        void onShutdownConnector( const int32_t & display ) override;

        // helper window interface
        void sendAuthenticateLoginPass( int displayNum, const QString & user, const QString & pass ) override;
        void sendAuthenticateToken( int displayNum, const QString & user ) override;
        QString getEncryptionInfo( int displayNum ) override;
        int getServiceVersion( void ) override;
        bool isAutoComplete( int displayNum ) override;
        void widgetStartedAction( int displayNum ) override;
        QString getTitle( int displayNum ) override;
        QString getDateFormat( int displayNum ) override;
        QStringList getUsersList( int displayNum ) override;

    public:
        ManagerServiceProxy();
        ~ManagerServiceProxy();
    };
}

#endif
