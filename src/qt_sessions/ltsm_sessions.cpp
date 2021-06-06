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

#include <QDir>
#include <QMenu>
#include <QThread>
#include <QProcess>
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>
#include <QTextStream>
#include <QTableWidget>
#include <QInputDialog>
#include <QTableWidgetItem>
#include <QCoreApplication>

#include "ltsm_sessions.h"
#include "ui_ltsm_sessions.h"

const QDBusArgument & operator>>(const QDBusArgument & arg, XvfbInfo & st)
{
    arg.beginStructure();
    arg >> st.display >> st.pid1 >> st.pid2 >> st.width >> st.height >>
	st.uid >> st.gid >> st.durationLimit >> st.mode >> st.policy >>
	st.user >> st.authfile >> st.remoteaddr >> st.conntype >> st.encription;
    arg.endStructure();
    return arg;
}

RowItem::RowItem(const XvfbInfo & info, const QString & label)
    : QTableWidgetItem(label)
{
    QVariant var;
    var.setValue(info);

    setData(Qt::UserRole, var);
}

RowItem::RowItem(const XvfbInfo & info, const QIcon & icon, const QString & label)
    : QTableWidgetItem(icon, label)
{
    QVariant var;
    var.setValue(info);

    setData(Qt::UserRole, var);
}

XvfbInfo RowItem::xvfbInfo(void) const
{
    auto var = data(Qt::UserRole);
    return qvariant_cast<XvfbInfo>(var);
}

int RowItem::display(void) const
{
    return xvfbInfo().display;
}

LTSM_Sessions::LTSM_Sessions(QWidget* parent) :
    QDialog(parent), ui(new Ui::LTSM_Sessions), selectedRow(nullptr)
{
    ui->setupUi(this);

    ui->tableWidget->setColumnCount(6);
    ui->tableWidget->setHorizontalHeaderLabels(QStringList() << "User" << "Display" << "Status" << "RemoteAddr" << "Pid" << "Uid");
    ui->tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    sdl2x11.setFile(QDir(QCoreApplication::applicationDirPath()).filePath("LTSM_sdl2x11"));

    if(! sdl2x11.exists())
    {
	ui->pushButtonShow->setEnabled(false);
	ui->pushButtonShow->setToolTip(QString("utility not found: %1").arg(sdl2x11.fileName()));
    }

    const char* service = "ltsm.manager.service";
    const char* path = "/ltsm/manager/service";
    const char* interface = "LTSM.Manager.Service";

    dbusInterfacePtr.reset(new QDBusInterface(service, path, interface, QDBusConnection::systemBus()));
    if(! dbusInterfacePtr->isValid())
    {
        dbusInterfacePtr.reset();

	QMessageBox::critical(this, "LTSM_sessions",
                QString("<b>DBus interface not found!</b><br><br>service: %1<br>path: %2<br>interface: %3").arg(service).arg(path).arg(interface),
        	QMessageBox::Ok);

	throw -1;
    }

    tableReload();

    connect(dbusInterfacePtr.data(), SIGNAL(displayRemoved(int)), this, SLOT(displayRemovedCallback(int)));
    connect(dbusInterfacePtr.data(), SIGNAL(sessionSleeped(int)), this, SLOT(sessionSleepedCallback(int)));
    connect(dbusInterfacePtr.data(), SIGNAL(sessionParamsChanged(int)), this, SLOT(sessionParamsChangedCallback(int)));
    connect(ui->tableWidget, SIGNAL(itemSelectionChanged()), this, SLOT(itemSelectionChanged()));
    connect(ui->tableWidget, SIGNAL(itemDoubleClicked(QTableWidgetItem*)), this, SLOT(itemDoubleClicked(QTableWidgetItem*)));
    connect(ui->tableWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(customContextMenu(QPoint)));
    connect(ui->pushButtonShow, SIGNAL(clicked()), this, SLOT(showClicked()));
    connect(ui->pushButtonSendMsg, SIGNAL(clicked()), this, SLOT(sendmsgClicked()));
    connect(ui->pushButtonLogoff, SIGNAL(clicked()), this, SLOT(logoffClicked()));
    connect(ui->pushButtonDisconnect, SIGNAL(clicked()), this, SLOT(disconnectClicked()));
}

LTSM_Sessions::~LTSM_Sessions()
{
    delete ui;
}

void LTSM_Sessions::customContextMenu(QPoint pos)
{
    if(selectedRow)
    {
	auto envDisplay = qgetenv("DISPLAY");
	auto myDisplay = envDisplay.size() && envDisplay[0] == ':' ? envDisplay.remove(0, 1).toInt() : 0;

	QMenu * menu = new QMenu(this);

	QAction* infoAction = new QAction("information");
	QAction* showAction = new QAction("show");
	QAction* disconnectAction = new QAction("disconnect");
	QAction* logoutAction = new QAction("logout");
	QAction* sendmsgAction = new QAction("send message");
	QAction* setSessionDurationAction = new QAction("set session duration");
	QAction* setSessionPolicyAction = new QAction("set session policy");

	menu->addAction(infoAction);
	menu->addSeparator();
	menu->addAction(showAction);
	menu->addAction(disconnectAction);
	menu->addAction(logoutAction);
	menu->addAction(sendmsgAction);
	menu->addSeparator();
	menu->addAction(setSessionDurationAction);
	menu->addAction(setSessionPolicyAction);

	if(myDisplay == selectedRow->display())
	{
	    showAction->setDisabled(true);
	    disconnectAction->setDisabled(true);
	    logoutAction->setDisabled(true);
	    sendmsgAction->setDisabled(true);
	}

	connect(infoAction, SIGNAL(triggered()), this, SLOT(showInformation()));
	connect(showAction, SIGNAL(triggered()), this, SLOT(showClicked()));
	connect(disconnectAction, SIGNAL(triggered()), this, SLOT(disconnectClicked()));
	connect(logoutAction, SIGNAL(triggered()), this, SLOT(logoffClicked()));
	connect(sendmsgAction, SIGNAL(triggered()), this, SLOT(sendmsgClicked()));
	connect(setSessionDurationAction, SIGNAL(triggered()), this, SLOT(changeSessionDuration()));
	connect(setSessionPolicyAction, SIGNAL(triggered()), this, SLOT(changeSessionPolicy()));

	menu->setDefaultAction(infoAction);
	menu->popup(ui->tableWidget->viewport()->mapToGlobal(pos));
    }
}

void LTSM_Sessions::showInformation(void)
{
    if(selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
	QString content;
	QTextStream ts(& content);

	QString status = "login";
	switch(xvfb.mode)
	{
	    case 1: status = "online"; break;
	    case 2: status = "sleep"; break;
	    default: break;
	}

	QString policy = "authlock";
	switch(xvfb.policy)
	{
	    case 1: policy = "authtake"; break;
	    case 2: policy = "authshare"; break;
	    default: break;
	}

	ts <<
	    "display: " << xvfb.display << "<br>" <<
	    "user: " << xvfb.user << "<br>" <<
	    "address: " << xvfb.remoteaddr << "<br>" <<
	    "pid1: " << xvfb.pid1 << "<br>" <<
	    "pid2: " << xvfb.pid2 << "<br>" <<
	    "width: " << xvfb.width <<"<br>" <<
	    "height: " << xvfb.height <<"<br>" <<
	    "uid: " << xvfb.uid << "<br>" <<
	    "gid: " << xvfb.gid << "<br>" <<
	    "status: " << status << "<br>" <<
	    "session duration: " << xvfb.durationLimit << "<br>" <<
	    "session policy: " << policy << "<br>" <<
	    "connection: " << xvfb.conntype << "<br>" <<
	    "encription: " << xvfb.encription << "<br>";

	QMessageBox::information(this, "Session Info", ts.readAll(), QMessageBox::Ok);
    }
}

void LTSM_Sessions::changeSessionDuration(void)
{
    if(selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
        bool change = false;
        int duration = QInputDialog::getInt(this, QString("Change session duration for: %1").arg(xvfb.user), "seconds:", xvfb.durationLimit, 0, 2147483647, 1, & change);
        if(change) dbusInterfacePtr->call(QDBus::CallMode::Block, "busSetSessionDurationSec", xvfb.display, duration);
    }
}

void LTSM_Sessions::changeSessionPolicy(void)
{
    if(selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
        bool change = false;
        QString policy = QInputDialog::getItem(this, QString("Change session policy for: %1").arg(xvfb.user), "", QStringList() << "authlock" << "authtake" << "authshare", xvfb.policy, false, & change);
        if(change) dbusInterfacePtr->call(QDBus::CallMode::Block, "busSetSessionPolicy", xvfb.display, policy);
    }
}

void LTSM_Sessions::itemDoubleClicked(QTableWidgetItem* item)
{
    showInformation();
}

void LTSM_Sessions::tableReload(void)
{
    selectedRow = nullptr;

    int row = ui->tableWidget->rowCount();
    while(0 < row--)
	ui->tableWidget->removeRow(row);

    ui->pushButtonDisconnect->setEnabled(false);
    ui->pushButtonLogoff->setEnabled(false);
    ui->pushButtonSendMsg->setEnabled(false);
    ui->pushButtonShow->setEnabled(false);

    auto res = dbusInterfacePtr->call(QDBus::CallMode::Block, "busGetSessions");

    if(! res.arguments().isEmpty())
    {
        auto args = res.arguments().front().value<QDBusArgument>();
        args.beginArray();

        while(! args.atEnd())
        {
    	    XvfbInfo info;
	    args >> info;

    	    row = ui->tableWidget->rowCount();
    	    ui->tableWidget->insertRow(row);

	    if(0 < info.mode)
	    {
    	        ui->tableWidget->setItem(row, 0, new RowItem(info,
                        QIcon(1 == info.mode ? ":/ltsm/ltsm_online.png" : ":/ltsm/ltsm_offline.png"), info.user));
    	        ui->tableWidget->setItem(row, 1, new RowItem(info, QString::number(info.display)));
		ui->tableWidget->setItem(row, 2, new RowItem(info, (1 == info.mode ? "online" : "sleep")));
    	        ui->tableWidget->setItem(row, 3, new RowItem(info, info.remoteaddr));
    	        ui->tableWidget->setItem(row, 4, new RowItem(info, QString::number(info.pid1)));
    	        ui->tableWidget->setItem(row, 5, new RowItem(info, QString::number(info.uid)));
            }
	}
    }
}

void LTSM_Sessions::displayRemovedCallback(int display)
{
    tableReload();
}

void LTSM_Sessions::sessionSleepedCallback(int display)
{
    tableReload();
}

void LTSM_Sessions::sessionParamsChangedCallback(int display)
{
    tableReload();
}

void LTSM_Sessions::disconnectClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownConnector", selectedRow->display());
}

void LTSM_Sessions::logoffClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownDisplay", selectedRow->display());
}

void LTSM_Sessions::sendmsgClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
        bool send = false;
        auto message = QInputDialog::getMultiLineText(this, QString("Send message to: %1").arg(xvfb.user), "", QString(), & send);
	if(send) dbusInterfacePtr->call(QDBus::CallMode::Block, "busSendMessage", xvfb.display, message);
    }
}

void LTSM_Sessions::showClicked(void)
{
    if(selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
	QStringList args;
	args << "--auth" << xvfb.authfile << "--display" << QString::number(xvfb.display);
	process.start(sdl2x11.absoluteFilePath(), args, QIODevice::NotOpen);
	ui->pushButtonShow->setEnabled(false);
    }
}

void LTSM_Sessions::itemSelectionChanged(void)
{
    if(ui->tableWidget->selectedItems().isEmpty())
    {
	selectedRow = nullptr;

	ui->pushButtonDisconnect->setEnabled(false);
	ui->pushButtonLogoff->setEnabled(false);
	ui->pushButtonSendMsg->setEnabled(false);
	ui->pushButtonShow->setEnabled(false);
    }
    else
    {
	selectedRow = dynamic_cast<RowItem*>(ui->tableWidget->currentItem());
    }

    if(selectedRow)
    {
	auto xvfb = selectedRow->xvfbInfo();
	auto envDisplay = qgetenv("DISPLAY");
	auto myDisplay = envDisplay.size() && envDisplay[0] == ':' ? envDisplay.remove(0, 1).toInt() : 0;

	if(myDisplay != xvfb.display)
	{
	    ui->pushButtonDisconnect->setEnabled(xvfb.mode != 2);
	    ui->pushButtonLogoff->setEnabled(true);
	    ui->pushButtonSendMsg->setEnabled(true);
	    ui->pushButtonShow->setEnabled(sdl2x11.exists() && QProcess::NotRunning == process.state());
	}
        else
	{
	    ui->pushButtonDisconnect->setEnabled(false);
	    ui->pushButtonLogoff->setEnabled(false);
	    ui->pushButtonSendMsg->setEnabled(false);
	    ui->pushButtonShow->setEnabled(false);
	}
    }
}
