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

#include "openssl/bio.h"
#include "openssl/pem.h"
#include "openssl/x509.h"
#include "openssl/pkcs7.h"
#include "openssl/safestack.h"

#include <chrono>
#include <thread>

#include <QFile>
#include <QScreen>
#include <QCursor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QApplication>
#include <QDesktopWidget>
#include <QGuiApplication>

#include "ltsm_global.h"
#include "ltsm_tools.h"
#include "ltsm_helperwindow.h"
#include "ui_ltsm_helperwindow.h"

using namespace std::chrono_literals;

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

void LTSM_HelperSDBus::onTokenAuthAttached(const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs)
{
    tokenAttached(display, serial, description, certs);
}

void LTSM_HelperSDBus::onTokenAuthDetached(const int32_t& display, const std::string& serial)
{
    tokenDetached(display, serial);
}

void LTSM_HelperSDBus::onTokenAuthReplyCheck(const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt)
{
    tokenReplyCheck(display, serial, cert, decrypt);
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

void LTSM_HelperSDBus::sendTokenAuthEncrypted(int displayNum, const std::string & serial, const std::string & pin, uint32_t cert, const uint8_t* ptr, size_t len)
{
    std::thread([this, display = displayNum, serial, pin, cert, buf = std::vector<uint8_t>(ptr, ptr + len)]()
    {
        this->helperTokenAuthEncrypted(display, serial, pin, cert, std::move(buf));
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
    ui->setupUi(this);
    ui->labelDomain->hide();
    ui->comboBoxDomain->hide();
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
    timerReloadUsers = startTimer(std::chrono::minutes(3));

    auto group = xkbGroup();
    auto names = xkbNames();

    if(0 <= group && group < names.size())
        ui->labelXkb->setText(QString::fromStdString(names[group]).toUpper().left(2));

#ifdef LTSM_TOKEN_AUTH
    try
    {
        ldap.reset(new LTSM::LdapWrapper());
    }
    catch(...)
    {
        ldap.reset();
    }
#endif
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

    if(! tokenAuthMode)
    {
        sendAuthenticateLoginPass(displayNum, ui->comboBoxUsername->currentText(), ui->lineEditPassword->text());
        return;
    }

#ifdef LTSM_TOKEN_AUTH
    // tokenAuthMode
    auto jo = ui->comboBoxDomain->currentData().toJsonObject();
    auto serial = jo["serial"].toString();
    auto cert = ui->comboBoxUsername->currentData().toString();

    tokenCheck = LTSM::Tools::randomHexString(64);
    tokenSerial = serial.toStdString();
    tokenCert = cert.toStdString();

    setLabelInfo("check token...");

    std::thread([this, display = displayNum, content = tokenCheck, cert = cert.toStdString(), serial = serial.toStdString(), pin = ui->lineEditPassword->text().toStdString()]
    {
        // crypt to pkcs7
        std::unique_ptr<BIO, void(*)(BIO*)> bio1{ BIO_new_mem_buf(cert.data(), cert.size()), BIO_free_all };
        std::unique_ptr<X509, void(*)(X509*)> x509{ PEM_read_bio_X509(bio1.get(), nullptr, nullptr, nullptr), X509_free };
        if(! x509)
        {
            return;
        }

        std::unique_ptr<BIO, void(*)(BIO*)> bio2{ BIO_new(BIO_s_mem()), BIO_free_all };
        BIO_write(bio2.get(), content.data(), content.size());

        auto sk_X509_free2 = [](stack_st_X509* st)
        {
            sk_X509_free(st);
        };
    
        std::unique_ptr<stack_st_X509, void(*)(stack_st_X509*)> certstack{ sk_X509_new_null(), sk_X509_free2 };
        sk_X509_push(certstack.get(), x509.get());
    
        std::unique_ptr<PKCS7, void(*)(PKCS7*)> p7{ PKCS7_encrypt(certstack.get(), bio2.get(), EVP_aes_128_cbc(),  PKCS7_BINARY), PKCS7_free };

        // pkcs7 to der format
        std::unique_ptr<BIO, void(*)(BIO*)> bio3{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(0 < i2d_PKCS7_bio(bio3.get(), p7.get()))
        {
            uint8_t* buf = nullptr;
            size_t len = BIO_get_mem_data(bio3.get(), & buf);

            this->sendTokenAuthEncrypted(display, serial, pin, LTSM::Tools::crc32b(cert), buf, len);
        }
    }).detach();
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

struct CertInfo
{
    std::string subjectName;
    std::string issuerName;
    std::string notBefore;
    std::string notAfter;
    std::string serialNumber;
};

CertInfo getX509CertInfo(const std::string& cert)
{
    CertInfo certInfo{ .subjectName = "unknown" };

#ifdef LTSM_TOKEN_AUTH
    std::unique_ptr<BIO, void(*)(BIO*)> bioX509{ BIO_new_mem_buf(cert.data(), cert.size()), BIO_free_all };
    std::unique_ptr<X509, void(*)(X509*)> x509{ PEM_read_bio_X509(bioX509.get(), nullptr, nullptr, nullptr), X509_free };
    if(x509)
    {

        std::unique_ptr<BIO, void(*)(BIO*)> bio{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(X509_NAME_print(bio.get(), X509_get_subject_name(x509.get()), 0))
        {
            char* ptr = nullptr;
            size_t len = BIO_get_mem_data(bio.get(), & ptr);
            certInfo.subjectName.assign(ptr, len);
        }

        BIO_reset(bio.get());

        if(X509_NAME_print(bio.get(), X509_get_issuer_name(x509.get()), 0))
        {
            char* ptr = nullptr;
            size_t len = BIO_get_mem_data(bio.get(), & ptr);
            certInfo.issuerName.assign(ptr, len);
        }

        BIO_reset(bio.get());

        if(ASN1_TIME_print(bio.get(), X509_get_notBefore(x509.get())))
        {
            char* ptr = nullptr;
            size_t len = BIO_get_mem_data(bio.get(), & ptr);
            certInfo.notBefore.assign(ptr, len);
        }

        BIO_reset(bio.get());

        if(ASN1_TIME_print(bio.get(), X509_get_notAfter(x509.get())))
        {
            char* ptr = nullptr;
            size_t len = BIO_get_mem_data(bio.get(), & ptr);
            certInfo.notAfter.assign(ptr, len);
        }

        BIO_reset(bio.get());

        std::unique_ptr<BIGNUM, void(*)(BIGNUM*)> bn{ ASN1_INTEGER_to_BN(X509_get_serialNumber(x509.get()), NULL), BN_free };
        if(bn)
        {
            size_t len = BN_num_bytes(bn.get());
            std::vector<uint8_t> buf(len, 0);
            BN_bn2bin(bn.get(), buf.data());

            certInfo.serialNumber = LTSM::Tools::buffer2hexstring<uint8_t>(buf.data(), buf.size(), 2, ":", false);
        }
    }
#endif
    return certInfo;
}

void LTSM_HelperWindow::usernameChanged(const QString & user)
{
    if(tokenAuthMode)
    {
        auto certInfo = getX509CertInfo(ui->comboBoxUsername->currentData().toString().toStdString());
        auto tooltip = QString("subjectName: %1\nissuerName: %2\nserialNumber: %3\nnotAfter: %4\nnotBefore: %5").
            arg(QString::fromStdString(certInfo.subjectName)).
            arg(QString::fromStdString(certInfo.issuerName)).
            arg(QString::fromStdString(certInfo.serialNumber)).
            arg(QString::fromStdString(certInfo.notAfter)).
            arg(QString::fromStdString(certInfo.notBefore));
        ui->comboBoxUsername->setToolTip(tooltip);
    }
    else
        ui->pushButtonLogin->setDisabled(ui->comboBoxUsername->currentText().isEmpty() || ui->lineEditPassword->text().isEmpty());
}

void LTSM_HelperWindow::tokenChanged(const QString& serial)
{
    if(tokenAuthMode)
    {
        auto jo = ui->comboBoxDomain->currentData().toJsonObject();
        auto certs = jo["certs"].toArray().toVariantList();

        ui->comboBoxUsername->clear();
        if(certs.size())
        {
            for(auto & var : certs)
            {
                auto certInfo = getX509CertInfo(var.toString().toStdString());
                ui->comboBoxUsername->insertItem(0, QString::fromStdString(certInfo.subjectName), var);
                auto tooltip = QString("subjectName: %1\nissuerName: %2\nserialNumber: %3\nnotAfter: %4\nnotBefore: %5").
                    arg(QString::fromStdString(certInfo.subjectName)).
                    arg(QString::fromStdString(certInfo.issuerName)).
                    arg(QString::fromStdString(certInfo.serialNumber)).
                    arg(QString::fromStdString(certInfo.notAfter)).
                    arg(QString::fromStdString(certInfo.notBefore));
                ui->comboBoxUsername->setItemData(0, tooltip, Qt::ToolTipRole);
            }

            ui->comboBoxUsername->setCurrentIndex(0);
            ui->comboBoxUsername->lineEdit()->setReadOnly(true);
            ui->pushButtonLogin->setDisabled(false);
        }
        else
        {
            ui->pushButtonLogin->setDisabled(true);
        }
    }
}

void LTSM_HelperWindow::tokenAttached(const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs)
{
    if(display == displayNum)
    {
        tokenAuthMode = true;

        ui->labelDomain->setVisible(true);
        ui->comboBoxDomain->setVisible(true);

        ui->labelDomain->setText("token id:");
        ui->labelUsername->setText("certificate:");
        ui->labelPassword->setText("pin code:");

        ui->comboBoxUsername->clear();
        ui->lineEditPassword->clear();

        QJsonArray ja;
        for(auto & cert : certs)
            ja.append(QString::fromStdString(cert));

        QJsonObject jo;
        jo.insert("serial", QString::fromStdString(serial));
        jo.insert("certs", ja);

        ui->comboBoxDomain->addItem(QString("%1 (%2)").arg(description.c_str()).arg(serial.c_str()), QVariant(jo));
        ui->comboBoxDomain->lineEdit()->setReadOnly(true);
        ui->comboBoxDomain->setCurrentIndex(0);

        ui->lineEditPassword->setFocus();
    }
}

void LTSM_HelperWindow::tokenDetached(const int32_t& display, const std::string& serial)
{
    if(display == displayNum && tokenAuthMode)
    {
        if(1 < ui->comboBoxDomain->count())
        {
            for(int index = 0; index < ui->comboBoxDomain->count(); ++index)
            {
                auto jo = ui->comboBoxDomain->itemData(index).toJsonObject();
                auto serial2 = jo["serial"].toString();

                if(serial2.toStdString() == serial)
                {
                    ui->comboBoxDomain->removeItem(index);
                    ui->comboBoxDomain->setCurrentIndex(0);
                    break;
                }
            }
        }
        else
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
    }
}

#ifdef LTSM_TOKEN_AUTH
std::vector<uint8_t> convertX509Pem2Der(const std::string & pem)
{
    std::vector<uint8_t> der;
    std::unique_ptr<BIO, void(*)(BIO*)> bio1{ BIO_new_mem_buf(pem.data(), pem.size()), BIO_free_all };
    std::unique_ptr<X509, void(*)(X509*)> x509{ PEM_read_bio_X509(bio1.get(), nullptr, nullptr, nullptr), X509_free };

    if(x509)
    {
        std::unique_ptr<BIO, void(*)(BIO*)> bio2{ BIO_new(BIO_s_mem()), BIO_free_all };
        if(0 < i2d_X509_bio(bio2.get(), x509.get()))
        {
            uint8_t* ptr = nullptr;
            size_t len = BIO_get_mem_data(bio2.get(), & ptr);

            der.assign(ptr, ptr + len);
        }
    }

    return der;
}

#endif

void LTSM_HelperWindow::tokenReplyCheck(const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt)
{
    if(display == displayNum && tokenAuthMode)
    {
        if(tokenSerial == serial && LTSM::Tools::crc32b(tokenCert) == cert && tokenCheck == decrypt)
        {
            // FIXME LDAP find cert login
#ifdef LTSM_TOKEN_AUTH
            if(ldap)
            {
                auto dn = ldap->findDnFromCertificate(convertX509Pem2Der(tokenCert));
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
        else
        {
            setLabelError(QString::fromStdString(decrypt));
        }

        // error
        ui->pushButtonLogin->setDisabled(false);
        ui->comboBoxUsername->setDisabled(false);
        ui->comboBoxUsername->setToolTip("");
        ui->lineEditPassword->setDisabled(false);
        ui->lineEditPassword->setFocus();
    }
}
