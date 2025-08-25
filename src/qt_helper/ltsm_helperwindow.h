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
    using TuplePosition = sdbus::Struct<int16_t, int16_t>;
    using TupleRegion = sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>;
    using TupleColor = sdbus::Struct<uint8_t, uint8_t, uint8_t>;

    class DBusProxy : public QObject, public sdbus::ProxyInterfaces<LTSM::Manager::Service_proxy>
    {
        Q_OBJECT

    private:
        int displayNum;

        // dbus virtual signals
        void onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}

        void onDisplayRemoved(const int32_t & display) override {}

        void onCreateChannel(const int32_t & display, const std::string &, const std::string &,
                             const std::string &, const std::string &, const std::string &) override {}

        void onDestroyChannel(const int32_t & display, const uint8_t & channel) override {}

        void onCreateListener(const int32_t & display, const std::string &, const std::string &,
                              const std::string &, const std::string &, const std::string &, const uint8_t &, const uint32_t &) override {}

        void onDestroyListener(const int32_t & display, const std::string &, const std::string &) override {}

        void onTransferAllow(const int32_t & display, const std::string & filepath, const std::string & tmpfile,
                             const std::string & dstdir) override {}

        void onDebugLevel(const int32_t & display, const std::string & level) override {}

        void onDebugChannel(const int32_t & display, const uint8_t & channel, const bool & debug) override {}

        void onPingConnector(const int32_t & display) override {}

        void onClearRenderPrimitives(const int32_t & display) override {}

        void onAddRenderRect(const int32_t & display, const TupleRegion & rect,
                             const TupleColor & color, const bool & fill) override {}

        void onAddRenderText(const int32_t & display, const std::string & text,
                             const TuplePosition & pos, const TupleColor & color) override {}


        void onSendBellSignal(const int32_t & display) override {}

    protected:
        // dbus virtual signals
        void onLoginFailure(const int32_t & display, const std::string & msg) override;
        void onLoginSuccess(const int32_t & display, const std::string & userName,
                            const uint32_t & userUid) override;
        void onHelperSetLoginPassword(const int32_t & display, const std::string & login,
                                      const std::string & pass, const bool & autologin) override;
        void onHelperSetTimezone(const int32_t & display, const std::string &) override;
        void onHelperWidgetStarted(const int32_t & display) override;
        void onHelperPkcs11ListennerStarted(const int32_t & display, const int32_t & connectorId) override;
        void onShutdownConnector(const int32_t & display) override;

    public:
        DBusProxy(int display);
        ~DBusProxy();

    signals:
        void loginFailureNotify(const QString &);
        void loginSuccessNotify(const QString &);
        void loginPasswordChangedNotify(const QString, const QString &, bool);
        void pkcs11ListennerStartedNotify(int);
        void connectorShutdownNotify(void);
        void widgetStartedNotify(void);

    public slots:
        void sendAuthenticateLoginPass(const QString & user, const QString & pass);
        void sendAuthenticateToken(const QString & user);
        void widgetStartedAction(void);
    };

    class LoginWindow : public QMainWindow, public LTSM::Application, protected LTSM::XCB::RootDisplay
    {
        Q_OBJECT

    public:
        explicit LoginWindow(QWidget * parent = 0);
        ~LoginWindow();

        int start(void) override { return 0; }

    protected slots:
        void loginClicked(void);
        void domainIndexChanged(int);
        void usernameIndexChanged(int);

        void passwordChanged(const QString &);
        void loginFailureCallback(const QString &);
        void loginSuccessCallback(const QString &);
        void setLoginPasswordCallback(const QString &, const QString &, bool);
        void pkcs11ListennerCallback(int);
        void shutdownConnectorCallback(void);
        void widgetStartedCallback(void);
        void setLabelError(const QString &);
        void setLabelInfo(const QString &);
        void reloadUsersList(void);

        void tokensChanged(void);

    protected:
        virtual void showEvent(QShowEvent*) override;
        virtual void timerEvent(QTimerEvent*) override;
        virtual void mouseMoveEvent(QMouseEvent*) override;
        virtual void mousePressEvent(QMouseEvent*) override;
        virtual void mouseReleaseEvent(QMouseEvent*) override;
        virtual void keyPressEvent(QKeyEvent*) override;

        // xkb client interface
        void xcbXkbGroupChangedEvent(int group) override;

        void switchLoginMode(void);

    private:
        Ui::LoginWindow* ui;
        QString dateFormat;
        QString prefferedLogin;
        QSize screenSize;

        std::string tokenCheck, tokenSerial, tokenCert;
        int displayNum;
        int timerOneSec;
        int timer200ms;
        int timerReloadUsers;
        int labelPause;
        bool loginAutoComplete;
        bool initArguments;
        bool tokenAuthMode;

        QScopedPointer<DBusProxy> dbus;
        QScopedPointer<QPoint> titleBarPressed;

#ifdef LTSM_PKCS11_AUTH
        QScopedPointer<LTSM::LdapWrapper> ldap;
        QScopedPointer<Pkcs11Client> pkcs11;
#endif
    };
}

#endif
