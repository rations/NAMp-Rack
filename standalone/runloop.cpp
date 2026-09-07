// RunLoop implementation. See runloop.h.

#include "runloop.h"

#include <X11/Xutil.h>
#include <sys/select.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
RunLoop::RunLoop(::Display *display) : mDisplay(display)
{
    mTrace = std::getenv("NAMPRACK_STANDALONE_TRACE") != nullptr;
}

//------------------------------------------------------------------------
void RunLoop::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    va_list args;
    va_start(args, fmt);
    fputs("namp-rack: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid)) {
        *obj = static_cast<Linux::IRunLoop *>(this);
        addRef();
        return kResultOk;
    }
    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        addRef();
        return kResultOk;
    }

    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
void RunLoop::setEmbedding(::Window window, IPlugView *view)
{
    mWindow = window;
    mView = view;

    ViewRect current = {};
    if (mView && mView->getSize(&current) == kResultTrue) {
        mAppliedW = current.getWidth();
        mAppliedH = current.getHeight();
    }
    // The width the window will keep. Taken from the view's OWN opening size rather than from a
    // constant here, so this file still knows nothing about pages: whatever the editor comes up
    // at is what the window is, and every later page change is height-only.
    mLockedW = mAppliedW;
    updateSizeHints();
}

//------------------------------------------------------------------------
void RunLoop::applySize(int w, int h)
{
    if (!mDisplay || !mWindow || w <= 0 || h <= 0)
        return;
    // Recorded BEFORE the request, because the ConfigureNotify it provokes may be dispatched
    // before we return here and must already be recognisable as ours.
    mAppliedW = w;
    mAppliedH = h;
    XResizeWindow(mDisplay, mWindow, static_cast<unsigned>(w), static_cast<unsigned>(h));
    XFlush(mDisplay);
}

//------------------------------------------------------------------------
void RunLoop::updateSizeHints()
{
    if (!mDisplay || !mWindow || !mView)
        return;

    // Ask the view rather than deciding here: this file knows nothing about pages, and does not
    // need to. checkSizeConstraint clamps whatever it is given into what the CURRENT page can be
    // drawn at, so a 1x1 rect comes back as that page's floor and an absurdly large one as its
    // ceiling.
    int minW = 1, minH = 1, maxW = 0, maxH = 0;
    ViewRect small(0, 0, 1, 1);
    if (mView->checkSizeConstraint(&small) == kResultTrue) {
        minW = small.getWidth();
        minH = small.getHeight();
    }
    ViewRect large(0, 0, 1 << 15, 1 << 15);
    if (mView->checkSizeConstraint(&large) == kResultTrue) {
        maxW = large.getWidth();
        maxH = large.getHeight();
    }

    XSizeHints hints = {};
    hints.flags = PMinSize;
    hints.min_width = minW;
    hints.min_height = minH;
    if (maxW >= minW && maxH >= minH) {
        hints.flags |= PMaxSize;
        hints.max_width = maxW;
        hints.max_height = maxH;
    }
    // A view that cannot be resized is pinned at the size it has, which is what a host would do
    // with canResize() == kResultFalse.
    if (mView->canResize() != kResultTrue && mAppliedW > 0 && mAppliedH > 0) {
        hints.flags |= PMinSize | PMaxSize;
        hints.min_width = hints.max_width = mAppliedW;
        hints.min_height = hints.max_height = mAppliedH;
    }
    XSetWMNormalHints(mDisplay, mWindow, &hints);
    trace("hints: %d..%d wide, %d..%d tall", hints.min_width, hints.max_width, hints.min_height,
          hints.max_height);
}

//------------------------------------------------------------------------
// The plug-in asking for a different window, which here means a page change: the two pages are two
// canvases and the editor calls this on both of them.
//
// HEIGHT IS GRANTED, WIDTH IS PINNED. See the policy at the top of runloop.h — the top-level
// window is shared with a strip below the editor, so its width is set once and a page change moves
// only the bottom edge. The view is then told the size it really got, not the size it asked for,
// which is what a host does with any request it cannot grant exactly.
//
// In practice the two agree: both pages are the same number of logical units wide, so at a given
// scale the editor asks for the width it already has and the pin changes nothing. That is the
// point. The pin is what makes it stay true — of a page added later, of a scale that rounds a
// pixel differently, and of the editor's own letterbox fallback, none of which this file wants to
// have to reason about.
//
// The SDK's sequence (pluginterfaces/gui/iplugview.h): the plug-in calls resizeView, the host
// resizes the window, and the host calls back into onSize IN THE SAME CALLSTACK. Doing it in that
// order matters — the editor's onSize re-enters its own constrainSize before this returns.
tresult PLUGIN_API RunLoop::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize)
        return kInvalidArgument;
    // Before the editor has attached there is no window to resize, and saying kResultTrue would
    // leave the view believing in a size nothing is drawn at.
    if (view != mView || !mWindow)
        return kResultFalse;

    const int wanted = newSize->getWidth();
    const int h = newSize->getHeight();
    if (wanted <= 0 || h <= 0)
        return kResultFalse;

    const int w = mLockedW > 0 ? mLockedW : wanted;
    if (w != wanted)
        trace("resizeView: the editor asked for %dx%d; width is pinned to %d", wanted, h, w);
    else
        trace("resizeView: the editor asked for %dx%d (we were at %dx%d)", w, h, mAppliedW,
              mAppliedH);

    // HINTS FIRST, THEN THE RESIZE, and the order is not cosmetic. The window still carries the
    // OUTGOING page's minimum, and a page change can ask for a window outside it — in height now
    // rather than in width, since the width no longer moves. A window manager that honours
    // PMinSize, which most do, clamps the request back and the editor is handed a size nobody
    // asked for. Measured that way round first in the parent project, before the width was
    // shared: the editor asked for 640x524 and got 748x524. Publishing the incoming page's
    // minimum before the request is what makes the request grantable.
    updateSizeHints();
    applySize(w, h);

    // The view is told what it GOT. Writing the granted width back into the caller's rect is not
    // optional: the editor reads this rect after the call returns, and leaving the width it asked
    // for in there would have it lay out for a window that does not exist.
    newSize->right = newSize->left + w;
    mView->onSize(newSize);
    return kResultTrue;
}

//------------------------------------------------------------------------
// The window manager telling us the window is now some size - the user dragging its frame, or a
// step on the way to a size we asked for ourselves.
//
// WHATEVER IT SAYS IS ACCEPTED, and the view is told. It is NOT pushed back on, and the first
// version of this file was wrong to: a resize we requested arrives as more than one configure on
// a reparenting window manager, so re-resizing to the constrained size turned an intermediate
// step into a new request and the two sides then argued. Measured - a page change back to the
// head page went out as 1133x403, an intermediate 748x460 came back, this function "corrected" it
// to 748x266, and the window stuck there while every later page change was overridden.
//
// A host does not negotiate this way either. It resizes its window and tells the view; the view's
// own onSize constrains what it DRAWS, centring the page when the window is not the shape the page
// wants. That is the same degradation the editor falls back to under a host that offers no
// IPlugFrame at all, where it cannot ask for a size and has to live in whatever it is given — so
// it is a path that is exercised rather than a theoretical one. What keeps a window manager from
// offering a size the page cannot use is the size hints above, which is the mechanism X11 has for
// exactly that.
//
// THIS IS ALSO WHERE THE WIDTH LOCK IS RE-TAKEN. Pinning the width across a page change does not
// mean the window cannot be resized: the user dragging the frame is the one thing that legitimately
// changes it, and whatever they drag it to becomes the new locked width. The distinction is between
// the user asking for a width and the EDITOR asking for one — the first is granted, the second is
// what would move the strip below.
void RunLoop::windowConfigured(int w, int h)
{
    if (!mView || w <= 0 || h <= 0)
        return;
    if (w == mAppliedW && h == mAppliedH) {
        trace("configure: %dx%d, which is the size we are already at - ignored", w, h);
        return;
    }

    mAppliedW = w;
    mAppliedH = h;
    mLockedW = w;
    trace("configure: the window is now %dx%d", w, h);

    ViewRect actual(0, 0, w, h);
    mView->onSize(&actual);
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::registerEventHandler(Linux::IEventHandler *handler,
                                                 Linux::FileDescriptor fd)
{
    if (!handler || fd < 0)
        return kInvalidArgument;
    mEventHandlers.push_back({handler, fd});
    return kResultTrue;
}

tresult PLUGIN_API RunLoop::unregisterEventHandler(Linux::IEventHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mEventHandlers.size();
    mEventHandlers.erase(
        std::remove_if(mEventHandlers.begin(), mEventHandlers.end(),
                       [handler](const EventEntry &e) { return e.handler == handler; }),
        mEventHandlers.end());
    return mEventHandlers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RunLoop::registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval ms)
{
    if (!handler || ms == 0)
        return kInvalidArgument;
    const std::chrono::milliseconds interval(ms);
    mTimers.push_back({handler, interval, Clock::now() + interval});
    return kResultTrue;
}

tresult PLUGIN_API RunLoop::unregisterTimer(Linux::ITimerHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mTimers.size();
    mTimers.erase(std::remove_if(mTimers.begin(), mTimers.end(),
                                 [handler](const TimerEntry &t) { return t.handler == handler; }),
                  mTimers.end());
    return mTimers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
int RunLoop::fireDueTimersAndGetTimeout()
{
    if (mTimers.empty())
        return -1;

    const Clock::time_point now = Clock::now();

    // Fire from a snapshot: a handler may register or unregister timers, which would otherwise
    // invalidate the iteration.
    std::vector<Linux::ITimerHandler *> due;
    for (TimerEntry &t : mTimers) {
        if (t.next <= now) {
            due.push_back(t.handler);
            // Skip missed firings rather than trying to catch up in a burst.
            t.next = now + t.interval;
        }
    }
    for (Linux::ITimerHandler *handler : due) {
        // The handler may have been unregistered by an earlier callback.
        const bool live =
            std::any_of(mTimers.begin(), mTimers.end(),
                        [handler](const TimerEntry &t) { return t.handler == handler; });
        if (live)
            handler->onTimer();
    }

    if (mTimers.empty())
        return -1;

    Clock::time_point soonest = mTimers.front().next;
    for (const TimerEntry &t : mTimers)
        soonest = std::min(soonest, t.next);

    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(soonest - Clock::now()).count();
    return delta < 0 ? 0 : static_cast<int>(delta);
}

//------------------------------------------------------------------------
void RunLoop::run()
{
    mRunning.store(true, std::memory_order_relaxed);
    const int xFd = mDisplay ? ConnectionNumber(mDisplay) : -1;

    while (mRunning.load(std::memory_order_relaxed)) {
        // Anything already queued on our own connection is handled first: select() would not
        // report the fd as readable for events Xlib has already buffered.
        if (mDisplay) {
            while (XPending(mDisplay)) {
                XEvent event;
                XNextEvent(mDisplay, &event);
                if (mXCallback)
                    mXCallback(event);
                if (!mRunning.load(std::memory_order_relaxed))
                    return;
            }
            XFlush(mDisplay);
        }

        const int timeoutMs = fireDueTimersAndGetTimeout();
        if (!mRunning.load(std::memory_order_relaxed))
            return;

        fd_set readSet;
        FD_ZERO(&readSet);
        int maxFd = -1;
        if (xFd >= 0) {
            FD_SET(xFd, &readSet);
            maxFd = xFd;
        }
        const std::vector<EventEntry> handlers = mEventHandlers; // snapshot
        for (const EventEntry &e : handlers) {
            FD_SET(e.fd, &readSet);
            maxFd = std::max(maxFd, e.fd);
        }
        if (maxFd < 0)
            break; // nothing left to wait on

        timeval tv;
        timeval *tvp = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvp = &tv;
        }

        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, tvp);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue; // timeout: loop round and fire the timers

        for (const EventEntry &e : handlers) {
            if (!FD_ISSET(e.fd, &readSet))
                continue;
            // Still registered? A previous callback may have removed it.
            const bool live =
                std::any_of(mEventHandlers.begin(), mEventHandlers.end(),
                            [&e](const EventEntry &c) { return c.handler == e.handler; });
            if (live)
                e.handler->onFDIsSet(e.fd);
            if (!mRunning.load(std::memory_order_relaxed))
                return;
        }
    }
}

} // namespace Rations
