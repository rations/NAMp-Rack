// PluginWindow — one top-level X window holding one hosted plug-in's editor.
//
// Every hosted plug-in gets its own window rather than sharing one: a shared window would have to
// tear an editor down and build another every time the user switched plug-in, and a plug-in editor
// is the single most expensive and most fragile thing to create and destroy repeatedly. Separate
// windows also let the window manager do the work — stacking, iconifying, remembering positions —
// instead of this host reimplementing it.
//
// LIFETIME. close() is not destruction. It tears the editor and the X window down and leaves this
// object alive and reusable, so open()/close() can cycle any number of times. That is not just
// tidiness: close() is reached from inside the X dispatch when the user clicks the title bar's
// close button, and destroying the object there would pull the ground out from under the callback
// that is running. The object is destroyed later, by whoever owns it.
//
// TEARDOWN ORDER IS LOAD-BEARING and is the reason this is a class rather than two loose functions:
//
//   1. backend.editorClose()  — the view unregisters its own run-loop event handler and timers from
//                               inside IPlugView::removed(), so this must happen while the run loop
//                               and the frame are both still alive;
//   2. loop.removeWindow()    — no further X events are dispatched to a window that is going away;
//   3. XDestroyWindow()       — only now, once nothing can still be drawing into it.
//
// Getting 1 and 3 the wrong way round leaves a plug-in's timer firing against a destroyed window,
// which is a crash inside the plug-in with our stack nowhere in the backtrace.
//
// NOT EVERY EDITOR NEEDS A WINDOW FROM US. An LV2 ui:showInterface UI opens and owns its own
// top-level window; the host's whole job there is to call show(), drive idle() and call hide().
// This class therefore has two modes, and `mOwnsWindow` is which one — creating an X window for a
// showInterface UI would leave an empty stray frame on screen next to the real editor.
//
// This file includes both <X11/Xlib.h> and the SDK's headers. Xlib defines None, True and False as
// macros, which is why the editor-kind enumerator in the host layer is spelled NoEditor.

#pragma once

#include "eventloop.h"
#include "plugframe.h"

#include "host/pluginbackend.h"

#include <X11/Xlib.h>

#include <cstdint>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class PluginWindow
{
public:
    PluginWindow(EventLoop &loop, NAMp::host::PluginBackend &backend);
    ~PluginWindow();

    PluginWindow(const PluginWindow &) = delete;
    PluginWindow &operator=(const PluginWindow &) = delete;

    // Creates the window, opens the editor into it and maps it. Reopening an already-open window
    // just raises it. False means the plug-in has no embeddable editor — the caller falls back to
    // the generic panel, which is P4's job — or that there is no display.
    bool open();
    void close();

    bool isOpen() const
    {
        return mOpen;
    }

    // Called from the UI timer: drives formats that need an idle callback and applies any resize
    // the editor latched from a thread we do not control.
    void idle();

    NAMp::host::PluginBackend &backend() const
    {
        return mBackend;
    }

private:
    void onXEvent(const XEvent &event);
    // The IPlugFrame half of the SDK resize contract: resize the window, then onSize() in the same
    // callstack.
    bool onViewResize(Steinberg::IPlugView *view, Steinberg::ViewRect *rect);
    // Ask the editor what it will accept nearest to w x h.
    void constrain(int32_t &w, int32_t &h) const;
    void applySizeHints(int32_t w, int32_t h);

    EventLoop &mLoop;
    NAMp::host::PluginBackend &mBackend;
    PlugFrame mFrame;

    ::Window mWindow = 0;
    // The window the plug-in created INSIDE ours (LV2 ui:X11UI). suil hands it back as the widget;
    // it is the plug-in's to draw and ours to keep the same size as its parent, because an X child
    // does not follow its parent's size on its own. VST3 leaves this zero: an IPlugView manages its
    // own child through onSize().
    ::Window mChild = 0;
    // False when the editor owns its own top-level window; see the note at the top of this file.
    bool mOwnsWindow = true;
    bool mOpen = false;
    Atom mWmDelete = 0;
    int32_t mWidth = 0;
    int32_t mHeight = 0;
    bool mResizable = false;
    std::string mTitle;
};

} // namespace Rations
