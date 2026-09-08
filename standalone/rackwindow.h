// RackWindow — the rack strip's own X window, below the amp's editor in the main window.
//
// A SIBLING CHILD WINDOW, not a region of the top-level. The amp's editor creates its own child
// window inside the top-level and handles its own input on its own X connection; the top-level
// therefore selects only structure events and would never see a click meant for the rack. Giving
// the rack a child window of its own means the two input paths never have to be told apart, and the
// editor's window can stay exactly what it was.
//
// PAINTING FOLLOWS THE SAME DISCIPLINE AS THE EDITOR ABOVE IT, and it is the editor's for a reason
// that applies here too: an X event never paints. It sets a dirty flag, and the next timer tick
// composes the whole strip into an offscreen image surface and blits it once with
// CAIRO_OPERATOR_SOURCE. A host that re-enters its run loop can therefore never recurse into
// drawing, and no partially drawn frame is ever on screen.
//
// ONE LOGICAL CANVAS. The strip is drawn in the same logical units as the editor and scaled by the
// same factor — one cairo_scale at compose time — so the two always agree at every window size.
// Mouse coordinates are divided by that scale before they reach RackView, which is why nothing in
// the rack code below knows what a pixel is.
//
// CHAIN EDITS HAPPEN HERE, ON THE RUN-LOOP THREAD, WITH AUDIO RUNNING. That is safe by
// construction and not by luck: ChainBuilder publishes an immutable snapshot by atomic exchange,
// the audio thread hands the old one back through a lock-free queue, and a removed plug-in is
// destroyed only once a later snapshot is live. Nothing here suspends the audio thread and nothing
// here is heard as a click.

#pragma once

#include "eventloop.h"

#include "gfx/fontstack.h"
#include "host/chainbuilder.h"
#include "rack/rackmodel.h"
#include "rack/rackview.h"

#include <X11/Xlib.h>

#include <functional>
#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class RackWindow
{
public:
    // Called when the user asks for a hosted plug-in's own editor. The standalone owns those
    // windows (they are top-level and outlive the rack's knowledge of them), so the rack only
    // reports the request.
    using EditorToggle = std::function<void(NAMp::host::ChainSection, int)>;
    // Called with a node's id immediately BEFORE its instance is removed from the chain. A hosted
    // editor window holds a reference to its backend, so a window left open across a removal would
    // be pointing at an object the builder is about to bury and then free. This is the one ordering
    // constraint the rack imposes on its owner.
    using EditorClose = std::function<void(uint64_t nodeId)>;
    // Called after any edit that changed the published chain, so the host can tell JACK its latency
    // moved.
    using ChainChanged = std::function<void()>;
    // Save and load a named rack. The rack strip knows nothing about the filesystem — it names a
    // preset and the standalone does the rest, which is what keeps NampRack free of file I/O and
    // lets the offline render tool drive the same overlay with no disk in the picture.
    using LoadPreset = std::function<bool(const std::string &name)>;
    using SavePreset = std::function<void(const std::string &name)>;
    // Discovery, for the same reason: the rack knows what the user asked for, the standalone owns
    // the catalogue, the path list and the file they persist to. A scan runs SYNCHRONOUSLY inside
    // this call — see the note on RackWindow::onTimer — so the callback is expected to repaint this
    // window from its progress callback and nothing else may reach the run loop meanwhile.
    using ScanPlugins = std::function<void()>;
    using EditSearchPath = std::function<void(const std::string &dir)>;

    RackWindow(EventLoop &loop, NAMp::host::ChainBuilder &builder);
    ~RackWindow();

    RackWindow(const RackWindow &) = delete;
    RackWindow &operator=(const RackWindow &) = delete;

    // Loads the fonts the editor above is set in, so the strip and the panel read as one window.
    // A failure is a warning and generic faces, never a refusal.
    void loadFonts(const std::string &resourceDir);

    void setCatalog(const std::vector<NAMp::host::PluginDesc> *catalog)
    {
        mModel.setCatalog(catalog);
    }
    void setEditorToggle(EditorToggle callback)
    {
        mEditorToggle = std::move(callback);
    }
    void setEditorClose(EditorClose callback)
    {
        mEditorClose = std::move(callback);
    }
    void setChainChanged(ChainChanged callback)
    {
        mChainChanged = std::move(callback);
    }
    void setPresetHandlers(LoadPreset load, SavePreset save)
    {
        mLoadPreset = std::move(load);
        mSavePreset = std::move(save);
    }
    void setDiscoveryHandlers(ScanPlugins scan, EditSearchPath add, EditSearchPath remove)
    {
        mScanPlugins = std::move(scan);
        mAddSearchPath = std::move(add);
        mRemoveSearchPath = std::move(remove);
    }
    // Where plug-ins are looked for, for the overlay to list. Not owned; must outlive this.
    void setSearchPaths(const std::vector<NAMp::rack::SearchPathRow> *paths)
    {
        mModel.setSearchPaths(paths);
        mDirty = true;
    }
    // Ask for a scan on the NEXT timer tick rather than now. A scan is synchronous and paints its
    // own progress, and the click that asked for it arrived in an X event handler — where nothing
    // in this project is allowed to paint, because a host that re-enters its run loop would then
    // recurse into drawing. Deferring by one tick keeps that intact rather than carving an
    // exception out of it.
    void requestScan()
    {
        mScanPending = true;
        mDirty = true;
    }
    // Called by the scan's progress callback: updates the banner and paints it NOW, because the run
    // loop is inside the scan and will not tick again until it finishes. Only ever reached from
    // onTimer, by way of requestScan().
    void showScanProgress(int index, int total, const std::string &current);
    void endScanProgress();
    // The saved racks the overlay lists, and which one is current. Not owned; must outlive this.
    void setPresets(const std::vector<std::string> *presets)
    {
        mModel.setPresets(presets);
        mDirty = true;
    }
    void setPresetName(std::string name)
    {
        mPresetName = std::move(name);
        mModel.setPresetName(mPresetName);
        mDirty = true;
    }
    const std::string &presetName() const
    {
        return mPresetName;
    }

    bool create(::Window parent, int x, int y, int w, int h);
    void destroy();

    // Move and resize with the top-level. `scale` is the window's logical-to-pixel factor, the same
    // one the editor is using.
    void setGeometry(int x, int y, int w, int h, double scale);

    // Run-loop tick: repaints if anything asked it to.
    void onTimer();
    void invalidate()
    {
        mDirty = true;
    }

    // The audio period the diagnostic cost bars are drawn against. Pushed in rather than read out
    // because the rack knows nothing about JACK, and re-pushed whenever the block size changes —
    // a cost bar measured against a stale period is a wrong number drawn confidently.
    void setAudioPeriod(double sampleRate, int frames)
    {
        mModel.setDiagPeriodMicros(sampleRate > 0.0 && frames > 0
                                       ? 1.0e6 * static_cast<double>(frames) / sampleRate
                                       : 0.0);
    }

    // The standalone tells the rack which hosted editors are on screen so the gear can light.
    void setEditorOpen(uint64_t nodeId, bool open);
    // The node id at a section/index, for turning an editor request back into something stable.
    uint64_t nodeIdAt(NAMp::host::ChainSection section, int index) const;

    // Applies one RackAction to the chain and republishes; returns true if the published chain
    // changed, which is what makes the host recompute its latency.
    //
    // Public because a RackAction is the project's one description of a chain edit, and more than
    // the pointer produces them: the live stress driver in the standalone drives real edits through
    // here while audio runs, and the preset loader will too. Keeping a second, parallel path from
    // "an edit" to "a published chain" is how the two would drift apart.
    bool applyAction(const NAMp::rack::RackAction &action);

    // Re-read the chain into the drawing snapshot. applyAction() does this itself; this is for a
    // caller that wants to inspect the rack before it has made its first edit.
    void refreshModel()
    {
        mModel.refresh();
        mDirty = true;
    }
    const NAMp::rack::RackModel &model() const
    {
        return mModel;
    }

private:
    void onXEvent(const XEvent &event);
    void redraw();
    bool resizeSurfaces(int w, int h);

    EventLoop &mLoop;
    NAMp::host::ChainBuilder &mBuilder;
    FontStack mFonts;
    NAMp::rack::RackModel mModel;
    NAMp::rack::RackView mView;

    EditorToggle mEditorToggle;
    EditorClose mEditorClose;
    ChainChanged mChainChanged;
    LoadPreset mLoadPreset;
    SavePreset mSavePreset;
    ScanPlugins mScanPlugins;
    EditSearchPath mAddSearchPath;
    EditSearchPath mRemoveSearchPath;
    std::string mPresetName;

    ::Window mWindow = 0;
    cairo_surface_t *mTarget = nullptr; // the X window
    cairo_surface_t *mBuffer = nullptr; // composed here, then blitted in one operation
    int mWidth = 0, mHeight = 0;
    double mScale = 1.0;
    bool mDirty = true;
    bool mScanPending = false;
};

} // namespace Rations
