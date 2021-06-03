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
#include <QtDBus/QtDBus>
#include <QScopedPointer>
#include <QTimerEvent>
#include <QMouseEvent>
#include <QKeyEvent>

namespace Ui
{
    class LTSM_HelperWindow;
}

class LTSM_HelperWindow : public QMainWindow
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
    void                setLoginPasswordCallback(int, const QString &, const QString &);
    void                autoLoginCallback(int, const QString &, const QString &);
    void                setLabelError(const QString &);
    void                reloadUsersList(void);

protected:
    virtual void        showEvent(QShowEvent*) override;
    virtual void        timerEvent(QTimerEvent*) override;
    virtual void        mouseMoveEvent(QMouseEvent*) override;
    virtual void        mousePressEvent(QMouseEvent*) override;
    virtual void        mouseReleaseEvent(QMouseEvent*) override;
    virtual void        keyPressEvent(QKeyEvent*) override;

private:
    Ui::LTSM_HelperWindow* ui;
    int                 displayNum;
    int                 idleTimeoutSec;
    int                 currentIdleSec;
    int                 timerOneSec;
    int                 timerReloadUsers;
    int                 errorPause;
    bool                loginAutoComplete;
    bool                initArguments;
    QString             dateFormat;
    QScopedPointer<QPoint> titleBarPressed;
    QScopedPointer<QDBusInterface> dbusInterfacePtr;
};

#endif // LTSM_HELPERWINDOW_H
