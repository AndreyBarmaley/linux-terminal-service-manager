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
#include <QThread>
#include <QProcess>
#include <QFileInfo>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QCoreApplication>

#include "ltsm_sessions.h"
#include "ui_ltsm_sessions.h"

const QDBusArgument & operator>>(const QDBusArgument & arg, XvfbInfo & st)
{
    arg.beginStructure();
    arg >> st.display >> st.pid1 >> st.pid2 >> st.width >> st.height >>
	st.uid >> st.gid >> st.durationLimit >> st.mode >> st.policy >>
	st.user >> st.authfile >> st.remoteaddr >> st.conntype;
    arg.endStructure();
    return arg;
}

RowItem::RowItem(const XvfbInfo & info, const QString & label)
    : QTableWidgetItem(label), display(info.display), mode(info.mode), authfile(info.authfile)
{
}

RowItem::RowItem(const XvfbInfo & info, const QIcon & icon, const QString & label)
    : QTableWidgetItem(icon, label), display(info.display), mode(info.mode), authfile(info.authfile)
{
}

LTSM_Sessions::LTSM_Sessions(QWidget* parent) :
    QDialog(parent), ui(new Ui::LTSM_Sessions), selectedRow(nullptr)
{
    ui->setupUi(this);
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
    if(dbusInterfacePtr->isValid())
    {
	tableReload();

        connect(dbusInterfacePtr.data(), SIGNAL(displayRemoved(int)), this, SLOT(displayRemovedCallback(int)));
        connect(dbusInterfacePtr.data(), SIGNAL(sessionSleeped(int)), this, SLOT(sessionSleepedCallback(int)));
    }
    else
    {
        qFatal("dbus interface not found: [%s, %s, %s]", service, path, interface);
        dbusInterfacePtr.reset();
    }
}

LTSM_Sessions::~LTSM_Sessions()
{
    delete ui;
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

void LTSM_Sessions::disconnectClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownConnector", selectedRow->display);
}

void LTSM_Sessions::logoffClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownDisplay", selectedRow->display);
}

void LTSM_Sessions::sendmsgClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
    {
	QString message("Hello World!");
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busSendMessage", selectedRow->display, message);
    }
}

void LTSM_Sessions::showClicked(void)
{
    if(selectedRow)
    {
	QStringList args;
	args << "--auth" << selectedRow->authfile << "--display" << QString::number(selectedRow->display);
	process.start(sdl2x11.absoluteFilePath(), args, QIODevice::NotOpen);
	ui->pushButtonShow->setEnabled(false);
    }
}

void LTSM_Sessions::itemClicked(QTableWidgetItem* item)
{
    selectedRow = dynamic_cast<RowItem*>(item);
    if(selectedRow)
    {
	auto envDisplay = qgetenv("DISPLAY");
	auto myDisplay = envDisplay.size() && envDisplay[0] == ':' ? envDisplay.remove(0, 1).toInt() : 0;

	if(myDisplay != selectedRow->display)
	{
	    ui->pushButtonDisconnect->setEnabled(selectedRow->mode != 2);
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
