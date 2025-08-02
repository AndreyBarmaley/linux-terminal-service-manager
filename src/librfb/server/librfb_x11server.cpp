/***********************************************************************
 *   Copyright © 2022 by Andrey Afletdinov <public.irkutsk@gmail.com>  *
 *                                                                     *
 *   Part of the LTSM: Linux Terminal Service Manager:                 *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager  *
 *                                                                     *
 *   This program is free software;                                    *
 *   you can redistribute it and/or modify it under the terms of the   *
 *   GNU Affero General Public License as published by the             *
 *   Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                               *
 *                                                                     *
 *   This program is distributed in the hope that it will be useful,   *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.              *
 *   See the GNU Affero General Public License for more details.       *
 *                                                                     *
 *   You should have received a copy of the                            *
 *   GNU Affero General Public License along with this program;        *
 *   if not, write to the Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.         *
 **********************************************************************/

#include <sys/stat.h>

#include <tuple>
#include <chrono>
#include <thread>

#include "ltsm_application.h"
#include "librfb_x11server.h"

#ifdef LTSM_ENCODING_FFMPEG
#include "librfb_ffmpeg.h"
#endif

using namespace std::chrono_literals;

namespace LTSM
{
    XCB::RootDisplay* RFB::X11Server::xcbDisplay(void)
    {
        return this;
    }

    const XCB::Region & RFB::X11Server::getClientRegion(void) const
    {
        return clientRegion;
    }

    void RFB::X11Server::xcbFixesCursorChangedEvent(void)
    {
        clientUpdateCursor = isClientSupportedEncoding(ENCODING_RICH_CURSOR);
    }

    void RFB::X11Server::xcbDamageNotifyEvent(const xcb_rectangle_t & rt)
    {
        const std::scoped_lock guard{ serverLock };
        damageRegion.join(rt.x, rt.y, rt.width, rt.height);
    }

    void RFB::X11Server::xcbDisplayConnectedEvent(void)
    {
        ExtClip::x11AtomsUpdate(*this);

        if(xcbNoDamageOption())
        {
            XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
        }

        if(rfbClipboardEnable())
        {
            // init selection copy
            selectionChangedEvent();
        }
        else
        {
            XCB::RootDisplay::extensionDisable(XCB::Module::SELECTION_COPY);
            XCB::RootDisplay::extensionDisable(XCB::Module::SELECTION_PASTE);
        }
    }

    void RFB::X11Server::xcbRandrScreenSetSizeEvent(const XCB::Size & wsz)
    {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, wsz.width, wsz.height);
        displayResizeProcessed = true;
    }

    void RFB::X11Server::xcbRandrScreenChangedEvent(const XCB::Size & wsz, const xcb_randr_notify_event_t & notify)
    {
        Application::info("%s: size: [%" PRIu16 ", %" PRIu16 "], sequence: 0x%04" PRIx16, __FUNCTION__, wsz.width, wsz.height,
                          (uint16_t) notify.sequence);
        xcbShmInit();
        displayResizeProcessed = false;
        serverDisplayResizedEvent(wsz);

        if(isClientSupportedEncoding(ENCODING_EXT_DESKTOP_SIZE))
        {
            auto status = randrSequence == notify.sequence ?
                          RFB::DesktopResizeStatus::ClientSide : RFB::DesktopResizeStatus::ServerRuntime;
            std::thread([ = ]()
            {
                if(status == RFB::DesktopResizeStatus::ServerRuntime)
                {
                    this->sendEncodingDesktopResize(status, RFB::DesktopResizeError::NoError, wsz);
                    this->displayResizeEvent(wsz);
                }
                else if(this->displayResizeNegotiation)
                {
                    // clientSide
                    this->sendEncodingDesktopResize(status, RFB::DesktopResizeError::NoError, wsz);
                    this->displayResizeEvent(wsz);
                    this->displayResizeNegotiation = false;
                }
            }).detach();
        }
    }

    bool RFB::X11Server::xcbProcessingEvents(void)
    {
        while(rfbMessagesRunning())
        {
            if(! xcbAllowMessages())
            {
                std::this_thread::sleep_for(20ms);
                continue;
            }

            if(auto err = XCB::RootDisplay::hasError())
            {
                xcbDisableMessages(true);
                rfbMessagesShutdown();
                Application::error("%s: xcb error, code: %d", __FUNCTION__, err);
                return false;
            }

            if(auto ev = XCB::RootDisplay::pollEvent())
            {
                if(auto extShm = XCB::RootDisplay::getExtension(XCB::Module::SHM))
                {
                    uint16_t opcode = 0;

                    if(shm && extShm->isEventError(ev, & opcode))
                    {
                        Application::warning("%s: %s error: 0x%04" PRIx16, __FUNCTION__, "shm", opcode);
                        shm.reset();
                    }
                }
                else if(auto extFixes = XCB::RootDisplay::getExtensionConst(XCB::Module::XFIXES))
                {
                    uint16_t opcode = 0;

                    if(extFixes->isEventError(ev, & opcode))
                    {
                        Application::warning("%s: %s error: 0x%04" PRIx16, __FUNCTION__, "xfixes", opcode);
                    }
                }
            }
            else
            {
                std::this_thread::sleep_for(10ms);
            }
        }

        return true;
    }

    XCB::Size RFB::X11Server::displaySize(void) const
    {
        return XCB::RootDisplay::size();
    }

    void RFB::X11Server::serverScreenUpdateRequest(void)
    {
        fullscreenUpdateReq = true;
    }

    void RFB::X11Server::serverScreenUpdateRequest(const XCB::Region & reg)
    {
        const std::scoped_lock guard{ serverLock };
        damageRegion.join(reg);
    }

    /* Connector::X11Server */
    int RFB::X11Server::rfbCommunication(void)
    {
        serverSelectEncodings();
        // vnc session not activated trigger
        auto timerNotActivated = Tools::BaseTimer::create<std::chrono::seconds>(30, false, [this]()
        {
            if(this->rfbMessagesRunning())
            {
                Application::error("session timeout trigger: %s", "rfbMessagesRunning");
                throw rfb_error(NS_FuncName);
            }
        });

        // RFB 6.1.1 version
        int protover = serverHandshakeVersion();

        if(protover == 0)
        {
            return EXIT_FAILURE;
        }

        serverHandshakeVersionEvent();

        // RFB 6.1.2 security
        if(! serverSecurityInit(protover, rfbSecurityInfo()))
        {
            return EXIT_FAILURE;
        }

        serverSecurityInitEvent();
        // RFB 6.3.1 client init
        serverClientInit("X11 Remote Desktop", XCB::RootDisplay::size(), XCB::RootDisplay::depth(), serverFormat());
        timerNotActivated->stop();
        xcbShmInit();

        serverConnectedEvent();
        Application::info("%s: wait RFB messages...", __FUNCTION__);

        // xcb on
        xcbDisableMessages(false);
        bool mainLoop = true;
        auto frameTimePoint = std::chrono::steady_clock::now();
        size_t delayTimeout = 75;

        // process rfb messages background
        auto rfbThread = std::thread([ = ]()
        {
            this->rfbMessagesLoop();
        });

        auto xcbThread = std::thread([ = ]()
        {
            this->xcbProcessingEvents();
        });

        std::this_thread::sleep_for(10ms);

        // main loop
        while(mainLoop)
        {
            serverMainLoopEvent();

            if(! rfbMessagesRunning())
            {
                mainLoop = false;
                continue;
            }

            if(! xcbAllowMessages())
            {
                std::this_thread::sleep_for(20ms);
                continue;
            }

            if(displayResizeProcessed || displayResizeNegotiation)
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
                continue;
            }

            // check timepoint frame
            if(isClientLtsmSupported() && delayTimeout)
            {
                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - frameTimePoint);

                if(dt.count() < delayTimeout)
                {
                    Application::debug(DebugType::X11Srv, "%s: update time ms: %lu", __FUNCTION__, dt.count());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayTimeout - dt.count()));
                    continue;
                }

                if(isClientVideoSupported())
                {
                    fullscreenUpdateReq = true;
                }
            }

            if(xcbNoDamageOption() || fullscreenUpdateReq)
            {
                const std::scoped_lock guard{ serverLock };
                damageRegion = XCB::RootDisplay::region();
                fullscreenUpdateReq = false;
            }

            if(clientRegion.empty())
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
                continue;
            }

            if(damageRegion.empty())
            {
                // wait loop
                std::this_thread::sleep_for(5ms);
            }
            else
            {
                // processed frame update
                frameTimePoint = std::chrono::steady_clock::now();
                auto serverRegion = XCB::RootDisplay::region();

                const std::scoped_lock guard{ serverLock };
                // fix out of screen
                damageRegion = serverRegion.intersected(damageRegion.align(4));
                damageRegion = clientRegion.intersected(damageRegion);

                if(! sendUpdateSafe(damageRegion))
                {
                    rfbMessagesShutdown();
                    continue;
                }

                if(clientUpdateCursor)
                {
                    sendUpdateRichCursor();
                    clientUpdateCursor = false;
                }

                damageRegion.reset();

                // update timepoint
                auto frameRate = frameRateOption();
                delayTimeout = frameRate ? 1000 / frameRate : 0;
            }
        } // main loop

        waitUpdateProcess();

        if(xcbThread.joinable())
        {
            xcbThread.join();
        }

        if(rfbThread.joinable())
        {
            rfbThread.join();
        }

        return EXIT_SUCCESS;
    }

    void RFB::X11Server::serverRecvPixelFormatEvent(const PixelFormat &, bool bigEndian)
    {
        if(! clientFormat().compare(serverFormat(), true))
        {
            Application::warning("%s: client/server format not optimal", __FUNCTION__);
        }
    }

    void RFB::X11Server::serverRecvSetEncodingsEvent(const std::vector<int> & recvEncodings)
    {
        serverSelectEncodings();
        serverEncodingsEvent();

        if(isClientSupportedEncoding(ENCODING_EXT_DESKTOP_SIZE) && rfbDesktopResizeEnabled())
        {
            std::thread([this]
            {
                this->sendEncodingDesktopResize(RFB::DesktopResizeStatus::ServerRuntime, RFB::DesktopResizeError::NoError, XCB::RootDisplay::size());
            }).detach();
        }
    }

    void RFB::X11Server::serverRecvKeyEvent(bool pressed, uint32_t keysym)
    {
        auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtensionConst(XCB::Module::TEST));

        if(xcbAllowMessages() && test)
        {
            auto keycode = rfbUserKeycode(keysym);

            if(! keycode)
                keycode = XCB::RootDisplay::keysymToKeycodeAuto(keysym);

            if(keycode)
            {
                test->screenInputKeycode(keycode, pressed);
            }
        }
    }

    void RFB::X11Server::serverRecvPointerEvent(uint8_t mask, uint16_t posx, uint16_t posy)
    {
        if(xcbAllowMessages())
        {
            auto test = static_cast<const XCB::ModuleTest*>(XCB::RootDisplay::getExtensionConst(XCB::Module::TEST));

            if(! test)
            {
                return;
            }

            if(pressedMask^ mask)
            {
                for(int num = 0; num < 8; ++num)
                {
                    int bit = 1 << num;

                    if(bit & mask)
                    {
                        Application::debug(DebugType::X11Srv, "%s: xfb fake input pressed: %d", __FUNCTION__, num + 1);

                        test->screenInputButton(num + 1, XCB::Point(posx, posy), true);
                        pressedMask |= bit;
                    }
                    else if(bit & pressedMask)
                    {
                        Application::debug(DebugType::X11Srv, "%s: xfb fake input released: %d", __FUNCTION__, num + 1);

                        test->screenInputButton(num + 1, XCB::Point(posx, posy), false);
                        pressedMask &= ~bit;
                    }
                }
            }
            else
            {
                Application::debug(DebugType::X11Srv, "%s: xfb fake input move, pos: [%" PRIu16 ", %" PRIu16 "]", __FUNCTION__, posx, posy);

                test->screenInputMove(XCB::Point(posx, posy));
            }
        }
    }

    void RFB::X11Server::extClipboardSendEvent(const std::vector<uint8_t> & buf)
    {
        sendCutTextEvent(buf.data(), buf.size(), true);
    }

    uint16_t RFB::X11Server::extClipboardLocalTypes(void) const
    {
        return clipLocalTypes;
    }

    std::vector<uint8_t> RFB::X11Server::extClipboardLocalData(uint16_t type) const
    {
        if(0 == extClipboardRemoteCaps())
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }

        auto ptr = const_cast<RFB::X11Server*>(this);
        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY)))
        {
            for(const auto & atom: ExtClip::typesToX11Atoms(type, *this))
            {
                ptr->clientClipboard.clear();
                copy->convertSelection(atom, *this);

                // wait data from selectionReceiveData
                Tools::Timeout waitCb(100ms);

                while(true)
                {
                    std::this_thread::sleep_for(3ms);
                    
                    if(waitCb.check())
                        break;

                    const std::scoped_lock guard{ serverLock };

                    if(clientClipboard.size())
                        return clientClipboard;
                }
            }
        }

        return {};
    }

    void RFB::X11Server::extClipboardRemoteTypesEvent(uint16_t types)
    {
        if(extClipboardRemoteCaps())
        {
            clipRemoteTypes = types;

            if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
                    paste->setSelectionOwner(*this);
        }
        else
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::X11Server::extClipboardRemoteDataEvent(uint16_t type, std::vector<uint8_t> && buf)
    {
        if(extClipboardRemoteCaps())
        {
            const std::scoped_lock guard{ serverLock };
            clientClipboard.swap(buf);
        }
        else
        {
            Application::error("%s: unsupported encoding: %s", __FUNCTION__, encodingName(ENCODING_EXT_CLIPBOARD));
            throw rfb_error(NS_FuncName);
        }
    }

    void RFB::X11Server::selectionReceiveData(xcb_atom_t atom, const uint8_t* buf, uint32_t len) const
    {
        if(auto ptr = const_cast<RFB::X11Server*>(this))
        {
            if(extClipboardRemoteCaps())
            {
                const std::scoped_lock guard{ serverLock };
                ptr->clientClipboard.assign(buf, buf + len);
            }
            else
            {
                ptr->sendCutTextEvent(buf, len, false);
            }
        }
    }

    void RFB::X11Server::selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const
    {
        clipLocalTypes = 0;

        if(extClipboardRemoteCaps())
        {
            // calc types
            std::for_each(beg, end, [&](auto & atom)
            {
                clipLocalTypes |= ExtClip::x11AtomToType(atom);
            });
        
            if(auto owner = const_cast<X11Server*>(this))
                owner->sendExtClipboardNotify(clipLocalTypes);
        }
        else
        {
            auto ptr = const_cast<RFB::X11Server*>(this);
            if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY)))
            {
                for(const auto & atom: selectionSourceTargets())
                {
                    if(std::any_of(beg, end, [&](auto & trgt){ return atom == trgt; }))
                    {
                        return copy->convertSelection(atom, *this);
                    }
                }
            }
        }
    }

    void RFB::X11Server::selectionChangedEvent(void) const
    {
        auto ptr = const_cast<RFB::X11Server*>(this);
        if(auto copy = static_cast<XCB::ModuleCopySelection*>(ptr->getExtension(XCB::Module::SELECTION_COPY)))
        {
            copy->convertSelection(getAtom("TARGETS"), *this);
        }
    }

    std::vector<xcb_atom_t> RFB::X11Server::selectionSourceTargets(void) const
    {
        return ExtClip::typesToX11Atoms(extClipboardRemoteCaps() ?
                    clipRemoteTypes : ExtClipCaps::TypeText, *this);
    }

    bool RFB::X11Server::selectionSourceReady(xcb_atom_t atom) const
    {
        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt){ return atom == trgt; }))
            return false;

        if(extClipboardRemoteCaps())
        {
            uint16_t requestType = ExtClip::x11AtomToType(atom);
            auto ptr = const_cast<RFB::X11Server*>(this);

            ptr->clientClipboard.clear();
            ptr->sendExtClipboardRequest(requestType);

            // wait data from extClipboardRemoteDataEvent
            Tools::Timeout waitCb(3000ms);

            while(true)
            {
                std::this_thread::sleep_for(3ms);
                    
                if(waitCb.check())
                    break;

                const std::scoped_lock guard{ serverLock };

                if(clientClipboard.size())
                    return true;
            }
        }
        else
        {
            // basic mode
            return clientClipboard.size();
        }

        return false;
    }

    size_t RFB::X11Server::selectionSourceSize(xcb_atom_t atom) const
    {
        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt){ return atom == trgt; }))
            return 0;

        const std::scoped_lock guard{ serverLock };
        return clientClipboard.size();
    }

    std::vector<uint8_t> RFB::X11Server::selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const
    {
        auto targets = selectionSourceTargets();

        if(std::none_of(targets.begin(), targets.end(), [&](auto & trgt){ return atom == trgt; }))
            return {};

        const std::scoped_lock guard{ serverLock };

        if(offset + length <= clientClipboard.size())
        {
            auto beg = clientClipboard.begin() + offset;
            return std::vector<uint8_t>(beg, beg + length);
        }
        else
        {
            Application::error("%s: invalid length: %lu, offset: %" PRIu32, __FUNCTION__, length, offset);
        }

        return {};
    }

    void RFB::X11Server::serverRecvCutTextEvent(std::vector<uint8_t> && buf)
    {
        if(rfbClipboardEnable())
        {
            const std::scoped_lock guard{ serverLock };
            clientClipboard.swap(buf);

            if(xcbAllowMessages())
            {
                if(auto paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE)))
                    paste->setSelectionOwner(*this);
            }
        }
    }

    void RFB::X11Server::serverRecvFBUpdateEvent(bool incremental, const XCB::Region & region)
    {
        if(! xcbAllowMessages())
        {
            fullscreenUpdateReq = true;
            return;
        }

        const std::scoped_lock guard{ serverLock };
        clientRegion = region;

        if(! incremental)
        {
            fullscreenUpdateReq = true;
        }
        else if(isContinueUpdatesProcessed())
        {
            // skipped FramebufferUpdateRequest
            // ref: https://github.com/rfbproto/rfbproto/blob/master/rfbproto.rst#enablecontinuousupdates
            clientRegion.reset();
        }
    }

    void RFB::X11Server::serverRecvDesktopSizeEvent(const std::vector<RFB::ScreenInfo> & screens)
    {
        XCB::Region desktop(0, 0, 0, 0);

        for(const auto & info : screens)
        {
            Application::info("%s: screen id: 0x%08" PRIx32 ", region: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16
                              "], flags: 0x%08" PRIx32,
                              __FUNCTION__, info.id, info.posx, info.posy, info.width, info.height, info.flags);
            desktop.join(XCB::Region(info.posx, info.posy, info.width, info.height));
        }

        if(desktop.x != 0 && desktop.y != 0)
        {
            Application::error("%s: incorrect desktop size: [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__,
                               desktop.x, desktop.y, desktop.width, desktop.height);
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::InvalidScreenLayout,
                                      XCB::RootDisplay::size());
        }
        else if(! xcbAllowMessages())
        {
            Application::error("%s: xcb disabled", __FUNCTION__);
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources, XCB::Size{0, 0});
        }
        else if(XCB::RootDisplay::size() == desktop.toSize())
        {
            sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::NoError,
                                      XCB::RootDisplay::size());
        }
        else
        {
            displayResizeNegotiation = true;
            std::thread([ &, sz = desktop.toSize()]
            {
                uint16_t sequence = 0;

                waitUpdateProcess();

                if(XCB::RootDisplay::setRandrScreenSize(sz, & sequence))
                {
                    randrSequence = sequence;
                }
                else
                {
                    sendEncodingDesktopResize(RFB::DesktopResizeStatus::ClientSide, RFB::DesktopResizeError::OutOfResources,
                                              XCB::RootDisplay::size());
                    displayResizeNegotiation = false;
                    displayResizeProcessed = false;
                    randrSequence = 0;
                }
            }).detach();
        }
    }

    void RFB::X11Server::sendUpdateRichCursor(void)
    {
        if(auto fixes = static_cast<const XCB::ModuleWindowFixes*>(XCB::RootDisplay::getExtensionConst(XCB::Module::WINFIXES)))
        {
            XCB::CursorImage replyCursor = fixes->getCursorImage();
            const auto & reply = replyCursor.reply();

            if(auto ptr = replyCursor.data())
            {
                size_t argbSize = reply->width * reply->height;
                size_t dataSize = replyCursor.size();

                Application::debug(X11Srv, "%s: data lenth: %lu", __FUNCTION__, dataSize);

                if(dataSize == argbSize)
                {
                    auto cursorRegion = XCB::Region(reply->x, reply->y, reply->width, reply->height);
#if (__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__)
                    auto cursorFB = FrameBuffer(reinterpret_cast<uint8_t*>(ptr), cursorRegion, BGRA32);
#else
                    auto cursorFB = FrameBuffer(reinterpret_cast<uint8_t*>(ptr), cursorRegion, ARGB32);
#endif
                    sendEncodingRichCursor(cursorFB, reply->xhot, reply->yhot);
                }
                else
                {
                    Application::warning("%s: size mismatch, data: %lu, argb: %lu", __FUNCTION__, dataSize, argbSize);
                }
            }
        }
    }

    void RFB::X11Server::serverSendFBUpdateEvent(const XCB::Region & reg)
    {
        if(! xcbNoDamageOption())
        {
            XCB::RootDisplay::rootDamageSubtrack(reg);
        }
    }

    void RFB::X11Server::xcbShmInit(uid_t uid)
    {
        if(auto ext = static_cast<const XCB::ModuleShm*>(XCB::RootDisplay::getExtension(XCB::Module::SHM)))
        {
            auto dsz = XCB::RootDisplay::size();
            auto bpp = XCB::RootDisplay::bitsPerPixel() >> 3;
            shm = ext->createShm(dsz.width* dsz.height* bpp, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP, false, uid);
        }
    }

    XcbFrameBuffer RFB::X11Server::serverFrameBuffer(const XCB::Region & reg) const
    {
        Application::debug(DebugType::X11Srv, "%s: region [%" PRId16 ", %" PRId16 ", %" PRIu16 ", %" PRIu16 "]", __FUNCTION__, reg.x, reg.y,
                           reg.width, reg.height);
        auto pixmapReply = XCB::RootDisplay::copyRootImageRegion(reg, shm);

        if(! pixmapReply)
        {
            Application::error("%s: %s", __FUNCTION__, "xcb copy region empty");
            throw rfb_error(NS_FuncName);
        }

        Application::trace(DebugType::X11Srv, "%s: request size [%" PRIu16 ", %" PRIu16 "], reply: length: %lu, bits per pixel: %" PRIu8
                               ", red: %08" PRIx32 ", green: %08" PRIx32 ", blue: %08" PRIx32,
                               __FUNCTION__, reg.width, reg.height, pixmapReply->size(), pixmapReply->bitsPerPixel(), pixmapReply->rmask,
                               pixmapReply->gmask, pixmapReply->bmask);

        // fix align
        if(pixmapReply->size() != reg.width* reg.height* pixmapReply->bytePerPixel())
        {
            Application::error("%s: region not aligned, reply size: %lu, reg size: [%" PRIu16 ", %" PRIu16 "], byte per pixel: %"
                               PRIu8,
                               __FUNCTION__, pixmapReply->size(), reg.width, reg.height, pixmapReply->bytePerPixel());
            throw rfb_error(NS_FuncName);
        }

        FrameBuffer fb(pixmapReply->data(), reg, serverFormat());
        serverFrameBufferModifyEvent(fb);

        return XcbFrameBuffer{std::move(pixmapReply), std::move(fb)};
    }

    void RFB::X11Server::serverRecvSetContinuousUpdatesEvent(bool enable, const XCB::Region & reg)
    {
        const std::scoped_lock guard{ serverLock };
        clientRegion = reg;
    }
}
