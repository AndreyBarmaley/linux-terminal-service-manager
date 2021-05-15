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

#include <QThread>
#include <QTableWidget>
#include <QTableWidgetItem>

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

RowItem::RowItem(const XvfbInfo & info, const QString & label) : display(info.display), mode(info.mode), authfile(info.authfile)
{
    setText(label);
}

LTSM_Sessions::LTSM_Sessions(QWidget* parent) :
    QDialog(parent), ui(new Ui::LTSM_Sessions), selectedRow(nullptr)
{
    ui->setupUi(this);
    const char* service = "ltsm.manager.service";
    const char* path = "/ltsm/manager/service";
    const char* interface = "LTSM.Manager.Service";

    dbusInterfacePtr.reset(new QDBusInterface(service, path, interface, QDBusConnection::systemBus()));

    if(dbusInterfacePtr->isValid())
    {
	tableReload();
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
    ui->tableWidget->clearContents();
    auto res = dbusInterfacePtr->call(QDBus::CallMode::Block, "busGetSessions");

    if(!res.arguments().isEmpty())
    {
        auto args = res.arguments().front().value<QDBusArgument>();
        args.beginArray();

        while(! args.atEnd())
        {
    	    XvfbInfo info;
	    args >> info;

    	    int row = ui->tableWidget->rowCount();
    	    ui->tableWidget->insertRow(row);

    	    ui->tableWidget->setItem(row, 0, new RowItem(info, info.user));
    	    ui->tableWidget->setItem(row, 1, new RowItem(info, QString::number(info.display)));

	    switch(info.mode)
	    {
		case 1: ui->tableWidget->setItem(row, 2, new RowItem(info, "online")); break;
		case 2: ui->tableWidget->setItem(row, 2, new RowItem(info, "sleep")); break;
		default: ui->tableWidget->setItem(row, 2, new RowItem(info, "login")); break;
	    }

    	    ui->tableWidget->setItem(row, 3, new RowItem(info, info.remoteaddr));
    	    ui->tableWidget->setItem(row, 4, new RowItem(info, QString::number(info.pid1)));
    	    ui->tableWidget->setItem(row, 5, new RowItem(info, QString::number(info.uid)));
	}
    }
}

void LTSM_Sessions::disconnectClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
    {
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownConnector", selectedRow->display);
	QThread::msleep(300);
	tableReload();
    }
}

void LTSM_Sessions::logoffClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
    {
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busShutdownDisplay", selectedRow->display);
	QThread::msleep(300);
	tableReload();
    }
}

void LTSM_Sessions::sendmsgClicked(void)
{
    if(dbusInterfacePtr->isValid() && selectedRow)
    {
	QString message("Hello World!");
	dbusInterfacePtr->call(QDBus::CallMode::Block, "busSendMessage", selectedRow->display, message);
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
	    ui->pushButtonSendMsg->setEnabled(false);
	}
        else
	{
	    ui->pushButtonDisconnect->setEnabled(false);
	    ui->pushButtonLogoff->setEnabled(false);
	    ui->pushButtonSendMsg->setEnabled(false);
	}
    }
}
