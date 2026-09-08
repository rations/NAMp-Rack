// RackView — drawing and hit-testing for the rack strip.
//
// Stateless with respect to the chain: everything it draws comes from RackModel, and everything a
// click means comes back as a RackAction for the owner to apply. It knows nothing about X11, JACK,
// the VST3 SDK or plug-in instances, which is what lets tools/rackrender.cpp drive the whole
// interface offline with a chain that has no plug-ins in it at all.
//
// LAYOUT IS COMPUTED ONCE AND SHARED. Every row rect, card rect and cable midpoint comes out of
// layoutList()/layoutNodes(), and both draw() and hitTest() call them. A UI whose painting and
// whose hit-testing each work the geometry out for themselves is a UI where a button eventually
// stops being where it looks like it is; deriving both from one pass makes that unrepresentable.
//
// NO ART. Every glyph here — the in-circuit dot, the reorder arrows, the gear, the cross, the
// cables, the ports — is drawn from Canvas primitives rather than loaded from a file. The rack is
// host chrome that has to work when a resource directory is missing, and the alternative is another
// asset load with another degradation path to get right for no visual gain at this size.
//
// Coordinates arriving here are ALREADY in logical units and already relative to the rack's own
// origin. The caller divides by the window scale and subtracts the editor's height; nothing in this
// file knows what a pixel is.

#pragma once

#include "rackmodel.h"

#include "filebrowser.h"
#include "gfx/canvas.h"

namespace NAMp::rack
{

// The drawing primitives and the folder chooser belong to the amp this host is built around, and
// they are named unqualified all through this file and its implementation. Pulled into the rack's
// own namespace rather than qualified at every use: it keeps this file readable as the file it was
// ported from, and the three names it imports are this project's own, into a namespace that is also
// this project's.
using Rations::Canvas;
using Rations::FileBrowser;
using Rations::Rect;

//------------------------------------------------------------------------
class RackView
{
public:
    // Not owned; must outlive this.
    void setModel(RackModel *model)
    {
        mModel = model;
    }

    // Repaints from the model as it stands. Calls RackModel::refresh() itself, so a caller that has
    // just applied an action does not have to remember to.
    void draw(Canvas &c);

    //--- input ----------------------------------------------------------
    // button: 1 = left, 3 = right, matching X11. Every one of these returns the action the user
    // asked for, or Kind::NoAction when nothing happened and Kind::Redraw when only the picture
    // changed.
    RackAction mouseMove(float x, float y);
    RackAction mouseDown(float x, float y, int button);
    RackAction mouseUp(float x, float y, int button);
    RackAction wheel(float x, float y, int delta);

    HitTarget hitTest(float x, float y) const;

    // The folder chooser, open only while the user is adding a search path. It is the plug-in's own
    // FileBrowser rather than anything native (no GTK, no Qt, no portal — the same reason the amp
    // picks its capture folder that way), drawn inside the picker's box.
    bool browserOpen() const
    {
        return mBrowser.isOpen();
    }
    void openFolderBrowser(const std::string &startPath);

private:
    void drawHeader(Canvas &c);
    void drawList(Canvas &c);
    void drawNodes(Canvas &c);
    void drawPicker(Canvas &c);
    void drawFooter(Canvas &c);
    void drawScanProgress(Canvas &c);

    RackAction pickerMouseDown(float x, float y, int button);
    // Rows the overlay is listing right now, whichever of its three lists is up.
    int pickerRowCount() const;
    // Where this node's wet/dry track is in whichever view is up, so a drag can keep following it
    // after the pointer has left the row or the card. False when the node is not on screen — the
    // shelf draws no slider, and a node can be removed mid-drag.
    bool mixTrackFor(uint64_t nodeId, Rect &track) const;

    RackModel *mModel = nullptr;
    FileBrowser mBrowser;
    // Set on mouse-down, consumed on mouse-up, so a press that slides off its control does not fire
    // it — the behaviour every other button in this project already has.
    HitTarget mPressed;
};

} // namespace NAMp::rack
