// RackWindow implementation. See rackwindow.h for the painting and threading discipline.

#include "rackwindow.h"

#include "host/diagnostics.h"

#include "rack/rackgeometry.h"

#include <cairo/cairo-xlib.h>

// XK_* for the key mapping, and XLookupString's declaration.
#include <X11/Xutil.h>
#include <X11/keysym.h>

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
    //
    // KeyPressMask and FocusChangeMask are here for the preset name field and nothing else. The
    // mask alone claims no keys: X routes a key to this window only while it holds the input focus,
    // and the focus is taken only around an open field (setKeyboardFocus).
    XSelectInput(display, mWindow,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     LeaveWindowMask | StructureNotifyMask | KeyPressMask | FocusChangeMask);
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
    // Never leave the focus pointed at a window that is about to stop existing.
    setKeyboardFocus(false);

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
// Ported from the editor's X11 view, which had to answer the same question first. The mapping of
// X modifier masks onto KeyModifier is the part worth reading twice: KeyModifier documents
// kCommandKey as "Windows: ctrl key" and kControlKey as "Windows: win key", so ControlMask is
// kCommandKey here and Mod4 (Super) is kControlKey. The other way round makes Ctrl-C read as a
// plain C.
namespace
{

Steinberg::int16 virtualKeyFromKeySym(KeySym sym)
{
    switch (sym) {
        case XK_BackSpace:
            return Steinberg::KEY_BACK;
        case XK_Tab:
            return Steinberg::KEY_TAB;
        case XK_Return:
            return Steinberg::KEY_RETURN;
        case XK_KP_Enter:
            return Steinberg::KEY_ENTER;
        case XK_Escape:
            return Steinberg::KEY_ESCAPE;
        case XK_Delete:
        case XK_KP_Delete:
            return Steinberg::KEY_DELETE;
        case XK_Left:
        case XK_KP_Left:
            return Steinberg::KEY_LEFT;
        case XK_Right:
        case XK_KP_Right:
            return Steinberg::KEY_RIGHT;
        case XK_Home:
        case XK_KP_Home:
            return Steinberg::KEY_HOME;
        case XK_End:
        case XK_KP_End:
            return Steinberg::KEY_END;
        default:
            return 0;
    }
}

} // namespace

//------------------------------------------------------------------------
void RackWindow::setKeyboardFocus(bool wanted)
{
    Display *display = mLoop.display();
    if (!display || !mWindow || wanted == mKeyFocus)
        return;

    if (wanted) {
        // XSetInputFocus on a window that is not viewable is a BadMatch.
        XWindowAttributes attrs;
        if (XGetWindowAttributes(display, mWindow, &attrs) == 0 || attrs.map_state != IsViewable)
            return;
        ::Window focus = 0;
        int revert = RevertToParent;
        XGetInputFocus(display, &focus, &revert);
        mPrevFocus = focus;
        mPrevRevert = revert;
        XSetInputFocus(display, mWindow, RevertToParent, CurrentTime);
        XFlush(display);
        mKeyFocus = true;
        return;
    }

    mKeyFocus = false;
    const ::Window prev = mPrevFocus;
    mPrevFocus = 0;
    // PointerRoot and None are legal focus values in their own right and are handed back as they
    // are; a real window may have been destroyed while we held the focus, so it is probed first
    // rather than trusted. A failed probe leaves the focus here — wrong, but far better than
    // pointing it at a dead id.
    if (prev == PointerRoot || prev == None) {
        XSetInputFocus(display, prev, mPrevRevert, CurrentTime);
    } else if (prev != mWindow) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(display, prev, &attrs) != 0)
            XSetInputFocus(display, prev, mPrevRevert, CurrentTime);
    }
    XFlush(display);
}

//------------------------------------------------------------------------
void RackWindow::syncKeyboardFocus()
{
    setKeyboardFocus(mView.wantsKeyboard());
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

        case KeyPress: {
            // XLookupString applies the shift and lock state and yields the Latin-1 byte, which is
            // why the character comes from it rather than from the keysym by hand. One byte is
            // asked for because the field is ASCII only; a longer answer is a multi-byte character
            // the field cannot store.
            XKeyEvent ke = event.xkey;
            char text[8] = {0};
            KeySym sym = NoSymbol;
            const int n = XLookupString(&ke, text, sizeof(text) - 1, &sym, nullptr);
            const unsigned char byte = (n >= 1) ? static_cast<unsigned char>(text[0]) : 0;
            const Steinberg::char16 ch =
                (byte >= 0x20 && byte < 0x7F) ? static_cast<Steinberg::char16>(byte) : 0;

            Steinberg::int16 mods = 0;
            if (ke.state & ShiftMask)
                mods |= Steinberg::kShiftKey;
            if (ke.state & ControlMask)
                mods |= Steinberg::kCommandKey;
            if (ke.state & Mod1Mask)
                mods |= Steinberg::kAlternateKey;
            if (ke.state & Mod4Mask)
                mods |= Steinberg::kControlKey;

            const NAMp::rack::RackAction action = mView.key(ch, virtualKeyFromKeySym(sym), mods);
            if (applyAction(action) || action.kind != NAMp::rack::RackAction::Kind::NoAction)
                mDirty = true;
            // After, not before: the key that closed the field is what releases the keyboard.
            syncKeyboardFocus();
            return;
        }

        case FocusOut:
            // The focus can be taken away by the window manager at any moment. Let the flag follow
            // reality, or a later release would hand focus somewhere it no longer is and steal it
            // from whoever holds it now.
            mKeyFocus = false;
            mPrevFocus = 0;
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
            // A click is what opens the name field and what dismisses it, so this is the other end
            // of the keyboard contract: taken here when a field appeared, handed back here when one
            // went away.
            syncKeyboardFocus();
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
            const std::string name = action.text.empty() ? mPresetName : action.text;
            if (name.empty())
                return false;
            if (mSavePreset)
                mSavePreset(name);
            // Saving under a new name makes that rack the one being worked on, which is what Save
            // As means everywhere else. Without it the list would show the new preset while the
            // save row still offered the OLD name, so the next save would quietly go somewhere the
            // user had just moved away from.
            mPresetName = name;
            mModel.setPresetName(mPresetName);
            mDirty = true;
            return false;
        }

        case Kind::DeletePreset: {
            if (action.text.empty())
                return false;
            if (mDeletePreset)
                mDeletePreset(action.text);
            // The name is deliberately NOT cleared when the rack that was deleted is the one
            // loaded. What is playing is still that rack; the file is what went. Leaving the name
            // means the save row still offers it, so a deletion made by mistake is undone by
            // pressing save — which is the only undo this has.
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
