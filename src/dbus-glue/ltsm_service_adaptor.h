
/*
 * This file was automatically generated by sdbus-c++-xml2cpp; DO NOT EDIT!
 */

#ifndef __sdbuscpp__ltsm_service_adaptor_h__adaptor__H__
#define __sdbuscpp__ltsm_service_adaptor_h__adaptor__H__

#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <tuple>

namespace LTSM {
namespace Manager {

class Service_adaptor
{
public:
    static constexpr const char* INTERFACE_NAME = "LTSM.Manager.Service";

protected:
    Service_adaptor(sdbus::IObject& object)
        : object_(object)
    {
        object_.registerMethod("busGetServiceVersion").onInterface(INTERFACE_NAME).withOutputParamNames("version").implementedAs([this](){ return this->busGetServiceVersion(); });
        object_.registerMethod("busStartLoginSession").onInterface(INTERFACE_NAME).withInputParamNames("depth", "remoteAddr", "connType").withOutputParamNames("display").implementedAs([this](const uint8_t& depth, const std::string& remoteAddr, const std::string& connType){ return this->busStartLoginSession(depth, remoteAddr, connType); });
        object_.registerMethod("busCreateAuthFile").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("path").implementedAs([this](const int32_t& display){ return this->busCreateAuthFile(display); });
        object_.registerMethod("busShutdownConnector").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("success").implementedAs([this](const int32_t& display){ return this->busShutdownConnector(display); });
        object_.registerMethod("busShutdownService").onInterface(INTERFACE_NAME).implementedAs([this](){ return this->busShutdownService(); });
        object_.registerMethod("busShutdownDisplay").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("success").implementedAs([this](const int32_t& display){ return this->busShutdownDisplay(display); });
        object_.registerMethod("busStartUserSession").onInterface(INTERFACE_NAME).withInputParamNames("display", "userName", "remoteAddr", "connType").withOutputParamNames("display").implementedAs([this](const int32_t& display, const std::string& userName, const std::string& remoteAddr, const std::string& connType){ return this->busStartUserSession(display, userName, remoteAddr, connType); });
        object_.registerMethod("busSendMessage").onInterface(INTERFACE_NAME).withInputParamNames("display", "message").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& message){ return this->busSendMessage(display, message); });
        object_.registerMethod("busSendNotify").onInterface(INTERFACE_NAME).withInputParamNames("display", "summary", "body", "icontype", "urgency").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& summary, const std::string& body, const uint8_t& icontype, const uint8_t& urgency){ return this->busSendNotify(display, summary, body, icontype, urgency); });
        object_.registerMethod("busSetDebugLevel").onInterface(INTERFACE_NAME).withInputParamNames("level").implementedAs([this](const std::string& level){ return this->busSetDebugLevel(level); });
        object_.registerMethod("busSetConnectorDebugLevel").onInterface(INTERFACE_NAME).withInputParamNames("display", "level").implementedAs([this](const int32_t& display, const std::string& level){ return this->busSetConnectorDebugLevel(display, level); });
        object_.registerMethod("busSetChannelDebug").onInterface(INTERFACE_NAME).withInputParamNames("display", "channel", "debug").implementedAs([this](const int32_t& display, const uint8_t& channel, const bool& debug){ return this->busSetChannelDebug(display, channel, debug); });
        object_.registerMethod("busSetEncryptionInfo").onInterface(INTERFACE_NAME).withInputParamNames("display", "info").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& info){ return this->busSetEncryptionInfo(display, info); });
        object_.registerMethod("busSetSessionDurationSec").onInterface(INTERFACE_NAME).withInputParamNames("display", "duration").withOutputParamNames("result").implementedAs([this](const int32_t& display, const uint32_t& duration){ return this->busSetSessionDurationSec(display, duration); });
        object_.registerMethod("busSetSessionPolicy").onInterface(INTERFACE_NAME).withInputParamNames("display", "policy").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& policy){ return this->busSetSessionPolicy(display, policy); });
        object_.registerMethod("busSetLoginsDisable").onInterface(INTERFACE_NAME).withInputParamNames("action").withOutputParamNames("result").implementedAs([this](const bool& action){ return this->busSetLoginsDisable(action); });
        object_.registerMethod("busSetSessionEnvironments").onInterface(INTERFACE_NAME).withInputParamNames("display", "map").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::map<std::string, std::string>& map){ return this->busSetSessionEnvironments(display, map); });
        object_.registerMethod("busSetSessionOptions").onInterface(INTERFACE_NAME).withInputParamNames("display", "map").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::map<std::string, std::string>& map){ return this->busSetSessionOptions(display, map); });
        object_.registerMethod("busSetSessionKeyboardLayouts").onInterface(INTERFACE_NAME).withInputParamNames("display", "layouts").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::vector<std::string>& layouts){ return this->busSetSessionKeyboardLayouts(display, layouts); });
        object_.registerMethod("busEncryptionInfo").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busEncryptionInfo(display); });
        object_.registerMethod("busDisplayResized").onInterface(INTERFACE_NAME).withInputParamNames("display", "width", "height").withOutputParamNames("result").implementedAs([this](const int32_t& display, const uint16_t& width, const uint16_t& height){ return this->busDisplayResized(display, width, height); });
        object_.registerMethod("busIdleTimeoutAction").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busIdleTimeoutAction(display); });
        object_.registerMethod("busConnectorTerminated").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busConnectorTerminated(display); });
        object_.registerMethod("busConnectorAlive").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busConnectorAlive(display); });
        object_.registerMethod("busTransferFilesRequest").onInterface(INTERFACE_NAME).withInputParamNames("display", "files").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::vector<sdbus::Struct<std::string, uint32_t>>& files){ return this->busTransferFilesRequest(display, files); });
        object_.registerMethod("busTransferFileStarted").onInterface(INTERFACE_NAME).withInputParamNames("display", "tmpfile", "filesz", "dstfile").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& tmpfile, const uint32_t& filesz, const std::string& dstfile){ return this->busTransferFileStarted(display, tmpfile, filesz, dstfile); });
        object_.registerMethod("busSetAuthenticateLoginPass").onInterface(INTERFACE_NAME).withInputParamNames("display", "login", "password").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& login, const std::string& password){ return this->busSetAuthenticateLoginPass(display, login, password); });
        object_.registerMethod("busSetAuthenticateToken").onInterface(INTERFACE_NAME).withInputParamNames("display", "login").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& login){ return this->busSetAuthenticateToken(display, login); });
        object_.registerMethod("busGetSessionJson").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busGetSessionJson(display); });
        object_.registerMethod("busGetSessionsJson").onInterface(INTERFACE_NAME).withOutputParamNames("result").implementedAs([this](){ return this->busGetSessionsJson(); });
        object_.registerMethod("busRenderRect").onInterface(INTERFACE_NAME).withInputParamNames("display", "rect", "color", "fill").withOutputParamNames("result").implementedAs([this](const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill){ return this->busRenderRect(display, rect, color, fill); });
        object_.registerMethod("busRenderText").onInterface(INTERFACE_NAME).withInputParamNames("display", "text", "pos", "color").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color){ return this->busRenderText(display, text, pos, color); });
        object_.registerMethod("busRenderClear").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->busRenderClear(display); });
        object_.registerMethod("busCreateChannel").onInterface(INTERFACE_NAME).withInputParamNames("display", "client", "cmode", "server", "smode", "speed").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed){ return this->busCreateChannel(display, client, cmode, server, smode, speed); });
        object_.registerMethod("busDestroyChannel").onInterface(INTERFACE_NAME).withInputParamNames("display", "channel").withOutputParamNames("result").implementedAs([this](const int32_t& display, const uint8_t& channel){ return this->busDestroyChannel(display, channel); });
        object_.registerMethod("tokenAuthAttached").onInterface(INTERFACE_NAME).withInputParamNames("display", "serial", "description", "certs").implementedAs([this](const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs){ return this->tokenAuthAttached(display, serial, description, certs); });
        object_.registerMethod("tokenAuthDetached").onInterface(INTERFACE_NAME).withInputParamNames("display", "serial").implementedAs([this](const int32_t& display, const std::string& serial){ return this->tokenAuthDetached(display, serial); });
        object_.registerMethod("tokenAuthReply").onInterface(INTERFACE_NAME).withInputParamNames("display", "serial", "cert", "decrypt").implementedAs([this](const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt){ return this->tokenAuthReply(display, serial, cert, decrypt); });
        object_.registerMethod("helperIsAutoComplete").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("success").implementedAs([this](const int32_t& display){ return this->helperIsAutoComplete(display); });
        object_.registerMethod("helperGetTitle").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->helperGetTitle(display); });
        object_.registerMethod("helperGetDateFormat").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->helperGetDateFormat(display); });
        object_.registerMethod("helperGetUsersList").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->helperGetUsersList(display); });
        object_.registerMethod("helperWidgetStartedAction").onInterface(INTERFACE_NAME).withInputParamNames("display").withOutputParamNames("result").implementedAs([this](const int32_t& display){ return this->helperWidgetStartedAction(display); });
        object_.registerMethod("helperSetSessionLoginPassword").onInterface(INTERFACE_NAME).withInputParamNames("display", "login", "password", "action").withOutputParamNames("result").implementedAs([this](const int32_t& display, const std::string& login, const std::string& password, const bool& action){ return this->helperSetSessionLoginPassword(display, login, password, action); });
        object_.registerMethod("helperTokenAuthEncrypted").onInterface(INTERFACE_NAME).withInputParamNames("display", "serial", "pin", "cert", "data").implementedAs([this](const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& data){ return this->helperTokenAuthEncrypted(display, serial, pin, cert, data); });
        object_.registerSignal("helperWidgetStarted").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("helperWidgetTimezone").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string>("display", "tz");
        object_.registerSignal("helperSetLoginPassword").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, bool>("display", "login", "pass", "autologin");
        object_.registerSignal("helperWidgetCentered").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("tokenAuthAttached").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, std::vector<std::string>>("display", "serial", "description", "certs");
        object_.registerSignal("tokenAuthDetached").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string>("display", "serial");
        object_.registerSignal("tokenAuthCheckPkcs7").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, uint32_t, std::vector<uint8_t>>("display", "serial", "pin", "cert", "pkcs7");
        object_.registerSignal("tokenAuthReplyCheck").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, uint32_t, std::string>("display", "serial", "cert", "decrypt");
        object_.registerSignal("loginFailure").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string>("display", "msg");
        object_.registerSignal("loginSuccess").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, uint32_t>("display", "userName", "userUid");
        object_.registerSignal("shutdownConnector").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("pingConnector").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("sendBellSignal").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("sessionReconnect").onInterface(INTERFACE_NAME).withParameters<std::string, std::string>("removeAddr", "connType");
        object_.registerSignal("sessionChanged").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("displayRemoved").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("clearRenderPrimitives").onInterface(INTERFACE_NAME).withParameters<int32_t>("display");
        object_.registerSignal("createChannel").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, std::string, std::string, std::string>("display", "client", "cmode", "server", "smode", "speed");
        object_.registerSignal("destroyChannel").onInterface(INTERFACE_NAME).withParameters<int32_t, uint8_t>("display", "channel");
        object_.registerSignal("transferAllow").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, std::string>("display", "filepath", "tmpfile", "dstdir");
        object_.registerSignal("createListener").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string, std::string, std::string, std::string, uint8_t>("display", "client", "cmode", "server", "smode", "speed", "limit");
        object_.registerSignal("destroyListener").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string>("display", "client", "server");
        object_.registerSignal("addRenderRect").onInterface(INTERFACE_NAME).withParameters<int32_t, sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>, sdbus::Struct<uint8_t, uint8_t, uint8_t>, bool>("display", "rect", "color", "fill");
        object_.registerSignal("addRenderText").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, sdbus::Struct<int16_t, int16_t>, sdbus::Struct<uint8_t, uint8_t, uint8_t>>("display", "text", "pos", "color");
        object_.registerSignal("debugLevel").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string>("display", "level");
        object_.registerSignal("debugChannel").onInterface(INTERFACE_NAME).withParameters<int32_t, uint8_t, bool>("display", "channel", "debug");
        object_.registerSignal("fuseSessionStart").onInterface(INTERFACE_NAME).withParameters<int32_t, std::string, std::string>("display", "addresses", "mount");
    }

    ~Service_adaptor() = default;

public:
    void emitHelperWidgetStarted(const int32_t& display)
    {
        object_.emitSignal("helperWidgetStarted").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitHelperWidgetTimezone(const int32_t& display, const std::string& tz)
    {
        object_.emitSignal("helperWidgetTimezone").onInterface(INTERFACE_NAME).withArguments(display, tz);
    }

    void emitHelperSetLoginPassword(const int32_t& display, const std::string& login, const std::string& pass, const bool& autologin)
    {
        object_.emitSignal("helperSetLoginPassword").onInterface(INTERFACE_NAME).withArguments(display, login, pass, autologin);
    }

    void emitHelperWidgetCentered(const int32_t& display)
    {
        object_.emitSignal("helperWidgetCentered").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitTokenAuthAttached(const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs)
    {
        object_.emitSignal("tokenAuthAttached").onInterface(INTERFACE_NAME).withArguments(display, serial, description, certs);
    }

    void emitTokenAuthDetached(const int32_t& display, const std::string& serial)
    {
        object_.emitSignal("tokenAuthDetached").onInterface(INTERFACE_NAME).withArguments(display, serial);
    }

    void emitTokenAuthCheckPkcs7(const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& pkcs7)
    {
        object_.emitSignal("tokenAuthCheckPkcs7").onInterface(INTERFACE_NAME).withArguments(display, serial, pin, cert, pkcs7);
    }

    void emitTokenAuthReplyCheck(const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt)
    {
        object_.emitSignal("tokenAuthReplyCheck").onInterface(INTERFACE_NAME).withArguments(display, serial, cert, decrypt);
    }

    void emitLoginFailure(const int32_t& display, const std::string& msg)
    {
        object_.emitSignal("loginFailure").onInterface(INTERFACE_NAME).withArguments(display, msg);
    }

    void emitLoginSuccess(const int32_t& display, const std::string& userName, const uint32_t& userUid)
    {
        object_.emitSignal("loginSuccess").onInterface(INTERFACE_NAME).withArguments(display, userName, userUid);
    }

    void emitShutdownConnector(const int32_t& display)
    {
        object_.emitSignal("shutdownConnector").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitPingConnector(const int32_t& display)
    {
        object_.emitSignal("pingConnector").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitSendBellSignal(const int32_t& display)
    {
        object_.emitSignal("sendBellSignal").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitSessionReconnect(const std::string& removeAddr, const std::string& connType)
    {
        object_.emitSignal("sessionReconnect").onInterface(INTERFACE_NAME).withArguments(removeAddr, connType);
    }

    void emitSessionChanged(const int32_t& display)
    {
        object_.emitSignal("sessionChanged").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitDisplayRemoved(const int32_t& display)
    {
        object_.emitSignal("displayRemoved").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitClearRenderPrimitives(const int32_t& display)
    {
        object_.emitSignal("clearRenderPrimitives").onInterface(INTERFACE_NAME).withArguments(display);
    }

    void emitCreateChannel(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed)
    {
        object_.emitSignal("createChannel").onInterface(INTERFACE_NAME).withArguments(display, client, cmode, server, smode, speed);
    }

    void emitDestroyChannel(const int32_t& display, const uint8_t& channel)
    {
        object_.emitSignal("destroyChannel").onInterface(INTERFACE_NAME).withArguments(display, channel);
    }

    void emitTransferAllow(const int32_t& display, const std::string& filepath, const std::string& tmpfile, const std::string& dstdir)
    {
        object_.emitSignal("transferAllow").onInterface(INTERFACE_NAME).withArguments(display, filepath, tmpfile, dstdir);
    }

    void emitCreateListener(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed, const uint8_t& limit)
    {
        object_.emitSignal("createListener").onInterface(INTERFACE_NAME).withArguments(display, client, cmode, server, smode, speed, limit);
    }

    void emitDestroyListener(const int32_t& display, const std::string& client, const std::string& server)
    {
        object_.emitSignal("destroyListener").onInterface(INTERFACE_NAME).withArguments(display, client, server);
    }

    void emitAddRenderRect(const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill)
    {
        object_.emitSignal("addRenderRect").onInterface(INTERFACE_NAME).withArguments(display, rect, color, fill);
    }

    void emitAddRenderText(const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color)
    {
        object_.emitSignal("addRenderText").onInterface(INTERFACE_NAME).withArguments(display, text, pos, color);
    }

    void emitDebugLevel(const int32_t& display, const std::string& level)
    {
        object_.emitSignal("debugLevel").onInterface(INTERFACE_NAME).withArguments(display, level);
    }

    void emitDebugChannel(const int32_t& display, const uint8_t& channel, const bool& debug)
    {
        object_.emitSignal("debugChannel").onInterface(INTERFACE_NAME).withArguments(display, channel, debug);
    }

    void emitFuseSessionStart(const int32_t& display, const std::string& addresses, const std::string& mount)
    {
        object_.emitSignal("fuseSessionStart").onInterface(INTERFACE_NAME).withArguments(display, addresses, mount);
    }

private:
    virtual int32_t busGetServiceVersion() = 0;
    virtual int32_t busStartLoginSession(const uint8_t& depth, const std::string& remoteAddr, const std::string& connType) = 0;
    virtual std::string busCreateAuthFile(const int32_t& display) = 0;
    virtual bool busShutdownConnector(const int32_t& display) = 0;
    virtual void busShutdownService() = 0;
    virtual bool busShutdownDisplay(const int32_t& display) = 0;
    virtual int32_t busStartUserSession(const int32_t& display, const std::string& userName, const std::string& remoteAddr, const std::string& connType) = 0;
    virtual bool busSendMessage(const int32_t& display, const std::string& message) = 0;
    virtual bool busSendNotify(const int32_t& display, const std::string& summary, const std::string& body, const uint8_t& icontype, const uint8_t& urgency) = 0;
    virtual void busSetDebugLevel(const std::string& level) = 0;
    virtual void busSetConnectorDebugLevel(const int32_t& display, const std::string& level) = 0;
    virtual void busSetChannelDebug(const int32_t& display, const uint8_t& channel, const bool& debug) = 0;
    virtual bool busSetEncryptionInfo(const int32_t& display, const std::string& info) = 0;
    virtual bool busSetSessionDurationSec(const int32_t& display, const uint32_t& duration) = 0;
    virtual bool busSetSessionPolicy(const int32_t& display, const std::string& policy) = 0;
    virtual bool busSetLoginsDisable(const bool& action) = 0;
    virtual bool busSetSessionEnvironments(const int32_t& display, const std::map<std::string, std::string>& map) = 0;
    virtual bool busSetSessionOptions(const int32_t& display, const std::map<std::string, std::string>& map) = 0;
    virtual bool busSetSessionKeyboardLayouts(const int32_t& display, const std::vector<std::string>& layouts) = 0;
    virtual std::string busEncryptionInfo(const int32_t& display) = 0;
    virtual bool busDisplayResized(const int32_t& display, const uint16_t& width, const uint16_t& height) = 0;
    virtual bool busIdleTimeoutAction(const int32_t& display) = 0;
    virtual bool busConnectorTerminated(const int32_t& display) = 0;
    virtual bool busConnectorAlive(const int32_t& display) = 0;
    virtual bool busTransferFilesRequest(const int32_t& display, const std::vector<sdbus::Struct<std::string, uint32_t>>& files) = 0;
    virtual bool busTransferFileStarted(const int32_t& display, const std::string& tmpfile, const uint32_t& filesz, const std::string& dstfile) = 0;
    virtual bool busSetAuthenticateLoginPass(const int32_t& display, const std::string& login, const std::string& password) = 0;
    virtual bool busSetAuthenticateToken(const int32_t& display, const std::string& login) = 0;
    virtual std::string busGetSessionJson(const int32_t& display) = 0;
    virtual std::string busGetSessionsJson() = 0;
    virtual bool busRenderRect(const int32_t& display, const sdbus::Struct<int16_t, int16_t, uint16_t, uint16_t>& rect, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color, const bool& fill) = 0;
    virtual bool busRenderText(const int32_t& display, const std::string& text, const sdbus::Struct<int16_t, int16_t>& pos, const sdbus::Struct<uint8_t, uint8_t, uint8_t>& color) = 0;
    virtual bool busRenderClear(const int32_t& display) = 0;
    virtual bool busCreateChannel(const int32_t& display, const std::string& client, const std::string& cmode, const std::string& server, const std::string& smode, const std::string& speed) = 0;
    virtual bool busDestroyChannel(const int32_t& display, const uint8_t& channel) = 0;
    virtual void tokenAuthAttached(const int32_t& display, const std::string& serial, const std::string& description, const std::vector<std::string>& certs) = 0;
    virtual void tokenAuthDetached(const int32_t& display, const std::string& serial) = 0;
    virtual void tokenAuthReply(const int32_t& display, const std::string& serial, const uint32_t& cert, const std::string& decrypt) = 0;
    virtual bool helperIsAutoComplete(const int32_t& display) = 0;
    virtual std::string helperGetTitle(const int32_t& display) = 0;
    virtual std::string helperGetDateFormat(const int32_t& display) = 0;
    virtual std::vector<std::string> helperGetUsersList(const int32_t& display) = 0;
    virtual bool helperWidgetStartedAction(const int32_t& display) = 0;
    virtual bool helperSetSessionLoginPassword(const int32_t& display, const std::string& login, const std::string& password, const bool& action) = 0;
    virtual void helperTokenAuthEncrypted(const int32_t& display, const std::string& serial, const std::string& pin, const uint32_t& cert, const std::vector<uint8_t>& data) = 0;

private:
    sdbus::IObject& object_;
};

}} // namespaces

#endif
