// RunLoop — the host side of Linux::IRunLoop, and the window's size policy.
//
// On Linux a VST3 plug-in owns no event loop; the host must provide one, and hand it to the
// plug-in through IPlugFrame::queryInterface. That is exactly what the editor asks for
// (src/platform/x11plugview.cpp, attachedToParent), so the standalone has to implement it to show
// its own editor at all.
//
// This object is therefore both:
//   * Linux::IRunLoop  — the plug-in registers file descriptors and timers;
//   * IPlugFrame       — what the host passes to IPlugView::setFrame, and the object the plug-in
//                        queries the run loop from.
//
// The two are DIFFERENT SCOPES and are one object here only because this standalone hosts exactly
// one view. Linux::IRunLoop is per PROCESS — it is the event loop, and there is one. IPlugFrame is
// per EDITOR. The moment a second editor exists (a hosted plug-in's own panel), the frame has to
// split off into an object per view while the loop stays single, and this class is the loop half.
//
// The loop itself is a select() over the host's X connection plus every descriptor the plug-in
// registered, with the timeout set from the nearest due timer. Registrations are allowed to change
// while a callback is running (a plug-in unregisters its handler from inside removed()), so the
// iteration works on copies.
//
// IT ALSO OWNS THE WINDOW'S SIZE, and the policy it enforces is the project's own rather than
// anything the SDK asks for: WIDTH BELONGS TO THE WINDOW, HEIGHT BELONGS TO THE PAGE.
//
// The editor is host-resizable and a page change reshapes the window through
// IPlugFrame::resizeView, so refusing the request outright would leave the page button
// dead-ended — that is what the sibling standalone this file was ported from does, and its editor
// is fixed-size. This one grants the request, but grants HEIGHT ONLY and pins the width to
// whatever the window already had. The reason is that the top-level window is not the editor's
// alone: a strip listing the hosted plug-in chain sits directly below it as a sibling child
// window, sharing the same width, and a page change that narrowed the window would drag that strip
// sideways and back. So the width is set once, when the window is created, and a page change moves
// the bottom edge and nothing else.
//
// THAT IS THIS HOST'S POLICY AND NOT A PROPERTY OF THE EDITOR. A DAW may grant any size it likes,
// including a width the editor never asked for, and the editor's own letterbox fallback handles
// that unchanged. Nothing here is baked into the drawing code.
//
// Reference: the SDK's own editorhost sample implements the same interfaces at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class RunLoop : public Steinberg::IPlugFrame, public Steinberg::Linux::IRunLoop
{
public:
    using XEventCallback = std::function<void(const XEvent &)>;

    explicit RunLoop(::Display *display);

    // Called for every X event on the host's own connection.
    void setXEventCallback(XEventCallback cb)
    {
        mXCallback = std::move(cb);
    }

    // The top-level window the editor was embedded into, and the view inside it. Set once, after
    // the view has attached: resizeView cannot do its job without both, and until it is called a
    // resize request is refused rather than acted on half-way.
    void setEmbedding(::Window window, Steinberg::IPlugView *view);

    // The window manager has resized the top-level. Runs the new size through the view's own
    // constraint and tells the view about it. A size we ourselves just applied is ignored, which
    // is what keeps this and resizeView from resizing each other in a loop.
    void windowConfigured(int w, int h);

    void run();

    // Safe to call from a signal handler, which is what the standalone does on SIGINT and SIGTERM.
    // That is why mRunning is an atomic and not a plain bool: a handler may portably touch only a
    // volatile sig_atomic_t or a lock-free atomic, and both parents this file was ported from
    // write a plain bool here. std::atomic<bool> is lock-free on every target this builds for, and
    // the assertion beside the member is what would catch a target where it is not.
    void stop()
    {
        mRunning.store(false, std::memory_order_relaxed);
    }

    // The width every page is shown at, set once when the window is created and never changed by
    // a page change. Zero until setEmbedding() runs, which is what makes resizeView fall back to
    // granting whatever it was asked for before there is a window to have a width.
    int lockedWidth() const
    {
        return mLockedW;
    }

    //---from IPlugFrame--------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) SMTG_OVERRIDE;

    //---from Linux::IRunLoop---------
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                                       Steinberg::Linux::FileDescriptor fd)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                                Steinberg::Linux::TimerInterval ms) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler)
        SMTG_OVERRIDE;

    //---from FUnknown----------------
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct EventEntry {
        Steinberg::Linux::IEventHandler *handler;
        int fd;
    };
    struct TimerEntry {
        Steinberg::Linux::ITimerHandler *handler;
        std::chrono::milliseconds interval;
        Clock::time_point next;
    };

    // Milliseconds until the soonest timer is due (0 if one is already due, -1 if there are no
    // timers at all).
    int fireDueTimersAndGetTimeout();

    // Inert unless RATIONS_STANDALONE_TRACE is set. The same env-var idiom as the editor's own
    // RATIONS_X11_TRACE, and here for the same reason: whether a window manager honours a resize
    // is a question that has to be answerable from a user's machine rather than from this one.
    void trace(const char *fmt, ...) const;

    // Resize the X window and remember the size, so the ConfigureNotify it provokes is recognised
    // as ours rather than treated as the user dragging the frame.
    void applySize(int w, int h);
    // Tell the window manager the smallest size the CURRENT page can be drawn at. The view is the
    // authority on that: checkSizeConstraint clamps whatever it is given up to the page's own
    // floor, and that floor differs per page — the page that scrolls has a much shorter one,
    // because its window is allowed to be shorter than the page it shows.
    void updateSizeHints();

    ::Display *mDisplay = nullptr;
    ::Window mWindow = 0;
    Steinberg::IPlugView *mView = nullptr;
    int mAppliedW = 0;
    int mAppliedH = 0;
    // The one width, per the policy at the top of this file. Taken from the window at
    // setEmbedding() time and thereafter changed only by the user dragging the frame — never by a
    // page change, which is the whole point.
    int mLockedW = 0;

    XEventCallback mXCallback;
    std::vector<EventEntry> mEventHandlers;
    std::vector<TimerEntry> mTimers;
    std::atomic<bool> mRunning{false};
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "stop() is called from a signal handler, which may only touch a lock-free "
                  "atomic or a volatile sig_atomic_t");
    bool mTrace = false;
};

} // namespace Rations
