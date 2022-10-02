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

#include <QFile>
#include <QScreen>
#include <QCursor>
#include <QDateTime>
#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>

#include "ltsm_helperwindow.h"
#include "ui_ltsm_helperwindow.h"

LTSM_HelperWindow::LTSM_HelperWindow(QWidget* parent) :
    QMainWindow(parent),
    ui(new Ui::LTSM_HelperWindow), displayNum(0), idleTimeoutSec(0), currentIdleSec(0),
    timerOneSec(0), timerReloadUsers(0), errorPause(0), loginAutoComplete(false), initArguments(false), dateFormat("dddd dd MMMM, hh:mm:ss")
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
    const char* service = "ltsm.manager.service";
    const char* path = "/ltsm/manager/service";
    const char* interface = "LTSM.Manager.Service";

    dbusInterfacePtr.reset(new QDBusInterface(service, path, interface, QDBusConnection::systemBus()));

    if(! dbusInterfacePtr->isValid())
    {
        syslog(LOG_ERR, "dbus interface not found: [%s, %s, %s]", service, path, interface);
        dbusInterfacePtr.reset();
        setLabelError(tr("dbus init failed"));
        idleTimeoutSec = 5;
    }
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

    if(dbusInterfacePtr)
        auto res = dbusInterfacePtr->call(QDBus::CallMode::NoBlock, "busCheckAuthenticate", displayNum, ui->comboBoxUsername->currentText(), ui->lineEditPassword->text());
}

void LTSM_HelperWindow::usernameChanged(const QString &)
{
    ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::passwordChanged(const QString &)
{
    ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::showEvent(QShowEvent*)
{
    if(dbusInterfacePtr)
        dbusInterfacePtr->call(QDBus::CallMode::NoBlock, "helperWidgetStartedAction", displayNum);

    auto screen = QGuiApplication::primaryScreen();
    auto pos = (screen->size() - size()) / 2;
    move(pos.width(), pos.height());

    if(dbusInterfacePtr && ! initArguments)
    {
        auto res = dbusInterfacePtr->call(QDBus::CallMode::Block, "helperGetIdleTimeoutSec", displayNum);

        if(!res.arguments().isEmpty())
            idleTimeoutSec = res.arguments().front().toInt();

        res = dbusInterfacePtr->call(QDBus::CallMode::Block, "helperGetTitle", displayNum);

        if(!res.arguments().isEmpty())
        {
            auto title = res.arguments().front().toString();
            res = dbusInterfacePtr->call(QDBus::CallMode::Block, "busGetServiceVersion");

            if(! res.arguments().isEmpty())
                title.replace("%{version}", QString::number(res.arguments().front().toInt()));

            ui->labelTitle->setText(title);
        }

        res = dbusInterfacePtr->call(QDBus::CallMode::Block, "helperGetDateFormat", displayNum);

        if(!res.arguments().isEmpty())
            dateFormat = res.arguments().front().toString();

        res = dbusInterfacePtr->call(QDBus::CallMode::Block, "helperIsAutoComplete", displayNum);

        if(!res.arguments().isEmpty())
            loginAutoComplete = res.arguments().front().toBool();

        res = dbusInterfacePtr->call(QDBus::CallMode::Block, "busEncryptionInfo", displayNum);
	QString encryption;

        if(!res.arguments().isEmpty())
            encryption = res.arguments().front().toString();

	if(encryption.isEmpty())
            encryption = "none";

        ui->lineEditEncryption->setText(encryption);

        if(loginAutoComplete)
        {
            timerReloadUsers = startTimer(std::chrono::minutes(15));
            reloadUsersList();
        }

        connect(dbusInterfacePtr.data(), SIGNAL(loginFailure(int, const QString &)), this, SLOT(loginFailureCallback(int, const QString &)));
        connect(dbusInterfacePtr.data(), SIGNAL(loginSuccess(int, const QString &)), this, SLOT(loginSuccessCallback(int, const QString &)));
        connect(dbusInterfacePtr.data(), SIGNAL(helperWidgetCentered(int)), this, SLOT(widgetCenteredCallback(int)));
        connect(dbusInterfacePtr.data(), SIGNAL(helperWidgetTimezone(int, const QString &)), this, SLOT(widgetTimezoneCallback(int, const QString &)));
        connect(dbusInterfacePtr.data(), SIGNAL(helperSetLoginPassword(int, const QString &, const QString &, const bool &)), this, SLOT(setLoginPasswordCallback(int, const QString &, const QString &, const bool &)));
	connect(dbusInterfacePtr.data(), SIGNAL(sessionChanged(int)), this, SLOT(sessionChangedCallback(int)));

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
            if(dbusInterfacePtr)
                dbusInterfacePtr->call(QDBus::CallMode::NoBlock, "helperIdleTimeoutAction", displayNum);

            close();
        }
    }
    else if(ev->timerId() == timerReloadUsers)
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

    if(dbusInterfacePtr)
    {
        auto res = dbusInterfacePtr->call(QDBus::CallMode::Block, "helperGetUsersList", displayNum);

        if(!res.arguments().isEmpty())
        {
            auto users = res.arguments().front().toStringList();
            ui->comboBoxUsername->addItems(users);
            ui->comboBoxUsername->setEditText("");
        }
    }
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

void LTSM_HelperWindow::sessionChangedCallback(int display)
{
    if(display == displayNum && dbusInterfacePtr)
    {
        auto res = dbusInterfacePtr->call(QDBus::CallMode::Block, "busEncryptionInfo", displayNum);
	QString encryption;

        if(!res.arguments().isEmpty())
            encryption = res.arguments().front().toString();

	if(encryption.isEmpty())
            encryption = "none";

        ui->lineEditEncryption->setText(encryption);
    }
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
