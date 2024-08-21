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

#include <ctime>
#include <chrono>
#include <thread>
#include <filesystem>

#include <QFile>
#include <QDate>
#include <QTime>
#include <QObject>
#include <QScreen>
#include <QCursor>
#include <QDateTime>
#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>
#include <QSslCertificate>
#include <QRandomGenerator>

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_pkcs11.h"
#include "ltsm_application.h"
#include "ltsm_helperwindow.h"
#include "ltsm_pkcs11_session.h"
#include "ltsm_openssl_wrapper.h"

#include "ui_ltsm_helperwindow.h"

using namespace std::chrono_literals;
using namespace LTSM;

/// LTSM_HelperSDBus
LTSM_HelperSDBus::LTSM_HelperSDBus() : ProxyInterfaces(sdbus::createSystemBusConnection(), LTSM::dbus_manager_service_name, LTSM::dbus_manager_service_path)
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

void LTSM_HelperSDBus::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t& userUid)
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
void LTSM_HelperSDBus::sendAuthenticateLoginPass(int displayNum, const QString & user, const QString & pass)
{
    std::thread([this, display = displayNum, user = user.toStdString(), pass = pass.toStdString()]()
    {
        this->busSetAuthenticateLoginPass(display, user, pass);
    }).detach();
}

void LTSM_HelperSDBus::sendAuthenticateToken(int displayNum, const QString & user)
{
    std::thread([this, display = displayNum, user = user.toStdString()]()
    {
        this->busSetAuthenticateToken(display, user);
    }).detach();
}

void LTSM_HelperSDBus::widgetStartedAction(int displayNum)
{
    std::thread([this, display = displayNum]()
    {
        std::this_thread::sleep_for(300ms);
        this->helperWidgetStartedAction(display);
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
    ui(new Ui::LTSM_HelperWindow), dateFormat("dddd dd MMMM, hh:mm:ss"), displayNum(0), timerOneSec(0), timer300ms(0),
        timerReloadUsers(0), labelPause(0), loginAutoComplete(false), initArguments(false), tokenAuthMode(false)
{
    Application::setDebug(DebugTarget::Syslog, DebugLevel::Info);

    ui->setupUi(this);
    ui->labelDomain->hide();
    ui->comboBoxDomain->hide();
    ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
    ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
    ui->labelTitle->setText(tr("X11 Remote Desktop"));
    ui->comboBoxUsername->setFocus();
    setWindowFlags(Qt::FramelessWindowHint);
    setMouseTracking(true);

    connect(ui->comboBoxDomain, SIGNAL(currentIndexChanged(int)), this, SLOT(domainIndexChanged(int)), Qt::QueuedConnection);
    connect(ui->comboBoxUsername, SIGNAL(currentIndexChanged(int)), this, SLOT(usernameIndexChanged(int)), Qt::QueuedConnection);

    auto val = qgetenv("DISPLAY");

    if(val.size() && val[0] == ':')
        displayNum = val.remove(0, 1).toInt();

    timerOneSec = startTimer(std::chrono::seconds(1));
    timer300ms = startTimer(std::chrono::milliseconds(300));
    timerReloadUsers = startTimer(std::chrono::minutes(3));

    auto group = xkbGroup();
    auto names = xkbNames();

    if(0 <= group && group < names.size())
        ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));

#ifdef LTSM_PKCS11_AUTH
    ldap.reset(new LdapWrapper());
    //
    pkcs11.reset(new Pkcs11Client(displayNum, this));
    connect(pkcs11.get(), SIGNAL(pkcs11TokensChanged()), this, SLOT(tokensChanged()), Qt::QueuedConnection);
    pkcs11->start();
#endif
}

LTSM_HelperWindow::~LTSM_HelperWindow()
{
    delete ui;
}

void LTSM_HelperWindow::switchLoginMode(void)
{
    tokenAuthMode = false;

    ui->labelDomain->setText(tr("domain:"));
    ui->labelUsername->setText(tr("username:"));
    ui->labelPassword->setText(tr("password:"));

    ui->comboBoxUsername->lineEdit()->setReadOnly(false);
    ui->comboBoxDomain->lineEdit()->setReadOnly(false);

    ui->comboBoxUsername->setFocus();
    ui->comboBoxUsername->lineEdit()->clear();
    ui->lineEditPassword->clear();

    ui->labelDomain->hide();
    ui->comboBoxDomain->hide();
    ui->pushButtonLogin->setDisabled(false);

    reloadUsersList();
}

QString tokenTooltip(const Pkcs11Token & st)
{
    auto manufacturerId = QString(QByteArray((char*) st.tokenInfo.manufacturerID, sizeof(st.tokenInfo.manufacturerID)).trimmed());
    auto label = QString(QByteArray((char*) st.tokenInfo.label, sizeof(st.tokenInfo.label)).trimmed());
    auto hardware = QString("%1.%2").arg(st.tokenInfo.hardwareVersion.major).arg(st.tokenInfo.hardwareVersion.minor);
    auto firmware = QString("%1.%2").arg(st.tokenInfo.firmwareVersion.major).arg(st.tokenInfo.firmwareVersion.minor);

    return QString("manufacturer id: %1\nlabel: %2\nhardware version: %3\nfirmware version: %4").
                            arg(manufacturerId).arg(label).arg(hardware).arg(firmware);
}

QString sslTooltip(const QSslCertificate & ssl)
{
    auto serial = QString(QByteArray::fromHex(ssl.serialNumber()).toHex(':'));
    auto email = ssl.subjectInfo(QSslCertificate::EmailAddress).join("");
    auto org = ssl.subjectInfo(QSslCertificate::Organization).join("");

    return QString("serial number: %1\nemail address: %2\nexpired date: %3\norganization: %4\nissuer: %5").
                            arg(serial).arg(email).arg(ssl.expiryDate().toString()).arg(org).arg(ssl.issuerDisplayName());
}

void LTSM_HelperWindow::tokensChanged(void)
{
#ifdef LTSM_PKCS11_AUTH
    auto & tokens = pkcs11->getTokens();

    if(tokens.empty())
    {
        switchLoginMode();
        return;
    }

    tokenAuthMode = true;

    ui->labelDomain->setVisible(true);
    ui->comboBoxDomain->setVisible(true);

    ui->labelDomain->setText("token id:");
    ui->labelUsername->setText("certificate:");
    ui->labelPassword->setText("pin code:");

    ui->comboBoxDomain->clear();
    ui->comboBoxUsername->clear();
    ui->lineEditPassword->clear();

    int rowIndex = 0;
    for(auto & st: tokens)
    {
        auto model = QString(QByteArray((char*) st.tokenInfo.model, sizeof(st.tokenInfo.model)).trimmed());
        auto serialNumber = QString(QByteArray((char*) st.tokenInfo.serialNumber, sizeof(st.tokenInfo.serialNumber)).trimmed());

        ui->comboBoxDomain->addItem(QString("%1 (%2)").arg(model).arg(serialNumber), QByteArray((const char*) & st, sizeof(st)));
        ui->comboBoxDomain->setItemData(rowIndex, tokenTooltip(st), Qt::ToolTipRole);

        rowIndex++;
    }

    ui->comboBoxDomain->lineEdit()->setReadOnly(true);
    ui->comboBoxDomain->setCurrentIndex(0);
    ui->lineEditPassword->setFocus();
#endif
}

void LTSM_HelperWindow::domainIndexChanged(int index)
{
    if(index < 0)
        return;

    if(tokenAuthMode)
    {
        auto buf = ui->comboBoxDomain->itemData(index, Qt::UserRole).toByteArray();
        if(buf.isEmpty() || buf.size() != sizeof(Pkcs11Token))
        {
            Application::error("%s: %s failed, index: %d", __FUNCTION__, "item", index);
            return;
        }

        ui->comboBoxUsername->clear();
        ui->lineEditPassword->clear();

        auto tokenPtr = reinterpret_cast<const Pkcs11Token*>(buf.data());
        ui->comboBoxDomain->setToolTip(tokenTooltip(*tokenPtr));

        int rowIndex = 0;
        auto certs = pkcs11->getCertificates(tokenPtr->slotId);

        if(certs.empty())
        {
            setLabelError("token empty");
            ui->pushButtonLogin->setDisabled(true);
            return;
        }

        for(auto & cert: certs)
        {
            auto ssl = QSslCertificate(QByteArray((const char*) cert.objectValue.data(), cert.objectValue.size()), QSsl::Der);

            if(ssl.isNull())
                continue;

            ui->comboBoxUsername->addItem(ssl.subjectDisplayName(), QByteArray((const char*) & cert, sizeof(cert)));
            ui->comboBoxUsername->setItemData(rowIndex, sslTooltip(ssl), Qt::ToolTipRole);

            rowIndex++;
        }

        ui->comboBoxUsername->setCurrentIndex(0);
        ui->comboBoxUsername->lineEdit()->setReadOnly(true);
        ui->pushButtonLogin->setDisabled(false);
    }
}

void LTSM_HelperWindow::usernameIndexChanged(int index)
{
    if(index < 0)
        return;

    if(tokenAuthMode)
    {
        auto buf = ui->comboBoxUsername->itemData(index, Qt::UserRole).toByteArray();
        if(buf.isEmpty()  || buf.size() != sizeof(Pkcs11Cert))
        {
            Application::error("%s: %s failed, index: %d", __FUNCTION__, "item", index);
            return;
        }

        auto certPtr = reinterpret_cast<const Pkcs11Cert*>(buf.data());
        auto ssl = QSslCertificate(QByteArray((const char*) certPtr->objectValue.data(), certPtr->objectValue.size()), QSsl::Der);

        ui->comboBoxUsername->setToolTip(sslTooltip(ssl));
        ui->pushButtonLogin->setDisabled(false);

        if(ssl.expiryDate() < QDateTime::currentDateTime())
        {
            setLabelError("certificate expired");
            ui->pushButtonLogin->setDisabled(true);
        }
    }
    else
    {
        ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
    }
}

void LTSM_HelperWindow::loginClicked(void)
{
    ui->pushButtonLogin->setDisabled(true);
    ui->comboBoxUsername->setDisabled(true);
    ui->lineEditPassword->setDisabled(true);

    if(! tokenAuthMode)
    {
        sendAuthenticateLoginPass(displayNum, ui->comboBoxUsername->currentText(), ui->lineEditPassword->text());
        return;
    }

#ifdef LTSM_PKCS11_AUTH
    auto bufToken = ui->comboBoxDomain->currentData(Qt::UserRole).toByteArray();
    auto tokenPtr = reinterpret_cast<const Pkcs11Token*>(bufToken.data());

    auto bufCert = ui->comboBoxUsername->currentData(Qt::UserRole).toByteArray();
    auto certPtr = reinterpret_cast<const Pkcs11Cert*>(bufCert.data());

    auto ssl = QSslCertificate(QByteArray((const char*) certPtr->objectValue.data(), certPtr->objectValue.size()), QSsl::Der);
    auto pin = ui->lineEditPassword->text().toStdString();

    // generate 32byte hash
    std::vector<uint8_t> hash1(32);
    QRandomGenerator::global()->generate(hash1.begin(), hash1.end());

    auto cert = OpenSSL::CertificateDer(certPtr->objectValue.data(), certPtr->objectValue.size());
    auto crypt = cert.publicKey().encryptData(hash1.data(), hash1.size());

    setLabelInfo("check token...");
    std::vector<uint8_t> hash2 = pkcs11->decryptData(tokenPtr->slotId, pin, certPtr->objectId, crypt.data(), crypt.size(), CKM_RSA_PKCS);

    if(hash1 != hash2)
    {
        setLabelError("token failed");
        ui->pushButtonLogin->setDisabled(false);
        ui->comboBoxUsername->setDisabled(false);
        //ui->comboBoxUsername->setToolTip("");
        ui->lineEditPassword->setDisabled(false);
        ui->lineEditPassword->setFocus();
        return;
    }

    if(ldap)
    {
        auto der = ssl.toDer();
        auto dn = ldap->findDnFromCertificate((const uint8_t*) der.data(), der.size());

        if(! dn.empty())
        {
            setLabelInfo("LDAP: certificate found");

            auto login = ldap->findLoginFromDn(dn);
            if(! login.empty())
            {
                setLabelInfo("LDAP: login found");

                sendAuthenticateToken(displayNum, QString::fromStdString(login));
                return;
            }
            else
            {
                setLabelError("LDAP: login not found");
            }
        }
        else
        {
            setLabelError("LDAP: certificate not found");
        }
    }
    else
    {
        setLabelError("LDAP: initialize failed");
    }
#endif
}

void LTSM_HelperWindow::passwordChanged(const QString & pass)
{
    if(tokenAuthMode)
        ui->pushButtonLogin->setDisabled(pass.isEmpty());
    else
        ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::showEvent(QShowEvent*)
{
    widgetStartedAction(displayNum);

    auto screen = QGuiApplication::primaryScreen();
    auto pos = (screen->size() - size()) / 2;
    move(pos.width(), pos.height());

    if(! initArguments)
    {
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
            reloadUsersList();

        initArguments = true;
    }
}

void LTSM_HelperWindow::timerEvent(QTimerEvent* ev)
{
    if(ev->timerId() == timerOneSec)
    {
        if(0 < labelPause)
            labelPause--;
        else
        {
            ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
            ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
        }
    }
    else
    if(ev->timerId() == timer300ms)
        xcbEventProcessing();
    else
    if(ev->timerId() == timerReloadUsers && loginAutoComplete)
        reloadUsersList();
}

void LTSM_HelperWindow::mouseMoveEvent(QMouseEvent* ev)
{
    if((ev->buttons() & Qt::LeftButton) && titleBarPressed)
    {
        auto distance = ev->globalPos() - *titleBarPressed;
        *titleBarPressed = ev->globalPos();
        move(pos() + distance);
    }
}

void LTSM_HelperWindow::mousePressEvent(QMouseEvent* ev)
{
    if(ui->labelTitle->geometry().contains(ev->pos()))
        titleBarPressed.reset(new QPoint(ev->globalPos()));
}

void LTSM_HelperWindow::mouseReleaseEvent(QMouseEvent*)
{
    titleBarPressed.reset();
}

void LTSM_HelperWindow::keyPressEvent(QKeyEvent*)
{
}

void LTSM_HelperWindow::reloadUsersList(void)
{
    ui->comboBoxUsername->clear();

    ui->comboBoxUsername->addItems(getUsersList(displayNum));
    ui->comboBoxUsername->setEditText(prefferedLogin);
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
    labelPause = 2;
}

void LTSM_HelperWindow::setLabelInfo(const QString & info)
{
    ui->labelInfo->setText(info);
    ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
    labelPause = 2;
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
    {
        close();
    }
}

void LTSM_HelperWindow::setLoginPasswordCallback(int display, const QString & login, const QString & pass, const bool & autoLogin)
{
    if(display == displayNum && 0 < login.size())
    {
        prefferedLogin = login;
        ui->comboBoxUsername->setEditText(prefferedLogin);

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

/*
QDateTime fromStringTime(const std::string & str)
{
    // dont use QDateTime::fromString
    // date cert in C locale, example: "Sep 12 00:11:22 2022 GMT"
    // QDateTime::fromString expected localized format and return invalid
    struct tm tm;

    if(strptime(str.c_str(), "%b %d %H:%M:%S %Y", & tm))
        return QDateTime(QDate(1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday), QTime(tm.tm_hour, tm.tm_min, tm.tm_sec), Qt::UTC);

    return QDateTime();
}
*/
