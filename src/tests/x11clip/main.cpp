#include <chrono>
#include <thread>
#include <fstream>
#include <cstring>
#include <iostream>
#include <exception>
#include <filesystem>

#include "ltsm_tools.h"
#include "ltsm_xcb_wrapper.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace std::chrono_literals;

class X11Clip : public Application, public XCB::RootDisplay
{
public:
    X11Clip(int display) : Application("x11clip"), XCB::RootDisplay(display)
    {
	Application::info("DisplayInfo: width: %u, height: %u, depth: %u, maxreq: %u", width(), height(), depth(), getMaxRequest());
        XCB::RootDisplay::extensionDisable(XCB::Module::DAMAGE);
    }

    virtual int start(void) = 0;
};

class X11ClipCopy : public X11Clip, public XCB::SelectionRecipient
{
    XCB::ModuleCopySelection* copy = nullptr;
    xcb_atom_t target = XCB_ATOM_STRING;
    xcb_atom_t targets = XCB_ATOM_NONE;
    std::filesystem::path file;
    
protected:
    void selectionReceiveData(xcb_atom_t atom, const uint8_t* ptr, uint32_t len) const override
    {
        auto name = getAtomName(atom);
        Application::info("%s: atom: `%s', size: %u", __FUNCTION__, name.data(), len);

        if(! file.empty())
        {
            Tools::binaryToFile(ptr, len, file);
        }
    }

    void selectionReceiveTargets(const xcb_atom_t* beg, const xcb_atom_t* end) const override
    {
        std::for_each(beg, end, [&](auto & atom)
        {
            Application::info("%s: target: `%s'", "selectionReceiveTargets", getAtomName(atom).data());

            if(atom == target)
                copy->convertSelection(target, *this);
        });
    }

    void selectionChangedEvent(void) const override
    {
        Application::info("%s", __FUNCTION__);
        copy->convertSelection(targets, *this);
    }

public:
    X11ClipCopy(int argc, const char** argv) : X11Clip(-1)
    {
	if(2 < argc)
	{
	    target = getAtom(argv[2], true);

            if(3 < argc)
                file = argv[3];
	}

	targets = getAtom("TARGETS", true);
	copy = static_cast<XCB::ModuleCopySelection*>(getExtension(XCB::Module::SELECTION_COPY));

	Application::info("mode: %s, target: `%s', data save: `%s'", "copy", getAtomName(target).data(), file.c_str());
    }

    int start(void) override
    {
        Tools::TimePoint getsel(std::chrono::seconds(3));

        copy->convertSelection(targets, *this);

        while(! hasError())
        {
            while(auto ev = pollEvent())
            {
            }

            if(getsel.check())
            {
            }

	    std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return EXIT_SUCCESS;
    }
};

class X11ClipPaste : public X11Clip, public XCB::SelectionSource
{
    XCB::ModulePasteSelection* paste = nullptr;
    xcb_atom_t target = XCB_ATOM_NONE;

    std::vector<uint8_t> buf;

protected:
    std::vector<xcb_atom_t> selectionSourceTargets(void) const override
    {
        return { target };
    }

    size_t selectionSourceSize(xcb_atom_t atom) const override
    {
        Application::info("%s, atom: `%s'", __FUNCTION__, getAtomName(atom).data());

        if(atom == target)
        {
            return buf.size();
        }

        return 0;
    }

    std::vector<uint8_t> selectionSourceData(xcb_atom_t atom, size_t offset, uint32_t length) const override
    {
        Application::info("%s, atom: `%s', offset: %u, length: %u", __FUNCTION__, getAtomName(atom).data(), offset, length);

        if(atom == target)
        {
            if(offset + length <= buf.size())
            {
                return std::vector<uint8_t>(buf.begin() + offset, buf.begin() + offset + length);
            }
            else
            {
                Application::error("invalid length: %u, offset: %u", length, offset);
            }
        }

        return {};
    }

public:
    X11ClipPaste(int argc, const char** argv) : X11Clip(-1)
    {
        std::string_view test{"0123456789"};

        if(2 < argc)
	{
	    target = getAtom(argv[2], true);

            if(3 < argc)
                buf = Tools::fileToBinaryBuf(argv[3]);
	}

        if(target == XCB_ATOM_NONE)
            target = XCB_ATOM_STRING;

        if(buf.empty())
            buf.assign(test.begin(), test.end());

	Application::info("mode: %s, target: `%s', data size: %u", "paste", getAtomName(target).data(), buf.size());
	paste = static_cast<XCB::ModulePasteSelection*>(getExtension(XCB::Module::SELECTION_PASTE));
    }

    int start(void) override
    {
        Tools::TimePoint getsel(std::chrono::seconds(3));

        auto thr = std::thread([this]()
        {
            while(! this->hasError())
            {
                while(auto ev = this->pollEvent())
                {
                }

	        std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        paste->setSelectionOwner(*this);

        thr.join();
        return EXIT_SUCCESS;
    }
};

int main(int argc, const char** argv)
{
    try
    {
        Application::setDebugLevel(DebugLevel::Info);

	if(1 < argc)
        {
	    std::unique_ptr<X11Clip> app;

            if(0 == strcmp(argv[1], "copy"))
	        app = std::make_unique<X11ClipCopy>(argc, argv);
            else
            if(0 == strcmp(argv[1], "paste"))
	        app = std::make_unique<X11ClipPaste>(argc, argv);

            if(app)
                return app->start();
	}

        std::cout << "usage: " << argv[0] << " <copy|paste> <target atom> <file>" << std::endl;
        return EXIT_SUCCESS;
    }
    catch(const std::exception & err)
    {
        Application::error("exception: %s", err.what());
    }

    return EXIT_FAILURE;
}
