// PanelWindow implementation. See panelwindow.h for the painting discipline.

#include "panelwindow.h"

#include "host/pluginbackend.h"

#include <cairo/cairo-xlib.h>
#include <X11/Xutil.h>

#include <cstdio>

namespace Rations
{

namespace
{

constexpr int kButtonLeft = 1;
constexpr int kButtonWheelUp = 4;
constexpr int kButtonWheelDown = 5;

// Below this the list is unreadable rather than merely cramped, and above it the rows just stretch.
constexpr int kMinWidth = 360;
constexpr int kMinHeight = 140;
constexpr int kMaxWidth = 1400;
constexpr int kMaxHeight = 1400;

} // namespace

//------------------------------------------------------------------------
PanelWindow::PanelWindow(EventLoop &loop, NAMp::host::PluginBackend &backend)
    : mLoop(loop), mBackend(backend)
{
    mTitle = std::string(backend.displayName()) + " - parameters";
    mPanel.setBackend(&backend);
}

//------------------------------------------------------------------------
PanelWindow::~PanelWindow()
{
    close();
}

//------------------------------------------------------------------------
void PanelWindow::loadFonts(const std::string &resourceDir)
{
    // As in RackWindow::loadFonts: an empty directory means "use the built-in faces", not "fail".
    if (!mFonts.load(resourceDir))
        fprintf(stderr, "namp-standalone: the parameter panel fell back to generic fonts\n");
}

//------------------------------------------------------------------------
bool PanelWindow::open()
{
    ::Display *display = mLoop.display();
    if (!display)
        return false;

    if (mWindow) {
        XRaiseWindow(display, mWindow);
        XFlush(display);
        return true;
    }

    mWidth = static_cast<int>(NAMp::rack::panelgeo::kPanelW);
    mHeight = static_cast<int>(NAMp::rack::panelgeo::kDefaultPanelH);

    const int screen = DefaultScreen(display);
    mWindow = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                                  static_cast<unsigned>(mWidth), static_cast<unsigned>(mHeight), 0,
                                  BlackPixel(display, screen), BlackPixel(display, screen));
    if (!mWindow)
        return false;

    XStoreName(display, mWindow, mTitle.c_str());
    // Unlike PluginWindow, this window wants input: we are the ones drawing it.
    XSelectInput(display, mWindow,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     LeaveWindowMask | StructureNotifyMask);

    mWmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, mWindow, &mWmDelete, 1);

    // A range rather than a fixed size: the list is the point, and a taller window shows more of
    // it. No PAspect anywhere near this — see the resize-loop note in the plan's risk list.
    XSizeHints hints = {};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = kMinWidth;
    hints.min_height = kMinHeight;
    hints.max_width = kMaxWidth;
    hints.max_height = kMaxHeight;
    XSetWMNormalHints(display, mWindow, &hints);

    mLoop.addWindow(mWindow, [this](const XEvent &event) { onXEvent(event); });

    Visual *visual = DefaultVisual(display, screen);
    mTarget = cairo_xlib_surface_create(display, mWindow, visual, mWidth, mHeight);
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS || !resizeSurfaces(mWidth, mHeight)) {
        fprintf(stderr, "namp-standalone: cannot create the parameter panel's surfaces\n");
        close();
        return false;
    }

    mPanel.setSize(static_cast<float>(mWidth), static_cast<float>(mHeight));
    XMapWindow(display, mWindow);
    XFlush(display);
    mDirty = true;
    return true;
}

//------------------------------------------------------------------------
void PanelWindow::close()
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
            XSync(display, False);
        }
        mWindow = 0;
        mWmDelete = 0;
    }
}

//------------------------------------------------------------------------
bool PanelWindow::resizeSurfaces(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;

    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, w, h);

    if (mBuffer)
        cairo_surface_destroy(mBuffer);
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void PanelWindow::idle()
{
    if (!mWindow)
        return;
    if (mPanel.poll())
        mDirty = true;
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void PanelWindow::redraw()
{
    if (!mBuffer || !mTarget)
        return;
    mDirty = false;

    cairo_t *cr = cairo_create(mBuffer);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        Canvas canvas(cr, &mFonts, static_cast<float>(mWidth), static_cast<float>(mHeight));
        mPanel.draw(canvas);
    }
    cairo_destroy(cr);

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
void PanelWindow::onXEvent(const XEvent &event)
{
    // Nothing here paints; every branch sets the dirty flag and lets the timer do it.
    switch (event.type) {
        case ClientMessage:
            if (mWmDelete != 0 && static_cast<Atom>(event.xclient.data.l[0]) == mWmDelete)
                close();
            return;

        case Expose:
            mDirty = true;
            return;

        case ConfigureNotify:
            if (event.xconfigure.width != mWidth || event.xconfigure.height != mHeight) {
                mWidth = event.xconfigure.width;
                mHeight = event.xconfigure.height;
                resizeSurfaces(mWidth, mHeight);
                mPanel.setSize(static_cast<float>(mWidth), static_cast<float>(mHeight));
            }
            mDirty = true;
            return;

        case LeaveNotify:
            if (mPanel.mouseMove(-1.0f, -1.0f))
                mDirty = true;
            return;

        case MotionNotify:
            if (mPanel.mouseMove(static_cast<float>(event.xmotion.x),
                                 static_cast<float>(event.xmotion.y)))
                mDirty = true;
            return;

        case ButtonPress: {
            const int button = static_cast<int>(event.xbutton.button);
            const float x = static_cast<float>(event.xbutton.x);
            const float y = static_cast<float>(event.xbutton.y);
            if (button == kButtonWheelUp || button == kButtonWheelDown) {
                if (mPanel.wheel(x, y, button == kButtonWheelUp ? 1 : -1))
                    mDirty = true;
                return;
            }
            if (button == kButtonLeft && mPanel.mouseDown(x, y, button))
                mDirty = true;
            return;
        }

        case ButtonRelease:
            if (static_cast<int>(event.xbutton.button) == kButtonLeft &&
                mPanel.mouseUp(static_cast<float>(event.xbutton.x),
                               static_cast<float>(event.xbutton.y), kButtonLeft))
                mDirty = true;
            return;

        default:
            return;
    }
}

} // namespace Rations
