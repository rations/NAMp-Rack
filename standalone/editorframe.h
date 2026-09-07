// EditorFrame — the IPlugFrame for the amp's own editor, and the top-level window's size policy.
//
// A view is given a frame in IPlugView::setFrame, and everything it needs from the host afterwards
// it gets through that one pointer: the run loop by queryInterface, and a resize request by
// resizeView. Both halves have to be right or the editor either never ticks or draws into the wrong
// rectangle.
//
// ONE PER VIEW, NOT ONE PER PROCESS. resizeView() must resize the window THIS view is embedded in,
// and the frame is the only context the callback gets — a shared frame would have to guess which of
// several open editors was asking. The run loop it hands back, by contrast, is deliberately the one
// shared EventLoop: there is a single X connection and a single select() for the whole process. See
// eventloop.h for why the two are separate objects at all.
//
// THIS IS THE AMP'S FRAME SPECIFICALLY, which is why it owns a window rather than taking a resize
// callback. A hosted plug-in's editor lives in a top-level of its own with no policy attached to
// its width, and gets a frame of its own when there is one to give it. The policy below belongs to
// this window and to no other.
//
// THE POLICY IS THIS PROJECT'S OWN AND NOT ANYTHING THE SDK ASKS FOR:
// WIDTH BELONGS TO THE WINDOW, HEIGHT BELONGS TO THE PAGE.
//
// The editor is host-resizable and a page change reshapes the window through
// IPlugFrame::resizeView, so refusing the request outright would leave the page button dead-ended —
// that is what the sibling standalone this file was ported from does, and its editor is fixed-size.
// This one grants the request, but grants HEIGHT ONLY and pins the width to whatever the window
// already had. The reason is that the top-level window is not the editor's alone: a strip listing
// the hosted plug-in chain sits directly below it as a sibling child window, sharing the same
// width, and a page change that narrowed the window would drag that strip sideways and back. So the
// width is set once, when the window is created, and a page change moves the bottom edge and
// nothing else.
//
// THAT IS THIS HOST'S POLICY AND NOT A PROPERTY OF THE EDITOR. A DAW may grant any size it likes,
// including a width the editor never asked for, and the editor's own letterbox fallback handles
// that unchanged. Nothing here is baked into the drawing code.
//
// Reference: the SDK's own editorhost sample implements the same interface at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "eventloop.h"

#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

namespace Rations
{

//------------------------------------------------------------------------
class EditorFrame : public Steinberg::IPlugFrame
{
public:
    explicit EditorFrame(EventLoop &loop);

    // The top-level window the editor was embedded into, and the view inside it. Set once, after
    // the view has attached: resizeView cannot do its job without both, and until it is called a
    // resize request is refused rather than acted on half-way.
    void setEmbedding(::Window window, Steinberg::IPlugView *view);

    // The window manager has resized the top-level. Runs the new size through the view's own
    // constraint and tells the view about it. A size we ourselves just applied is ignored, which
    // is what keeps this and resizeView from resizing each other in a loop.
    void windowConfigured(int w, int h);

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

    //---from FUnknown----------------
    // Lifetime is the owning window's, not the reference count's; see the note in eventloop.h.
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
    // Inert unless NAMPRACK_STANDALONE_TRACE is set. The same env-var idiom as the editor's own
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

    EventLoop &mLoop;
    ::Window mWindow = 0;
    Steinberg::IPlugView *mView = nullptr;
    int mAppliedW = 0;
    int mAppliedH = 0;
    // The one width, per the policy at the top of this file. Taken from the window at
    // setEmbedding() time and thereafter changed only by the user dragging the frame — never by a
    // page change, which is the whole point.
    int mLockedW = 0;
    bool mTrace = false;
};

} // namespace Rations
