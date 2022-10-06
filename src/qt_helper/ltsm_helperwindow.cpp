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

#include <stdlib.h>
#include <syslog.h>

#include <thread>

#include <QFile>
#include <QScreen>
#include <QCursor>
#include <QDateTime>
#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>

#include "ltsm_global.h"
#include "ltsm_helperwindow.h"
#include "ui_ltsm_helperwindow.h"

/// LTSM_HelperSDBus
LTSM_HelperSDBus::LTSM_HelperSDBus() : ProxyInterfaces(sdbus::createSystemBusConnection(), LTSM::dbus_service_name, LTSM::dbus_object_path)
{
    registerProxy();
}

LTSM_HelperSDBus::~LTSM_HelperSDBus()
{
    unregisterProxy();
}

void LTSM_HelperSDBus::onLoginFailure(const int32_t & display, const std::string & msg)
{
    loginFailureCallback(display, QString::fromStdString(msg));
}

void LTSM_HelperSDBus::onLoginSuccess(const int32_t & display, const std::string & userName)
{
    loginSuccessCallback(display, QString::fromStdString(userName));
}

void LTSM_HelperSDBus::onHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass, const bool& autologin)
{
    setLoginPasswordCallback(display, QString::fromStdString(login), QString::fromStdString(pass), autologin);
}

void LTSM_HelperSDBus::onHelperWidgetCentered(const int32_t& display)
{
    widgetCenteredCallback(display);
}

void LTSM_HelperSDBus::onHelperWidgetTimezone(const int32_t& display, const std::string& tz)
{
    widgetTimezoneCallback(display, QString::fromStdString(tz));
}

void LTSM_HelperSDBus::onSessionChanged(const int32_t& display)
{
    sessionChangedCallback(display);
}

void LTSM_HelperSDBus::onShutdownConnector(const int32_t & display)
{
    shutdownConnectorCallback(display);
}

///
void LTSM_HelperSDBus::sendAuthenticateInfo(int displayNum, const QString & user, const QString & pass)
{
    std::thread([this, display = displayNum, user = user.toStdString(), pass = pass.toStdString()]()
    {
        this->busSetAuthenticateInfo(display, user, pass);
    }).detach();
}

void LTSM_HelperSDBus::widgetStartedAction(int displayNum)
{
    std::thread([this, display = displayNum]()
    {
        this->helperWidgetStartedAction(display);
    }).detach();
}

void LTSM_HelperSDBus::idleTimeoutAction(int displayNum)
{
    std::thread([this, display = displayNum]()
    {
        this->helperIdleTimeoutAction(display);
    }).detach();
}

QString LTSM_HelperSDBus::getEncryptionInfo(int displayNum)
{
    return QString::fromStdString(busEncryptionInfo(displayNum));
}

int LTSM_HelperSDBus::getServiceVersion(void)
{
    return busGetServiceVersion();
}

bool LTSM_HelperSDBus::isAutoComplete(int displayNum)
{
    return helperIsAutoComplete(displayNum);
}

int LTSM_HelperSDBus::getIdleTimeoutSec(int displayNum)
{
    return helperGetIdleTimeoutSec(displayNum);
}

QString LTSM_HelperSDBus::getTitle(int displayNum)
{
    return QString::fromStdString(helperGetTitle(displayNum));
}

QString LTSM_HelperSDBus::getDateFormat(int displayNum)
{
    return QString::fromStdString(helperGetDateFormat(displayNum));
}

QStringList LTSM_HelperSDBus::getUsersList(int displayNum)
{
    QStringList res;
    for(auto & user : helperGetUsersList(displayNum))
        res << QString::fromStdString(user);
    return res;
}

/// LTSM_HelperWindow
LTSM_HelperWindow::LTSM_HelperWindow(QWidget* parent) :
    QMainWindow(parent),
    ui(new Ui::LTSM_HelperWindow), displayNum(0), idleTimeoutSec(0), currentIdleSec(0),
    timerOneSec(0), timer300ms(0), timerReloadUsers(0), errorPause(0), loginAutoComplete(false), initArguments(false), dateFormat("dddd dd MMMM, hh:mm:ss")
{
    ui->setupUi(this);
    ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
    ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
    ui->labelTitle->setText(tr("X11 Remote Desktop"));
    ui->comboBoxUsername->setFocus();
    setWindowFlags(Qt::FramelessWindowHint);
    setMouseTracking(true);
    openlog("ltsm_helper", LOG_USER, 0);
    auto val = qgetenv("DISPLAY");

    if(val.size() && val[0] == ':')
        displayNum = val.remove(0, 1).toInt();

    timerOneSec = startTimer(std::chrono::seconds(1));
    timer300ms = startTimer(std::chrono::milliseconds(300));

    auto group = xkbGroup();
    auto names = xkbNames();

    if(0 <= group && group < names.size())
        ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));
}

LTSM_HelperWindow::~LTSM_HelperWindow()
{
    closelog();
    delete ui;
}

void LTSM_HelperWindow::loginClicked(void)
{
    ui->pushButtonLogin->setDisabled(true);
    ui->comboBoxUsername->setDisabled(true);
    ui->lineEditPassword->setDisabled(true);

    sendAuthenticateInfo(displayNum, ui->comboBoxUsername->currentText(), ui->lineEditPassword->text());
}

void LTSM_HelperWindow::usernameChanged(const QString &)
{
    ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::passwordChanged(const QString &)
{
    ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::setIdleTimeoutSec(int val)
{
    idleTimeoutSec = val;
}

void LTSM_HelperWindow::showEvent(QShowEvent*)
{
    widgetStartedAction(displayNum);

    auto screen = QGuiApplication::primaryScreen();
    auto pos = (screen->size() - size()) / 2;
    move(pos.width(), pos.height());

    if(! initArguments)
    {
        idleTimeoutSec = getIdleTimeoutSec(displayNum);

        auto title = getTitle(displayNum);

        if(! title.isEmpty())
        {
            auto version = getServiceVersion();
            title.replace("%{version}", QString::number(version));

            ui->labelTitle->setText(title);
        }

        dateFormat = getDateFormat(displayNum);
        loginAutoComplete = isAutoComplete(displayNum);

        auto encryption = getEncryptionInfo(displayNum);
        ui->lineEditEncryption->setText(encryption);

        if(loginAutoComplete)
        {
            timerReloadUsers = startTimer(std::chrono::minutes(15));
            reloadUsersList();
        }

        initArguments = true;
    }
}

void LTSM_HelperWindow::timerEvent(QTimerEvent* ev)
{
    if(ev->timerId() == timerOneSec)
    {
        currentIdleSec += 1;

        if(0 < errorPause)
            errorPause--;
        else
        {
            ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
            ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
        }

        if(currentIdleSec > idleTimeoutSec)
        {
            idleTimeoutAction(displayNum);
            close();
        }
    }
    else
    if(ev->timerId() == timer300ms)
        xcbEventProcessing();
    else
    if(ev->timerId() == timerReloadUsers)
        reloadUsersList();
}

void LTSM_HelperWindow::mouseMoveEvent(QMouseEvent* ev)
{
    currentIdleSec = 0;

    if((ev->buttons() & Qt::LeftButton) && titleBarPressed)
    {
        auto distance = ev->globalPos() - *titleBarPressed;
        *titleBarPressed = ev->globalPos();
        move(pos() + distance);
    }
}

void LTSM_HelperWindow::mousePressEvent(QMouseEvent* ev)
{
    currentIdleSec = 0;

    if(ui->labelTitle->geometry().contains(ev->pos()))
        titleBarPressed.reset(new QPoint(ev->globalPos()));
}

void LTSM_HelperWindow::mouseReleaseEvent(QMouseEvent*)
{
    titleBarPressed.reset();
}

void LTSM_HelperWindow::keyPressEvent(QKeyEvent*)
{
    currentIdleSec = 0;
}

void LTSM_HelperWindow::reloadUsersList(void)
{
    ui->comboBoxUsername->clear();

    ui->comboBoxUsername->addItems(getUsersList(displayNum));
    ui->comboBoxUsername->setEditText("");
}

void LTSM_HelperWindow::widgetTimezoneCallback(int display, const QString & tz)
{
    if(display == displayNum)
    {
        setenv("TZ", tz.toStdString().c_str(), 1);
    }
}

void LTSM_HelperWindow::widgetCenteredCallback(int display)
{
    if(display == displayNum)
    {
	if(auto primary = QGuiApplication::primaryScreen())
	{
	    auto screenGeometry = primary->geometry();
    	    int nx = (screenGeometry.width() - width()) / 2;
    	    int ny = (screenGeometry.height() - height()) / 2;
    	    move(nx, ny);
	}
    }
}

void LTSM_HelperWindow::loginFailureCallback(int display, const QString & error)
{
    if(display == displayNum)
    {
        ui->pushButtonLogin->setDisabled(false);
        ui->comboBoxUsername->setDisabled(false);
        ui->lineEditPassword->setDisabled(false);
        ui->lineEditPassword->selectAll();
        ui->lineEditPassword->setFocus();
        setLabelError(error);
    }
}

void LTSM_HelperWindow::setLabelError(const QString & error)
{
    ui->labelInfo->setText(error);
    ui->labelInfo->setStyleSheet("QLabel { color: red; }");
    errorPause = 2;
}

void LTSM_HelperWindow::shutdownConnectorCallback(int display)
{
    if(display == displayNum)
        close();
}

void LTSM_HelperWindow::sessionChangedCallback(int display)
{
    if(display == displayNum)
        ui->lineEditEncryption->setText(getEncryptionInfo(displayNum));
}

void LTSM_HelperWindow::loginSuccessCallback(int display, const QString & username)
{
    if(display == displayNum)
        close();
}

void LTSM_HelperWindow::setLoginPasswordCallback(int display, const QString & login, const QString & pass, const bool & autoLogin)
{
    if(display == displayNum && 0 < login.size())
    {
        ui->comboBoxUsername->setEditText(login);

        ui->lineEditPassword->setFocus();
        if(0 < pass.size())
            ui->lineEditPassword->setText(pass);

        if(autoLogin)
            loginClicked();
    }
}

void LTSM_HelperWindow::xkbStateChangeEvent(int group)
{
    auto names = xkbNames();

    if(0 <= group && group < names.size())
        ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));
}
