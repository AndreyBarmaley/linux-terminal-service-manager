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

#ifdef LTSM_PKCS11_AUTH
#include "gnutls/x509.h"
#include "gnutls/abstract.h"
#endif

#include "ltsm_tools.h"
#include "ltsm_global.h"
#include "ltsm_pkcs11.h"
#include "ltsm_sockets.h"
#include "ltsm_application.h"
#include "ltsm_helperwindow.h"
#include "ltsm_pkcs11_session.h"

#include "ui_ltsm_helperwindow.h"

using namespace std::chrono_literals;

namespace LTSM::LoginHelper
{
    /// ManagerServiceProxy
    ManagerServiceProxy::ManagerServiceProxy() : LTSM::Application("ltsm_helper"),
#ifdef SDBUS_2_0_API
        ProxyInterfaces(sdbus::createSystemBusConnection(), sdbus::ServiceName {LTSM::dbus_manager_service_name}, sdbus::ObjectPath {LTSM::dbus_manager_service_path})
#else
        ProxyInterfaces(sdbus::createSystemBusConnection(), LTSM::dbus_manager_service_name, LTSM::dbus_manager_service_path)
#endif
    {
        Application::setDebug(DebugTarget::Syslog, DebugLevel::Info);
        registerProxy();

        Application::info("%s: started, display: `%s'", "LoginWindow", getenv("DISPLAY"));
    }

    ManagerServiceProxy::~ManagerServiceProxy()
    {
        unregisterProxy();
    }

    void ManagerServiceProxy::onLoginFailure(const int32_t & display, const std::string & msg)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", message: `%s'",
                           __FUNCTION__, display, msg.c_str());
        loginFailureCallback(display, QString::fromStdString(msg));
    }

    void ManagerServiceProxy::onLoginSuccess(const int32_t & display, const std::string & userName, const uint32_t & userUid)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", username: `%s', uid: %" PRIu32,
                           __FUNCTION__, display, userName.c_str(), userUid);

        loginSuccessCallback(display, QString::fromStdString(userName));
    }

    void ManagerServiceProxy::onHelperSetLoginPassword(const int32_t & display, const std::string & login,
            const std::string & pass, const bool & autologin)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", login: `%s', pass length: %" PRIu32 ", auto login: %d",
                           __FUNCTION__, display, login.c_str(), pass.size(), static_cast<int>(autologin));

        setLoginPasswordCallback(display, QString::fromStdString(login), QString::fromStdString(pass), autologin);
    }

    void ManagerServiceProxy::onHelperPkcs11ListennerStarted(const int32_t & display, const int32_t & connectorId)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", connectorId: 0x%08" PRIx32,
                           __FUNCTION__, display, connectorId);

        pkcs11ListennerCallback(display, connectorId);
    }

    void ManagerServiceProxy::onHelperWidgetCentered(const int32_t & display)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32,
                           __FUNCTION__, display);

        widgetCenteredCallback(display);
    }

    void ManagerServiceProxy::onHelperWidgetTimezone(const int32_t & display, const std::string & tz)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", tz: `%s'",
                           __FUNCTION__, display, tz.c_str());

        widgetTimezoneCallback(display, QString::fromStdString(tz));
    }

    void ManagerServiceProxy::onSessionChanged(const int32_t & display)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32,
                           __FUNCTION__, display);

        sessionChangedCallback(display);
    }

    void ManagerServiceProxy::onShutdownConnector(const int32_t & display)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32,
                           __FUNCTION__, display);

        shutdownConnectorCallback(display);
    }

    ///
    void ManagerServiceProxy::sendAuthenticateLoginPass(int displayNum, const QString & user, const QString & pass)
    {
        auto login = user.toStdString();

        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", user: `%s', pass length: %" PRIu32,
                           __FUNCTION__, displayNum, login.c_str(), pass.size());

        std::thread([this, display = displayNum, user = std::move(login), pass = pass.toStdString()]()
        {
            this->busSetAuthenticateLoginPass(display, user, pass);
        }).detach();
    }

    void ManagerServiceProxy::sendAuthenticateToken(int displayNum, const QString & user)
    {
        auto login = user.toStdString();

        Application::debug(DebugType::Helper, "%s: display: %" PRId32 ", user: `%s'",
                           __FUNCTION__, displayNum, login.c_str());

        std::thread([this, display = displayNum, user = std::move(login)]()
        {
            this->busSetAuthenticateToken(display, user);
        }).detach();
    }

    void ManagerServiceProxy::widgetStartedAction(int displayNum)
    {
        Application::debug(DebugType::Helper, "%s: display: %" PRId32,
                           __FUNCTION__, displayNum);

        std::thread([this, display = displayNum]()
        {
            std::this_thread::sleep_for(300ms);
            this->helperWidgetStartedAction(display);
        }).detach();
    }

    QString ManagerServiceProxy::getEncryptionInfo(int displayNum)
    {
        return QString::fromStdString(busEncryptionInfo(displayNum));
    }

    int ManagerServiceProxy::getServiceVersion(void)
    {
        return busGetServiceVersion();
    }

    bool ManagerServiceProxy::isAutoComplete(int displayNum)
    {
        return helperIsAutoComplete(displayNum);
    }

    QString ManagerServiceProxy::getTitle(int displayNum)
    {
        return QString::fromStdString(helperGetTitle(displayNum));
    }

    QString ManagerServiceProxy::getDateFormat(int displayNum)
    {
        return QString::fromStdString(helperGetDateFormat(displayNum));
    }

    QStringList ManagerServiceProxy::getUsersList(int displayNum)
    {
        QStringList res;

        for(const auto & user : helperGetUsersList(displayNum))
        {
            res << QString::fromStdString(user);
        }

        return res;
    }

    /// LoginWindow
    LoginWindow::LoginWindow(QWidget* parent) :
        QMainWindow(parent),
        ui(new Ui::LoginWindow), dateFormat("dddd dd MMMM, hh:mm:ss"), displayNum(0), timerOneSec(0), timer300ms(0),
        timerReloadUsers(0), labelPause(0), loginAutoComplete(false), initArguments(false), tokenAuthMode(false)
    {
        if(! RootDisplay::displayConnect(-1, XCB::InitModules::Xkb, nullptr))
        {
            Application::error("%s: xcb connect failed", __FUNCTION__);
            throw xcb_error(NS_FuncName);
        }

        ui->setupUi(this);
        ui->labelDomain->hide();
        ui->comboBoxDomain->hide();
        ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
        ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
        ui->labelTitle->setText(tr("X11 Remote Desktop"));
        ui->comboBoxUsername->setFocus();
        setWindowFlags(Qt::FramelessWindowHint);
        setMouseTracking(true);
        connect(ui->comboBoxDomain, SIGNAL(currentIndexChanged(int)), this, SLOT(domainIndexChanged(int)),
                Qt::QueuedConnection);
        connect(ui->comboBoxUsername, SIGNAL(currentIndexChanged(int)), this, SLOT(usernameIndexChanged(int)),
                Qt::QueuedConnection);
        auto val = qgetenv("DISPLAY");

        if(val.size() && val[0] == ':')
        {
            displayNum = val.remove(0, 1).toInt();
        }

        timerOneSec = startTimer(std::chrono::seconds(1));
        timer300ms = startTimer(std::chrono::milliseconds(300));
        timerReloadUsers = startTimer(std::chrono::minutes(3));

        if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)))
        {
            auto names = extXkb->getNames();
            int group = extXkb->getLayoutGroup();

            if(0 <= group && group < names.size())
            {
                ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));
            }
        }
    }

    LoginWindow::~LoginWindow()
    {
        delete ui;
    }

    void LoginWindow::switchLoginMode(void)
    {
        Application::debug(DebugType::Helper, "%s: set login mode", __FUNCTION__);

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
        auto manufacturerId = QString(QByteArray((char*) st.tokenInfo.manufacturerID,
                                      sizeof(st.tokenInfo.manufacturerID)).trimmed());
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
#if QT_VERSION < QT_VERSION_CHECK(5, 12, 0)
        auto issuer = ssl.issuerInfo(QSslCertificate::CommonName).join(":");
#else
        auto issuer = ssl.issuerDisplayName();
#endif
        return QString("serial number: %1\nemail address: %2\nexpired date: %3\norganization: %4\nissuer: %5").
               arg(serial).arg(email).arg(ssl.expiryDate().toString()).arg(org).arg(issuer);
    }

    void LoginWindow::tokensChanged(void)
    {
#ifdef LTSM_PKCS11_AUTH
        auto & tokens = pkcs11->getTokens();

        Application::debug(DebugType::Helper, "%s: tokens count: %lu", __FUNCTION__, tokens.size());

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

        for(const auto & st : tokens)
        {
            auto model = QString(QByteArray((char*) st.tokenInfo.model, sizeof(st.tokenInfo.model)).trimmed());
            auto serialNumber = QString(QByteArray((char*) st.tokenInfo.serialNumber, sizeof(st.tokenInfo.serialNumber)).trimmed());
            ui->comboBoxDomain->addItem(QString("%1 (%2)").arg(model).arg(serialNumber), QByteArray((const char*) & st,
                                        sizeof(st)));
            ui->comboBoxDomain->setItemData(rowIndex, tokenTooltip(st), Qt::ToolTipRole);
            rowIndex++;
        }

        ui->comboBoxDomain->lineEdit()->setReadOnly(true);
        ui->comboBoxDomain->setCurrentIndex(0);
        ui->lineEditPassword->setFocus();
#endif
    }

    void LoginWindow::domainIndexChanged(int index)
    {
        if(index < 0)
        {
            return;
        }

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
            ui->comboBoxDomain->setToolTip(tokenTooltip(* tokenPtr));
            int rowIndex = 0;
            auto certs = pkcs11->getCertificates(tokenPtr->slotId);

            if(certs.empty())
            {
                setLabelError("token empty");
                ui->pushButtonLogin->setDisabled(true);
                return;
            }

            for(const auto & cert : certs)
            {
                auto ssl = QSslCertificate(QByteArray((const char*) cert.objectValue.data(), cert.objectValue.size()), QSsl::Der);

                if(ssl.isNull())
                {
                    continue;
                }

#if QT_VERSION < QT_VERSION_CHECK(5, 12, 0)
                auto subject = ssl.subjectInfo(QSslCertificate::CommonName).join(":");
#else
                auto subject = ssl.subjectDisplayName();
#endif
                ui->comboBoxUsername->addItem(subject, QByteArray((const char*) & cert, sizeof(cert)));
                ui->comboBoxUsername->setItemData(rowIndex, sslTooltip(ssl), Qt::ToolTipRole);
                rowIndex++;
            }

            ui->comboBoxUsername->setCurrentIndex(0);
            ui->comboBoxUsername->lineEdit()->setReadOnly(true);
            ui->pushButtonLogin->setDisabled(false);
        }
    }

    void LoginWindow::usernameIndexChanged(int index)
    {
        if(index < 0)
        {
            return;
        }

        if(tokenAuthMode)
        {
            auto buf = ui->comboBoxUsername->itemData(index, Qt::UserRole).toByteArray();

            if(buf.isEmpty() || buf.size() != sizeof(Pkcs11Cert))
            {
                Application::error("%s: %s failed, index: %d", __FUNCTION__, "item", index);
                return;
            }

            auto certPtr = reinterpret_cast<const Pkcs11Cert*>(buf.data());
            auto ssl = QSslCertificate(QByteArray((const char*) certPtr->objectValue.data(), certPtr->objectValue.size()),
                                       QSsl::Der);
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
            ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() ||
                                             ui->lineEditPassword->text().isEmpty());
        }
    }

#ifdef LTSM_PKCS11_AUTH
    struct DatumAlloc : gnutls_datum_t
    {
        DatumAlloc(const gnutls_datum_t & dt)
        {
            data = dt.data;
            size = dt.size;
        }

        ~DatumAlloc()
        {
            if(data && size)
            {
                gnutls_free(data);
            }
        }
    };

    std::unique_ptr<DatumAlloc> gnutlsEncryptData(const std::vector<uint8_t> & certder, const std::vector<uint8_t> & vals)
    {
        gnutls_x509_crt_t ptr1 = nullptr;

        if(int err = gnutls_x509_crt_init(& ptr1); err != GNUTLS_E_SUCCESS)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "gnutls_x509_crt_init", gnutls_strerror(err),
                               err);
            throw LTSM::gnutls_error(NS_FuncName);
        }

        const gnutls_datum_t dt1 = { .data = (unsigned char*) certder.data(), .size = (unsigned int) certder.size() };
        const gnutls_datum_t dt2 = { .data = (unsigned char*) vals.data(), .size = (unsigned int) vals.size() };
        std::unique_ptr<gnutls_x509_crt_int, void(*)(gnutls_x509_crt_t)> cert = { ptr1, gnutls_x509_crt_deinit };

        if(int err = gnutls_x509_crt_import(cert.get(), & dt1, GNUTLS_X509_FMT_DER); err != GNUTLS_E_SUCCESS)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "gnutls_x509_crt_import", gnutls_strerror(err),
                               err);
            throw gnutls_error(NS_FuncName);
        }

        gnutls_pubkey_t ptr2 = nullptr;

        if(int err = gnutls_pubkey_init(& ptr2); GNUTLS_E_SUCCESS != err)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "gnutls_pubkey_init", gnutls_strerror(err), err);
            throw gnutls_error(NS_FuncName);
        }

        std::unique_ptr<gnutls_pubkey_st, void(*)(gnutls_pubkey_t)> pkey = { ptr2, gnutls_pubkey_deinit };

        if(int err = gnutls_pubkey_import_x509(pkey.get(), cert.get(), 0); GNUTLS_E_SUCCESS != err)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "gnutls_pubkey_import_x509",
                               gnutls_strerror(err), err);
            throw gnutls_error(NS_FuncName);
        }

        gnutls_datum_t res;

        if(int err = gnutls_pubkey_encrypt_data(pkey.get(), 0, & dt2, & res); GNUTLS_E_SUCCESS != err)
        {
            Application::error("%s: %s failed, error: %s, code: %d", __FUNCTION__, "gnutls_pubkey_encrypt_data",
                               gnutls_strerror(err), err);
            throw gnutls_error(NS_FuncName);
        }

        return std::make_unique<DatumAlloc>(res);
    }

#endif

    void LoginWindow::loginClicked(void)
    {
        Application::error("%s: tokenAuthMode: %d", __FUNCTION__, (int) tokenAuthMode);

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
        auto ssl = QSslCertificate(QByteArray((const char*) certPtr->objectValue.data(), certPtr->objectValue.size()),
                                   QSsl::Der);
        auto pin = ui->lineEditPassword->text().toStdString();
        // generate 32byte hash
        std::vector<uint8_t> hash1 = Tools::randomBytes(32);
        setLabelInfo("check token...");
        bool certValidate = false;

        try
        {
            auto dt = gnutlsEncryptData(certPtr->objectValue, hash1);
            std::vector<uint8_t> hash2 = pkcs11->decryptData(tokenPtr->slotId, pin, certPtr->objectId, dt->data, dt->size,
                                         CKM_RSA_PKCS);
            certValidate = (hash1 == hash2);
        }
        catch(const std::exception & err)
        {
        }

        if(! certValidate)
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

    void LoginWindow::passwordChanged(const QString & pass)
    {
        if(tokenAuthMode)
        {
            ui->pushButtonLogin->setDisabled(pass.isEmpty());
        }
        else
        {
            ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() ||
                                             ui->lineEditPassword->text().isEmpty());
        }
    }

    void LoginWindow::showEvent(QShowEvent*)
    {
        widgetStartedAction(displayNum);
        auto screen = QGuiApplication::primaryScreen();
        auto pos = (screen->size() - QMainWindow::size()) / 2;
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
            {
                reloadUsersList();
            }

            initArguments = true;
        }
    }

    void LoginWindow::timerEvent(QTimerEvent* ev)
    {
        if(ev->timerId() == timerOneSec)
        {
            if(0 < labelPause)
            {
                labelPause--;
            }
            else
            {
                ui->labelInfo->setText(QDateTime::currentDateTime().toString(dateFormat));
                ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
            }
        }
        else if(ev->timerId() == timer300ms)
        {
            if(auto err = XCB::RootDisplay::hasError())
            {
                Application::error("%s: x11 has error: %d", __FUNCTION__, err);
                return;
            }

            if(auto ev = XCB::RootDisplay::pollEvent())
            {
                if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)))
                {
                    uint16_t opcode = 0;

                    if(extXkb->isEventError(ev, & opcode))
                    {
                        Application::warning("%s: %s error: 0x%04" PRIx16, __FUNCTION__, "xkb", opcode);
                    }
                }
            }
        }
        else if(ev->timerId() == timerReloadUsers && loginAutoComplete)
        {
            reloadUsersList();
        }
    }

    void LoginWindow::mouseMoveEvent(QMouseEvent* ev)
    {
        if((ev->buttons() & Qt::LeftButton) && titleBarPressed)
        {
            auto distance = ev->globalPos() - *titleBarPressed;
            * titleBarPressed = ev->globalPos();
            move(pos() + distance);
        }
    }

    void LoginWindow::mousePressEvent(QMouseEvent* ev)
    {
        if(ui->labelTitle->geometry().contains(ev->pos()))
        {
            titleBarPressed.reset(new QPoint(ev->globalPos()));
        }
    }

    void LoginWindow::mouseReleaseEvent(QMouseEvent*)
    {
        titleBarPressed.reset();
    }

    void LoginWindow::keyPressEvent(QKeyEvent*)
    {
    }

    void LoginWindow::reloadUsersList(void)
    {
        ui->comboBoxUsername->clear();
        ui->comboBoxUsername->addItems(getUsersList(displayNum));
        ui->comboBoxUsername->setEditText(prefferedLogin);
    }

    void LoginWindow::pkcs11ListennerCallback(int display, int connectorId)
    {
        if(display == displayNum)
        {
#ifdef LTSM_PKCS11_AUTH

            if(! pkcs11)
            {
                try
                {
                    ldap.reset(new LdapWrapper());
                }
                catch(const std::exception &)
                {
                }

                pkcs11.reset(new Pkcs11Client(displayNum, this));
                connect(pkcs11.data(), SIGNAL(pkcs11TokensChanged()), this, SLOT(tokensChanged()), Qt::QueuedConnection);
                pkcs11->start();
            }

#endif
        }
    }

    void LoginWindow::widgetTimezoneCallback(int display, const QString & tz)
    {
        if(display == displayNum)
        {
            setenv("TZ", tz.toStdString().c_str(), 1);
        }
    }

    void LoginWindow::widgetCenteredCallback(int display)
    {
        if(display == displayNum)
        {
            if(auto primary = QGuiApplication::primaryScreen())
            {
                auto screenGeometry = primary->geometry();
                int nx = (screenGeometry.width() - QMainWindow::width()) / 2;
                int ny = (screenGeometry.height() - QMainWindow::height()) / 2;
                move(nx, ny);
            }
        }
    }

    void LoginWindow::loginFailureCallback(int display, const QString & error)
    {
        if(display == displayNum)
        {
            Application::error("%s: login failure", __FUNCTION__);
            ui->pushButtonLogin->setDisabled(false);
            ui->comboBoxUsername->setDisabled(false);
            ui->lineEditPassword->setDisabled(false);
            ui->lineEditPassword->selectAll();
            ui->lineEditPassword->setFocus();
            setLabelError(error);
        }
    }

    void LoginWindow::setLabelError(const QString & error)
    {
        ui->labelInfo->setText(error);
        ui->labelInfo->setStyleSheet("QLabel { color: red; }");
        labelPause = 2;
    }

    void LoginWindow::setLabelInfo(const QString & info)
    {
        ui->labelInfo->setText(info);
        ui->labelInfo->setStyleSheet("QLabel { color: blue; }");
        labelPause = 2;
    }

    void LoginWindow::shutdownConnectorCallback(int display)
    {
        if(display == displayNum)
        {
            close();
        }
    }

    void LoginWindow::sessionChangedCallback(int display)
    {
        if(display == displayNum)
        {
            ui->lineEditEncryption->setText(getEncryptionInfo(displayNum));
        }
    }

    void LoginWindow::loginSuccessCallback(int display, const QString & username)
    {
        if(display == displayNum)
        {
            close();
        }
    }

    void LoginWindow::setLoginPasswordCallback(int display, const QString & login, const QString & pass,
            const bool & autoLogin)
    {
        if(display == displayNum && 0 < login.size())
        {
            prefferedLogin = login;
            ui->comboBoxUsername->setEditText(prefferedLogin);
            ui->lineEditPassword->setFocus();

            if(0 < pass.size())
            {
                ui->lineEditPassword->setText(pass);
            }

            if(autoLogin)
            {
                loginClicked();
            }
        }
    }

    void LoginWindow::xcbXkbGroupChangedEvent(int group)
    {
        if(auto extXkb = static_cast<const XCB::ModuleXkb*>(XCB::RootDisplay::getExtensionConst(XCB::Module::XKB)))
        {
            auto names = extXkb->getNames();

            if(0 <= group && group < names.size())
            {
                ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));
            }
        }
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
}
