// RackWindow implementation. See rackwindow.h for the painting and threading discipline.

#include "rackwindow.h"

#include "host/diagnostics.h"

#include "rack/rackgeometry.h"

#include <cairo/cairo-xlib.h>

#include <cstdio>

namespace Rations
{

namespace
{

// X button numbers. 4 and 5 are the wheel, which X reports as button presses.
constexpr int kButtonLeft = 1;
constexpr int kButtonRight = 3;
constexpr int kButtonWheelUp = 4;
constexpr int kButtonWheelDown = 5;

} // namespace

//------------------------------------------------------------------------
RackWindow::RackWindow(EventLoop &loop, NAMp::host::ChainBuilder &builder)
    : mLoop(loop), mBuilder(builder)
{
    mModel.setBuilder(&builder);
    mView.setModel(&mModel);
    // The one place $NAMP_DIAG is consulted for the strip. See RackModel::setDiagArmed.
    mModel.setDiagArmed(NAMp::host::diagArmed());
}

//------------------------------------------------------------------------
RackWindow::~RackWindow()
{
    destroy();
}

//------------------------------------------------------------------------
void RackWindow::loadFonts(const std::string &resourceDir)
{
    // An empty directory is the normal case in a single-file build and is NOT a failure: FontStack
    // falls back to the faces linked into this binary (src/gfx/resourcestore.h). Only a face that
    // ends up as a generic system font is worth a word.
    if (!mFonts.load(resourceDir))
        fprintf(stderr, "namp-standalone: the rack fell back to generic fonts\n");
}

//------------------------------------------------------------------------
bool RackWindow::create(::Window parent, int x, int y, int w, int h)
{
    ::Display *display = mLoop.display();
    if (!display || !parent || w <= 0 || h <= 0)
        return false;
    if (mWindow)
        return true;

    const int screen = DefaultScreen(display);
    mWindow = XCreateSimpleWindow(display, parent, x, y, static_cast<unsigned>(w),
                                  static_cast<unsigned>(h), 0, BlackPixel(display, screen),
                                  BlackPixel(display, screen));
    if (!mWindow)
        return false;

    // Unlike the top-level, this window wants input: the rack is the one part of the interface the
    // standalone draws and handles itself.
    XSelectInput(display, mWindow,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     LeaveWindowMask | StructureNotifyMask);
    mLoop.addWindow(mWindow, [this](const XEvent &event) { onXEvent(event); });

    Visual *visual = DefaultVisual(display, screen);
    mTarget = cairo_xlib_surface_create(display, mWindow, visual, w, h);
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "namp-standalone: cannot create the rack's drawing surface\n");
        destroy();
        return false;
    }
    mWidth = w;
    mHeight = h;
    if (!resizeSurfaces(w, h)) {
        destroy();
        return false;
    }

    XMapWindow(display, mWindow);
    XFlush(display);
    mDirty = true;
    return true;
}

//------------------------------------------------------------------------
void RackWindow::destroy()
{
    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mWindow) {
        mLoop.removeWindow(mWindow);
        if (::Display *display = mLoop.display()) {
            XDestroyWindow(display, mWindow);
            XFlush(display);
        }
        mWindow = 0;
    }
}

//------------------------------------------------------------------------
// The xlib surface wraps a window we resized, so it is told its new size in place; the offscreen
// buffer has a fixed allocation and has to be rebuilt. Same split as the plug-in's own view.
bool RackWindow::resizeSurfaces(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;

    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, w, h);

    if (mBuffer)
        cairo_surface_destroy(mBuffer);
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "namp-standalone: cannot create the rack's %dx%d buffer\n", w, h);
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void RackWindow::setGeometry(int x, int y, int w, int h, double scale)
{
    ::Display *display = mLoop.display();
    if (!display || !mWindow || w <= 0 || h <= 0)
        return;

    mScale = scale > 0.0 ? scale : 1.0;
    XMoveResizeWindow(display, mWindow, x, y, static_cast<unsigned>(w), static_cast<unsigned>(h));

    if (w != mWidth || h != mHeight) {
        mWidth = w;
        mHeight = h;
        resizeSurfaces(w, h);
    }
    mDirty = true;
}

//------------------------------------------------------------------------
void RackWindow::setEditorOpen(uint64_t nodeId, bool open)
{
    mModel.setEditorOpen(nodeId, open);
    mDirty = true;
}

//------------------------------------------------------------------------
uint64_t RackWindow::nodeIdAt(NAMp::host::ChainSection section, int index) const
{
    NAMp::host::ChainNodeInfo info;
    return mBuilder.nodeInfo(section, index, info) ? info.id : 0;
}

//------------------------------------------------------------------------
void RackWindow::onTimer()
{
    // A requested scan runs here, on the tick, and not where the click was handled — see
    // requestScan(). It paints its own progress while it runs, which is legal from here.
    if (mScanPending) {
        mScanPending = false;
        if (mScanPlugins)
            mScanPlugins();
    }
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void RackWindow::redraw()
{
    if (!mBuffer || !mTarget)
        return;
    mDirty = false;

    // Compose offscreen...
    cairo_t *cr = cairo_create(mBuffer);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        // The single scale the whole design rests on. Everything below is in logical units.
        cairo_scale(cr, mScale, mScale);
        Canvas canvas(cr, &mFonts, NAMp::rackgeo::kRackW, NAMp::rackgeo::kRackH);
        mView.draw(canvas);
    }
    cairo_destroy(cr);

    // ...then blit in one operation, so no partially drawn frame is ever visible.
    cairo_t *out = cairo_create(mTarget);
    if (cairo_status(out) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(out, mBuffer, 0.0, 0.0);
        cairo_paint(out);
    }
    cairo_destroy(out);

    cairo_surface_flush(mTarget);
    if (::Display *display = mLoop.display())
        XFlush(display);
}

//------------------------------------------------------------------------
// Progress for a running scan.
//
// A scan is synchronous: it walks every bundle and spawns a helper process for each one that
// changed, and the run loop does not tick again until it returns. Painting on the next tick would
// therefore mean painting when it is over, which is exactly when nobody needs to be told it is
// running — so this composes and blits immediately, the same way onTimer does.
//
// That is allowed because of where it is called from: requestScan() defers the whole scan to the
// timer tick, so this never runs inside an X event handler and nothing here can recurse into
// drawing at all. No X event is dispatched while it runs.
void RackWindow::showScanProgress(int index, int total, const std::string &current)
{
    NAMp::rack::ScanState &scan = mModel.scan();
    scan.running = true;
    scan.index = index;
    scan.total = total;
    scan.current = current;
    mDirty = true;
    redraw();
}

void RackWindow::endScanProgress()
{
    mModel.scan() = NAMp::rack::ScanState();
    mDirty = true;
}

//------------------------------------------------------------------------
void RackWindow::onXEvent(const XEvent &event)
{
    // Nothing here paints. Every branch either updates state or asks for a repaint on the next
    // tick — see the discipline note in the header.
    const double scale = mScale > 0.0 ? mScale : 1.0;

    switch (event.type) {
        case Expose:
            mDirty = true;
            return;

        case ConfigureNotify:
            if (event.xconfigure.width != mWidth || event.xconfigure.height != mHeight) {
                mWidth = event.xconfigure.width;
                mHeight = event.xconfigure.height;
                resizeSurfaces(mWidth, mHeight);
            }
            mDirty = true;
            return;

        case LeaveNotify:
            // Clear the hover, or a control keeps its highlight after the pointer has gone.
            mModel.setHover(NAMp::rack::HitTarget());
            mDirty = true;
            return;

        case MotionNotify: {
            const float x = static_cast<float>(event.xmotion.x / scale);
            const float y = static_cast<float>(event.xmotion.y / scale);
            // Motion ACTS, it does not only repaint. A drag on the wet/dry slider reports its new
            // value from here and nowhere else, so a handler that merely set the dirty flag drew
            // the drag and threw the value away.
            const NAMp::rack::RackAction action = mView.mouseMove(x, y);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }

        case ButtonPress: {
            const float x = static_cast<float>(event.xbutton.x / scale);
            const float y = static_cast<float>(event.xbutton.y / scale);
            const int button = static_cast<int>(event.xbutton.button);

            if (button == kButtonWheelUp || button == kButtonWheelDown) {
                const int delta = button == kButtonWheelUp ? 1 : -1;
                const NAMp::rack::RackAction action = mView.wheel(x, y, delta);
                if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                    mDirty = true;
                return;
            }
            if (button != kButtonLeft && button != kButtonRight)
                return;

            const NAMp::rack::RackAction action = mView.mouseDown(x, y, button);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }

        case ButtonRelease: {
            const int button = static_cast<int>(event.xbutton.button);
            if (button != kButtonLeft && button != kButtonRight)
                return;
            const float x = static_cast<float>(event.xbutton.x / scale);
            const float y = static_cast<float>(event.xbutton.y / scale);
            const NAMp::rack::RackAction action = mView.mouseUp(x, y, button);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            return;
        }

        default:
            return;
    }
}

//------------------------------------------------------------------------
bool RackWindow::applyAction(const NAMp::rack::RackAction &action)
{
    using Kind = NAMp::rack::RackAction::Kind;

    switch (action.kind) {
        case Kind::NoAction:
        case Kind::Redraw:
            return false;

        case Kind::SetViewMode:
            mModel.setViewMode(action.mode);
            mModel.setListScroll(0);
            return false;

        case Kind::ToggleEditor: {
            if (mEditorToggle)
                mEditorToggle(action.section, action.index);
            return false;
        }

        case Kind::Route: {
            mBuilder.setEnabled(action.section, action.index, action.flag);
            if (action.toIndex >= 0)
                mBuilder.move(action.section, action.index, action.toIndex);
            break;
        }

        case Kind::SetMix:
            mBuilder.setMix(action.section, action.index, action.value);
            break;

        case Kind::Remove: {
            // Any window showing this plug-in's own editor has to go FIRST: it holds a reference to
            // the backend that is about to be buried, and a later collect() would free it under the
            // window's feet.
            if (mEditorClose)
                mEditorClose(nodeIdAt(action.section, action.index));
            // The instance itself is not destroyed here. It goes to the builder's graveyard and is
            // freed by collect() once a snapshot newer than any that could name it is live on the
            // audio thread — which is the whole reason a chain can be edited while it is playing.
            mBuilder.remove(action.section, action.index);
            break;
        }

        case Kind::Add: {
            // Loading a plug-in opens a shared object and runs its static initialisers, which
            // blocks this thread — the run loop — for as long as that takes.
            //
            //   Measured in P7 and deliberately left here: 12-18 ms for a native plug-in, ~860 ms
            //   for a Windows one over Wine. See the note at the top of host/chainbuilder.h for why
            //   a worker is not the free fix it looks like. It does NOT interrupt audio either way,
            //   because the chain the audio thread is running is not touched until publish() below.
            std::string error;
            if (mBuilder.add(action.section, action.ref, error) < 0) {
                fprintf(stderr, "namp-standalone: cannot add that plug-in: %s\n", error.c_str());
                return false;
            }
            break;
        }

        case Kind::LoadPreset: {
            // Every open hosted editor first, for the same reason Remove does it: a preset load
            // removes every node, and a window holding a backend the builder is about to bury is a
            // use-after-free waiting for the next collect().
            if (mEditorClose) {
                for (const NAMp::rack::RackNode &node : mModel.nodes())
                    mEditorClose(node.id);
            }
            if (!mLoadPreset)
                return false;
            if (!mLoadPreset(action.text))
                return false;
            mPresetName = action.text;
            mModel.setPresetName(mPresetName);
            // applyRack has already published; falling through to publish() again would be
            // harmless but would spend a snapshot for nothing.
            mModel.refresh();
            mDirty = true;
            if (mChainChanged)
                mChainChanged();
            return true;
        }

        case Kind::SavePreset: {
            if (mSavePreset)
                mSavePreset(action.text.empty() ? mPresetName : action.text);
            mDirty = true;
            return false;
        }

        // Discovery. None of the three touches the chain, so none of them falls through to
        // publish(): a scan changes what COULD be added, not what is playing.
        case Kind::ScanPlugins: {
            requestScan();
            return false;
        }

        case Kind::AddSearchPath: {
            if (mAddSearchPath)
                mAddSearchPath(action.text);
            mDirty = true;
            return false;
        }

        case Kind::RemoveSearchPath: {
            if (mRemoveSearchPath)
                mRemoveSearchPath(action.text);
            mDirty = true;
            return false;
        }
    }

    mBuilder.publish();
    // Free whatever the audio thread has handed back by now. Cheap, and doing it on every edit
    // keeps the graveyard from growing across a long editing session.
    mBuilder.collect();
    // The snapshot the strip is drawn from is now stale by definition, so re-read it here rather
    // than relying on whoever called this to remember. Every caller wants the picture to follow the
    // edit, and a caller that is not the pointer — the stress driver, and later the preset
    // loader — has no X event to piggyback the refresh on.
    mModel.refresh();
    mDirty = true;
    if (mChainChanged)
        mChainChanged();
    return true;
}

} // namespace Rations
