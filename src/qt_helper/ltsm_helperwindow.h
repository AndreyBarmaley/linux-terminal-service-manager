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

#ifndef LTSM_HELPERWINDOW_H
#define LTSM_HELPERWINDOW_H

#include <QMainWindow>
#include <QTimerEvent>
#include <QMouseEvent>
#include <QKeyEvent>

#include "ltsm_dbus_proxy.h"
#include "ltsm_xcb_wrapper.h"


namespace Ui
{
    class LTSM_HelperWindow;
}

class LTSM_HelperWindow : public QMainWindow, public LTSM::XCB::XkbClient
{
    Q_OBJECT

public:
    explicit LTSM_HelperWindow(QWidget* parent = 0);
    ~LTSM_HelperWindow();

protected slots:
    void                loginClicked(void);
    void                usernameChanged(const QString &);
    void                passwordChanged(const QString &);
    void                loginFailureCallback(int, const QString &);
    void                loginSuccessCallback(int, const QString &);
    void                setLoginPasswordCallback(int, const QString &, const QString &, const bool &);
    void                widgetCenteredCallback(int);
    void                widgetTimezoneCallback(int, const QString &);
    void        	sessionChangedCallback(int);
    void        	shutdownConnectorCallback(int);
    void                setLabelError(const QString &);
    void                reloadUsersList(void);

protected:
    virtual void        showEvent(QShowEvent*) override;
    virtual void        timerEvent(QTimerEvent*) override;
    virtual void        mouseMoveEvent(QMouseEvent*) override;
    virtual void        mousePressEvent(QMouseEvent*) override;
    virtual void        mouseReleaseEvent(QMouseEvent*) override;
    virtual void        keyPressEvent(QKeyEvent*) override;

    virtual void        sendAuthenticateInfo(int displayNum, const QString & user, const QString & pass) = 0;
    virtual QString     getEncryptionInfo(int displayNum) = 0;
    virtual int         getServiceVersion(void) = 0;
    virtual void        widgetStartedAction(int displayNum) = 0;
    virtual void        idleTimeoutAction(int displayNum) = 0;
    virtual bool        isAutoComplete(int displayNum) = 0;
    virtual int         getIdleTimeoutSec(int displayNum) = 0;
    virtual QString     getTitle(int displayNum) = 0;
    virtual QString     getDateFormat(int displayNum) = 0;
    virtual QStringList getUsersList(int displayNum) = 0;

    void                setIdleTimeoutSec(int);

    // xkb client interface
    void                xkbStateChangeEvent(int group) override;

private:
    Ui::LTSM_HelperWindow* ui;
    int                 displayNum;
    int                 idleTimeoutSec;
    int                 currentIdleSec;
    int                 timerOneSec;
    int                 timer300ms;
    int                 timerReloadUsers;
    int                 errorPause;
    bool                loginAutoComplete;
    bool                initArguments;
    QString             dateFormat;
    QScopedPointer<QPoint> titleBarPressed;
};

class LTSM_HelperSDBus : public LTSM_HelperWindow, public sdbus::ProxyInterfaces<LTSM::Manager::Service_proxy>
{
private:
    // dbus virtual signals
    void                onSessionReconnect(const std::string & removeAddr, const std::string & connType) override {}
    void                onDisplayRemoved(const int32_t& display) override {}
    void                onCreateChannel(const int32_t & display, const std::string&, const std::string&, const std::string&, const std::string&) override {}
    void                onDestroyChannel(const int32_t& display, const uint8_t& channel) override {}
    void                onCreateListener(const int32_t& display, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&) override {}
    void                onDestroyListener(const int32_t& display, const std::string&, const std::string&) override {}
    void                onTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile,  const std::string& dstdir) override {}
    void                onDebugLevel(const std::string & level) override {}
    void                onPingConnector(const int32_t & display) override {}
    void                onClearRenderPrimitives(const int32_t & display) override {}
    void                onAddRenderRect(const int32_t & display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t> & rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color, const bool & fill) override {}
    void                onAddRenderText(const int32_t & display, const std::string & text, const sdbus::Struct<int16_t, int16_t> & pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t> & color) override {}
    void                onHelperWidgetStarted(const int32_t & display) override {}
    void                onSendBellSignal(const int32_t& display) override {}

protected:
    // dbus virtual signals
    void                onLoginFailure(const int32_t & display, const std::string & msg) override;
    void                onLoginSuccess(const int32_t & display, const std::string & userName) override;
    void                onHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass, const bool& autologin) override;
    void                onHelperWidgetCentered(const int32_t& display) override;
    void                onHelperWidgetTimezone(const int32_t& display, const std::string&) override;
    void                onSessionChanged(const int32_t& display) override;
    void                onShutdownConnector(const int32_t & display) override;

    // helper window interface
    void                sendAuthenticateInfo(int displayNum, const QString & user, const QString & pass) override;
    QString             getEncryptionInfo(int displayNum) override;
    int                 getServiceVersion(void) override;
    bool                isAutoComplete(int displayNum) override;
    void                widgetStartedAction(int displayNum) override;
    void                idleTimeoutAction(int displayNum) override;
    int                 getIdleTimeoutSec(int displayNum) override;
    QString             getTitle(int displayNum) override;
    QString             getDateFormat(int displayNum) override;
    QStringList         getUsersList(int displayNum) override;

public:
    LTSM_HelperSDBus();
    ~LTSM_HelperSDBus();
};

#endif // LTSM_HELPERWINDOW_H
