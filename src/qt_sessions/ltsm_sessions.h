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

#ifndef LTSM_SESSIONS_H
#define LTSM_SESSIONS_H

#include <QIcon>
#include <QDialog>
#include <QProcess>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QtDBus/QtDBus>
#include <QScopedPointer>
#include <QTableWidgetItem>

struct XvfbInfo
{
    qint32      display;
    qint32      pid1;
    qint32      pid2;
    qint32      width;
    qint32      height;
    qint32      uid;
    qint32      gid;
    qint32      durationLimit;
    qint32      mode;
    qint32      policy;
    QString     user;
    QString     authfile;
    QString     remoteaddr;
    QString     conntype;
    QString     encryption;
};

Q_DECLARE_METATYPE(XvfbInfo);

struct RowItem : QTableWidgetItem
{
    RowItem(const XvfbInfo &, const QString &);
    RowItem(const XvfbInfo &, const QIcon &, const QString &);

    XvfbInfo    xvfbInfo(void) const;
    int		display(void) const;
};

namespace Ui
{
    class LTSM_Sessions;
}

class LTSM_Sessions : public QDialog
{
    Q_OBJECT

protected slots:
    void	tableReload(void);
    void	disconnectClicked(void);
    void	logoffClicked(void);
    void	showClicked(void);
    void	showInformation(void);
    void	sendmsgClicked(void);
    void	itemSelectionChanged(void);
    void        itemDoubleClicked(QTableWidgetItem*);
    void        displayRemovedCallback(int);
    void        sessionSleepedCallback(int);
    void        sessionParamsChangedCallback(int);
    void	customContextMenu(QPoint);
    void	changeSessionPolicy(void);
    void	changeSessionDuration(void);

public:
    explicit LTSM_Sessions(QWidget* parent = 0);
    ~LTSM_Sessions();

private:
    Ui::LTSM_Sessions*             ui;
    QScopedPointer<QDBusInterface> dbusInterfacePtr;
    const RowItem*                 selectedRow;
    QFileInfo	                   sdl2x11;
    QProcess	                   process;
};

#endif // LTSM_SESSIONS_H
