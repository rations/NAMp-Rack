// namp-rack — the amp head, and the rack of other people's plug-ins around it, without a DAW.
//
// One top-level X window with the amp's own editor embedded in it, a run loop the editor can
// register with, and an audio backend feeding the processor. This phase is the amp alone: the rack
// strip, plug-in discovery and the chain engine are not built yet, and everything below is written
// so that adding them is an addition rather than a rearrangement.
//
// ONE FILE, NO INSTALLATION. The plug-in is linked in rather than loaded from a bundle, and its
// art and fonts are linked in with it, so this binary runs on a machine with nothing installed and
// can never end up running a DIFFERENT, older plug-in than the one it was built from. That is a
// change from the amp's own standalone, which dlopen'd an installed .vst3 — a reasonable choice
// there, where the bundle IS the product, and the wrong one here, where it is not.
//
// A file on disk still wins over the built-in copies, so replacing a layer of art still needs no
// rebuild. See the resource store for that lookup order.
//
// Threading: everything except the audio callback runs on this thread. The editor, the controller
// and the run loop are all single-threaded here, which is the same contract a DAW provides.

#include "audiobackend.h"
#include "jackclient.h"
#include "midiroute.h"
#include "editorframe.h"
#include "rackwindow.h"
#include "pluginwindow.h"
#include "panelwindow.h"

#include "rack/rackgeometry.h"
#include "eventloop.h"

#include "host/catalog.h"
#include "host/pluginpaths.h"
#include "host/rackpreset.h"
#include "host/chainbuilder.h"
#include "host/chainengine.h"
#include "host/scanchild.h"

#include "gfx/resourcestore.h"
#include "platform/respath.h"
#include "rationsids.h"
#include "version.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace Steinberg;

//------------------------------------------------------------------------
// The plug-in this binary was built from, with no bundle involved. GetPluginFactory() is the entry
// point the .vst3 exports to a host; here the same factory is simply linked in, so there is
// nothing to find and nothing to install.
//
// DECLARED rather than included: the SDK's constexpr factory header DEFINES it, and one binary
// cannot define it twice. It has C++ linkage — END_FACTORY does not wrap it in extern "C" — so
// this must match that macro's declaration exactly, at global scope.
Steinberg::IPluginFactory *PLUGIN_API GetPluginFactory();

namespace
{

// How often the run loop pumps what the audio thread published back into the controller, and how
// often it checks for a buffer-size change. 33 ms is a redraw cadence rather than a measurement:
// meters at 30 Hz look continuous and a footswitch echo inside 33 ms is not perceptible.
constexpr uint32 kUiTickMs = 33;

// The size the editor comes up at is the editor's own (its head page at scale 1.0); these are only
// the last resort for a view that refuses to report one.
constexpr int kFallbackW = 1133;
constexpr int kFallbackH = 403;

Rations::EventLoop *gEventLoop = nullptr;

void onSignal(int)
{
    if (gEventLoop)
        gEventLoop->stop();
}

//------------------------------------------------------------------------
// The host end of the VST3 edit loop. The editor calls performEdit() when the user moves a
// control; we forward the value to the processor through the backend's lock-free ring.
class ComponentHandler : public Vst::IComponentHandler
{
public:
    explicit ComponentHandler(Rations::AudioBackend &audio) : mAudio(audio)
    {
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE
    {
        mAudio.pushParameter(id, value);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit(Vst::ParamID) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent(int32) SMTG_OVERRIDE
    {
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IComponentHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::AudioBackend &mAudio;
};

//------------------------------------------------------------------------
// Re-runs setupProcessing when the audio system changes its buffer size under the running client.
// The chunk loop inside the backend keeps audio correct without this - no block ever reaches the
// processor larger than the size it was set up for - but the processor would otherwise stay
// configured for the size it saw at startup, sizing its internal buffers and its reported latency
// for a block the host is no longer sending.
//
// This runs on the run loop, not in the audio system's own notification callback: setActive and
// setupProcessing are VST3 main-thread calls and the plug-in's message thread may be part-way
// through a load. AudioBackend::suspendProcessing() is what keeps the audio callback out of the
// processor while it is reconfigured.
class BufferSizeWatcher : public Linux::ITimerHandler
{
public:
    BufferSizeWatcher(Rations::AudioBackend &audio, Vst::IComponent *component,
                      Vst::IAudioProcessor *processor, const Vst::ProcessSetup &setup,
                      NAMp::host::ChainEngine &engine, NAMp::host::ChainBuilder &builder)
        : mAudio(audio), mEngine(engine), mBuilder(builder), mComponent(component),
          mProcessor(processor), mSetup(setup)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        const int size = mAudio.takeBufferSizeChange();
        if (size <= 0)
            return;

        if (!mAudio.suspendProcessing()) {
            fprintf(stderr,
                    "namp-rack: the audio thread did not respond, so the processor was left set "
                    "up for %d frames\n",
                    mAudio.blockSize());
            return;
        }

        mProcessor->setProcessing(false);
        mComponent->setActive(false);

        Vst::ProcessSetup setup = mSetup;
        setup.maxSamplesPerBlock = size;
        const bool ok = mProcessor->setupProcessing(setup) == kResultOk;
        if (ok)
            mSetup = setup;
        else
            fprintf(stderr, "namp-rack: the plug-in refused %d frames; keeping %d\n", size,
                    mSetup.maxSamplesPerBlock);

        mComponent->setActive(true);
        mProcessor->setProcessing(true);

        // THE RACK IS RECONFIGURED INSIDE THE SAME SUSPENSION. Its scratch buses are sized for the
        // old block and every hosted plug-in was told the old maximum; leaving either behind means
        // the engine falls transparent the moment a larger block arrives — silently, because a
        // transparent engine is exactly what an empty rack looks like.
        //
        // Only adopt the new chunk size if the plug-in accepted it. If it did not, the old size is
        // still what it is prepared for, and the chunk loop must keep honouring that.
        const int adopted = ok ? size : mSetup.maxSamplesPerBlock;
        mEngine.prepare(adopted);
        mBuilder.configure(mSetup.sampleRate, adopted);
        mAudio.resumeProcessing(adopted);
        mAudio.notifyLatencyChanged();

        printf("namp-rack: the audio buffer size is now %d frames\n", mAudio.blockSize());
        fflush(stdout);
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::AudioBackend &mAudio;
    NAMp::host::ChainEngine &mEngine;
    NAMp::host::ChainBuilder &mBuilder;
    Vst::IComponent *mComponent = nullptr;
    Vst::IAudioProcessor *mProcessor = nullptr;
    Vst::ProcessSetup mSetup;
};

//------------------------------------------------------------------------
// Feeds everything the audio thread published back into the controller. Runs as a run-loop timer
// on the UI thread, so no IEditController call ever happens on the RT thread.
//
// THIS IS THE HOST'S HALF OF A LOOP, and it carries more than the meters. A VST3 plug-in reports a
// parameter IT changed by itself through outputParameterChanges, and the host is what turns that
// back into IEditController::setParamNormalized so the panel agrees with the audio. The amp
// changes parameters by itself whenever the MIDI learn table fires - a stomp is the plug-in moving
// its own channel switch - so a pump that forwarded only the hidden meter parameters left a
// footswitch that changed the SOUND while the panel sat still: the bat switch stayed where it was.
// Nothing was wrong with the plug-in; the host end of the loop was missing.
//
// Only slots whose sequence number has moved are forwarded, so the meters - which arrive every
// block - do not turn into a redraw storm.
class FeedbackPump : public Linux::ITimerHandler
{
public:
    FeedbackPump(Rations::AudioBackend &audio, Vst::IEditController *controller)
        : mAudio(audio), mController(controller)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        if (!mController)
            return;
        for (int i = 0; i < Rations::AudioBackend::kFeedbackSlots; ++i) {
            Vst::ParamID id = Vst::kNoParamId;
            double value = 0.0;
            uint32_t seq = 0;
            if (!mAudio.readFeedback(i, id, value, seq))
                break; // slots are filled in order; the first empty one is the end
            if (seq == mSeen[i])
                continue;
            mSeen[i] = seq;
            mController->setParamNormalized(id, value);
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::AudioBackend &mAudio;
    Vst::IEditController *mController;
    // The sequence number last forwarded for each slot. Zero is "never seen", and the audio thread
    // increments before it ever publishes, so slot values always start out looking new.
    uint32_t mSeen[Rations::AudioBackend::kFeedbackSlots] = {};
};

//------------------------------------------------------------------------
// Wrap the factory linked into this binary in the hosting layer's PluginFactory, so everything
// downstream (PlugProvider, class enumeration, instantiation) is identical whether the amp came
// from here or from a bundle named on the command line.
//
// owned() rather than shared(): the SDK's own module loader takes ownership of what
// GetPluginFactory() returns, and the constexpr factory is ImplementsNonDestroyable, so the
// matching release() at teardown frees nothing.
VST3::Hosting::PluginFactory builtInFactory()
{
    return VST3::Hosting::PluginFactory(owned(::GetPluginFactory()));
}

//------------------------------------------------------------------------
// Settings file. A four-channel amp that forgets which captures it was playing every time it
// starts is not usable - there are four banks, two impulse responses and four MIDI bindings behind
// this - and a DAW would keep all of it in the project.
std::string statePath()
{
    std::string dir;
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        dir = xdg;
    else if (const char *home = getenv("HOME"); home && *home)
        dir = std::string(home) + "/.config";
    else
        return {};
    return dir + "/NAMp-Rack/standalone.state";
}

// Restores what the last session was playing. A truncated or foreign file is not an error worth
// stopping for: the plug-in is required to reject a bad blob cleanly, and the standalone then
// simply starts with four empty channels - which is an ordinary state here, not a broken one.
void loadState(Vst::IComponent *component, Vst::IEditController *controller)
{
    const std::string path = statePath();
    if (path.empty())
        return;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return;

    std::vector<char> bytes;
    char buffer[4096];
    size_t got = 0;
    // A settings file this large is not one we wrote; stop reading rather than grow without bound
    // on a file someone else put there.
    constexpr size_t kMaxState = 1u << 20;
    bool tooLarge = false;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
        if (bytes.size() > kMaxState) {
            tooLarge = true;
            break;
        }
    }
    fclose(file);
    if (tooLarge) {
        fprintf(stderr, "namp-rack: %s is implausibly large; ignoring it\n", path.c_str());
        return;
    }
    if (bytes.empty())
        return;

    MemoryStream stream(bytes.data(), static_cast<TSize>(bytes.size()));
    if (component->setState(&stream) != kResultOk) {
        fprintf(stderr, "namp-rack: %s was rejected; starting empty\n", path.c_str());
        return;
    }
    // The controller reads the SAME blob from the start, which is what a host does. Its reader is
    // a second walk over the same bytes, so the two must be handed identical input or the panel
    // shows something the audio does not agree with.
    int64 ignored = 0;
    stream.seek(0, IBStream::kIBSeekSet, &ignored);
    controller->setComponentState(&stream);
}

void saveState(Vst::IComponent *component)
{
    const std::string path = statePath();
    if (path.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return;

    MemoryStream stream;
    if (component->getState(&stream) != kResultOk)
        return;

    FILE *file = fopen(path.c_str(), "wb");
    if (!file) {
        fprintf(stderr, "namp-rack: cannot write %s\n", path.c_str());
        return;
    }
    const size_t size = static_cast<size_t>(stream.getSize());
    if (size > 0 && fwrite(stream.getData(), 1, size, file) != size)
        fprintf(stderr, "namp-rack: short write to %s\n", path.c_str());
    fclose(file);
}

//------------------------------------------------------------------------
// Turn a --pre/--post argument into a catalogue entry. A full key is taken as it stands; anything
// else is matched against display names, exactly first and then as a substring, both
// case-insensitively.
//
// AN AMBIGUOUS SUBSTRING IS REPORTED RATHER THAN GUESSED AT. Silently loading the wrong plug-in is
// worse than refusing to load one: the chain still makes sound, so nothing looks broken, and the
// person is left wondering why their pedal does not sound like their pedal.
bool resolvePlugin(const NAMp::host::Catalog &catalog, const std::string &spec,
                   NAMp::host::PluginRef &out)
{
    for (const auto &entry : catalog.entries()) {
        if (entry.ref.key == spec) {
            out = entry.ref;
            return true;
        }
    }

    auto lower = [](std::string text) {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string needle = lower(spec);

    const NAMp::host::PluginDesc *exact = nullptr;
    std::vector<const NAMp::host::PluginDesc *> partial;
    for (const auto &entry : catalog.entries()) {
        const std::string name = lower(entry.name);
        if (name == needle)
            exact = &entry;
        else if (name.find(needle) != std::string::npos)
            partial.push_back(&entry);
    }

    if (exact) {
        out = exact->ref;
        return true;
    }
    if (partial.size() == 1) {
        out = partial.front()->ref;
        printf("namp-rack: '%s' -> %s\n", spec.c_str(), partial.front()->name.c_str());
        return true;
    }
    if (partial.empty()) {
        fprintf(stderr, "namp-rack: no scanned plug-in matches '%s'\n", spec.c_str());
        return false;
    }
    fprintf(stderr, "namp-rack: '%s' is ambiguous:\n", spec.c_str());
    for (const auto *entry : partial)
        fprintf(stderr, "    %s\n", entry->name.c_str());
    return false;
}

//------------------------------------------------------------------------
// The builder's idle tick.
//
// The audio thread never frees anything: it pushes the chain snapshot it has stopped using into a
// lock-free queue, and this is the thread that pops it and runs the only delete. Doing nothing here
// leaks nothing permanently — it just leaves retired snapshots and departed plug-ins alive until
// something does collect — but with audio running and a chain being edited it is what keeps that
// bounded.
class ChainCollector : public Linux::ITimerHandler
{
public:
    explicit ChainCollector(NAMp::host::ChainBuilder &builder) : mBuilder(builder)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        mBuilder.collect();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NAMp::host::ChainBuilder &mBuilder;
};

//------------------------------------------------------------------------
// Add and remove a plug-in from the running chain, over and over, with audio going.
//
// THIS IS THE GATE ON THE THING THAT IS SAFE BY CONSTRUCTION RATHER THAN BY LUCK. Editing the rack
// while the audio thread is inside it works because the chain is an immutable snapshot published by
// atomic exchange, the audio thread never edits or frees one, and a departed plug-in is destroyed
// only once a LATER snapshot is live. Every one of those is a claim about a race, and a claim about
// a race is worth exactly what it has been measured at.
//
// So this churns as fast as the UI timer runs — publish, collect, publish — while the amp is
// sounding, and the dropout count at the end is the answer. It runs on the run-loop thread, which
// is where a rack edit comes from when a person does it, so the path under test is the real one.
class RackStress : public Linux::ITimerHandler
{
public:
    // `byEnable` toggles a node that stays loaded instead of adding and removing one. The two
    // churn the SAME publish/adopt/retire handshake, and differ in exactly one thing: whether the
    // plug-in the audio thread starts running has any internal state in it yet. That is what makes
    // the pair able to tell a click in the mechanism from a click in the plug-in.
    RackStress(NAMp::host::ChainBuilder &builder, Rations::AudioBackend &audio,
               Rations::EventLoop &loop, const NAMp::host::PluginRef &ref, double seconds,
               bool byEnable)
        : mBuilder(builder), mAudio(audio), mLoop(loop), mRef(ref), mSeconds(seconds),
          mByEnable(byEnable)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        if (mStart == Clock::time_point{})
            mStart = Clock::now();

        const double elapsed = std::chrono::duration<double>(Clock::now() - mStart).count();
        if (elapsed >= mSeconds) {
            // Names the mode it actually ran. The two churn very differently — one reloads the
            // plug-in every cycle and one does not — and a line that reported them the same way
            // would make two measurements look like one repeated.
            printf("namp-rack: %d %s cycles in %.1f s, %u dropout%s at %d frames\n", mCycles,
                   mByEnable ? "enable/disable" : "add/remove", elapsed, mAudio.dropouts(),
                   mAudio.dropouts() == 1 ? "" : "s", mAudio.blockSize());
            fflush(stdout);
            mLoop.stop();
            return;
        }

        // Alternate: one tick adds, the next removes. Both publish, so the audio thread adopts a
        // new snapshot every tick and hands the previous one back — which is the handshake being
        // exercised.
        if (mByEnable) {
            // The instance was loaded once, at startup, and stays loaded. Only its enabled flag
            // moves, so every snapshot the audio thread adopts holds a plug-in that has been
            // running and has its history.
            mPresent = !mPresent;
            mBuilder.setEnabled(NAMp::host::ChainSection::Post, 0, mPresent);
        } else if (mPresent) {
            mBuilder.remove(NAMp::host::ChainSection::Post, 0);
            mPresent = false;
        } else {
            std::string error;
            if (mBuilder.add(NAMp::host::ChainSection::Post, mRef, error) < 0) {
                fprintf(stderr, "namp-rack: rack-stress could not add the plug-in: %s\n",
                        error.c_str());
                mLoop.stop();
                return;
            }
            mPresent = true;
        }
        mBuilder.publish();
        mAudio.notifyLatencyChanged();
        ++mCycles;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    using Clock = std::chrono::steady_clock;

    NAMp::host::ChainBuilder &mBuilder;
    Rations::AudioBackend &mAudio;
    Rations::EventLoop &mLoop;
    NAMp::host::PluginRef mRef;
    double mSeconds = 0.0;
    Clock::time_point mStart{};
    int mCycles = 0;
    bool mPresent = false;
    bool mByEnable = false;
};

//------------------------------------------------------------------------
// One hosted plug-in's editor window, whichever kind it turned out to need.
//
// Exactly one of the two is set. `window` is the plug-in's own editor embedded in a window of
// ours; `panel` is the generic parameter list, for a plug-in that has no editor this host can
// show. Both answer the same four verbs, which is why nothing below has to know which it got —
// choosing between them happens once, in makeEditor(), and every other path stays
// format-agnostic.
struct HostedEditor {
    uint64_t id = 0;
    std::unique_ptr<Rations::PluginWindow> window;
    std::unique_ptr<Rations::PanelWindow> panel;

    bool isOpen() const
    {
        return window ? window->isOpen() : (panel && panel->isOpen());
    }
    bool open()
    {
        return window ? window->open() : (panel && panel->open());
    }
    void close()
    {
        if (window)
            window->close();
        if (panel)
            panel->close();
    }
    void idle()
    {
        if (window)
            window->idle();
        if (panel)
            panel->idle();
    }
};

//------------------------------------------------------------------------
// The rack strip's own tick: repaint if anything asked, and keep the diagnostic cost bars measured
// against the period the audio thread is actually running at.
class RackTicker : public Linux::ITimerHandler
{
public:
    RackTicker(Rations::RackWindow &rack, Rations::AudioBackend &audio) : mRack(rack), mAudio(audio)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        // Re-pushed every tick rather than once at startup: the buffer size can change under a
        // running client, and a cost bar drawn as a fraction of a stale period is a wrong number
        // drawn confidently.
        if (mAudio.isOpen())
            mRack.setAudioPeriod(mAudio.sampleRate(), mAudio.blockSize());
        mRack.onTimer();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Rations::RackWindow &mRack;
    Rations::AudioBackend &mAudio;
};

//------------------------------------------------------------------------
// Drives every open hosted editor once per tick. Some formats need an idle callback to draw at all,
// and a resize a plug-in latched from a thread we do not control is applied here.
class EditorPump : public Linux::ITimerHandler
{
public:
    using Windows = std::vector<HostedEditor>;

    EditorPump(Windows &windows, Rations::RackWindow *rack) : mWindows(windows), mRack(rack)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        for (auto &hosted : mWindows) {
            const bool wasOpen = hosted.isOpen();
            hosted.idle();
            // A plug-in's editor can close itself — the user clicks the title bar and the window
            // handles it without telling anyone. The rack's gear would otherwise stay lit for a
            // window that is gone.
            if (mRack && wasOpen && !hosted.isOpen())
                mRack->setEditorOpen(hosted.id, false);
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Windows &mWindows;
    Rations::RackWindow *mRack = nullptr;
};

//------------------------------------------------------------------------
// Opens and closes every hosted editor over and over, which is the only way to prove the teardown
// order holds. editorClose() -> removeWindow() -> XDestroyWindow() is load-bearing: destroy the X
// window before telling the plug-in to close its editor and its timer fires against a window that
// no longer exists, which is a crash inside somebody else's code with our stack nowhere in the
// backtrace. Reading the source cannot show that; cycling it can.
class EditorCycler : public Linux::ITimerHandler
{
public:
    using Windows = std::vector<HostedEditor>;

    EditorCycler(Windows &windows, Rations::EventLoop &loop, int cycles)
        : mWindows(windows), mLoop(loop), mCycles(cycles)
    {
    }

    void PLUGIN_API onTimer() SMTG_OVERRIDE
    {
        if (mTick == 0) {
            int opened = 0;
            for (auto &hosted : mWindows)
                opened += hosted.open() ? 1 : 0;
            if (mCycle == 0)
                mOpenedFirstCycle = opened;
            else if (opened != mOpenedFirstCycle)
                // A window that opened once and will not open again is exactly the failure this
                // test exists to catch, and it does not show up as a leak.
                fprintf(stderr, "namp-rack: cycle %d opened %d editor(s), the first opened %d\n",
                        mCycle + 1, opened, mOpenedFirstCycle);
        } else if (mTick == kOpenTicks) {
            for (auto &hosted : mWindows)
                hosted.close();
        }

        if (++mTick < kOpenTicks + kClosedTicks)
            return;

        mTick = 0;
        if (++mCycle >= mCycles) {
            mDone = true;
            mLoop.stop();
        }
    }

    bool finished() const
    {
        return mDone;
    }
    int openedPerCycle() const
    {
        return mOpenedFirstCycle;
    }
    int completedCycles() const
    {
        return mCycle;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Linux::ITimerHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::ITimerHandler *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    // Long enough at the UI tick for a view to register its handlers, receive its first X events
    // and paint at least once; short enough that a hundred cycles is still seconds rather than
    // minutes.
    static constexpr int kOpenTicks = 8;
    static constexpr int kClosedTicks = 2;

    Windows &mWindows;
    Rations::EventLoop &mLoop;
    int mCycles;
    int mCycle = 0;
    int mTick = 0;
    int mOpenedFirstCycle = 0;
    bool mDone = false;
};

//------------------------------------------------------------------------
// Where plug-ins are looked for, as the overlay lists it. The automatic rows are the ones discovery
// reaches on its own — measured by the host layer rather than declared here — and cannot be
// removed, because removing something nobody added is not a thing this host can do. They are listed
// anyway: without them the list reads as though the rack only looks where it was pointed, which
// would have people adding ~/.vst3 by hand.
std::vector<NAMp::rack::SearchPathRow> buildSearchPathRows(const NAMp::host::PluginPaths &paths)
{
    std::vector<NAMp::rack::SearchPathRow> rows;
    for (const std::string &dir : NAMp::host::automaticVst3Roots())
        rows.push_back({dir, "VST3", true});
    for (const std::string &dir : NAMp::host::automaticLv2Roots())
        rows.push_back({dir, "LV2", true});
    // A user folder carries no format tag: it is offered to every scanner, which is why adding one
    // does not make anybody choose a format they have no way of knowing yet.
    for (const std::string &dir : paths.roots())
        rows.push_back({dir, std::string(), false});
    return rows;
}

//------------------------------------------------------------------------
struct Options {
    // An external .vst3 to host INSTEAD of the amp linked in. Empty is the normal case. It exists
    // so a build of the amp made somewhere else can be driven by this rig without reinstalling
    // anything, which is how a bug that only appears in the bundle gets reproduced.
    std::string bundle;
    bool useState = true;
    // Plug-ins to put in the rack before and after the amp, in the order given. Named rather than
    // keyed: a display name is what a person has, and resolvePlugin() refuses an ambiguous one
    // instead of guessing. There is no rack interface yet, so this is how a chain is built.
    std::vector<std::string> pre;
    std::vector<std::string> post;
    // List what the scan found and exit. The same catalogue --pre/--post resolve against, so the
    // two can never disagree about what is installed.
    bool listPlugins = false;
    // Seconds to spend adding and removing a plug-in from the chain with audio running, then
    // report and exit. Zero is off. Needs one --post to know what to churn.
    double rackStressSeconds = 0.0;
    // Churn by toggling the node's enabled flag rather than by loading and unloading it.
    bool rackStressByEnable = false;
    // The saved rack to start from and write back to. Empty means "default", so a chain survives a
    // restart the same way a capture bank does — a pedalboard you have to rebuild every time you
    // start is not a pedalboard.
    std::string rack;
    // Open every hosted plug-in's editor at startup, rather than waiting for the gear to be
    // clicked. For looking at one without driving the rack first.
    bool showEditors = false;
    // Open and close every hosted editor this many times, then report and exit. This is the gate on
    // the teardown order, which cannot be proved by reading it.
    int editorCycles = 0;
    // Use the generic parameter panel for every plug-in, even one that has an editor of its own.
    // A plug-in's own editor can be broken, unreadable at this screen's size, or simply worse than
    // a list of its parameters, and the panel is built from the backend interface so it always
    // works. It is also the only way to reach the no-editor branch on a machine where every
    // installed plug-in happens to have one.
    bool genericPanel = false;
};

void printUsage()
{
    const std::string path = statePath();
    printf("usage: namp-rack [options] [path to a .vst3 bundle]\n"
           "\n"
           "  The amp head as a JACK application: its own editor in a window of its own, with the\n"
           "  amp on JACK's ports. A JACK server must already be running; without one the editor\n"
           "  still opens and everything but the sound works.\n"
           "\n"
           "  The amp is built into this binary along with its art and fonts, so nothing has to\n"
           "  be installed. Naming a bundle hosts THAT plug-in instead, which is for driving a\n"
           "  build made elsewhere.\n"
           "\n"
           "  --pre NAME    put a scanned plug-in in front of the amp; repeatable, and the\n"
           "                order given is the order they run in\n"
           "  --post NAME   the same, after the amp\n"
           "  --list        list every plug-in the scan found, and exit\n"
           "  --rack NAME   start from the saved rack NAME and write it back on exit\n"
           "                (default: 'default'; --pre/--post override it for this run)\n"
           "  --editors     open every hosted plug-in's editor at startup\n"
           "  --generic-panel  show the parameter list instead of a plug-in's own editor\n"
           "  --editor-cycles N  open and close every hosted editor N times, then report and\n"
           "                exit; the gate on the editor teardown order\n"
           "  --rack-stress S  add and remove the first --post plug-in for S seconds with audio\n"
           "                running, then report the dropout count and exit\n"
           "  --rack-stress-enable  as above, but toggle the node's enabled flag instead, so the\n"
           "                plug-in stays loaded and keeps its internal state\n"
           "  --no-state    do not read or write %s\n"
           "  -h, --help    this message\n"
           "\n"
           "  Ports: NAMp-Rack:in, NAMp-Rack:out_l, NAMp-Rack:out_r (connected to the first\n"
           "  physical ports found) and NAMp-Rack:midi_in for a footswitch, which is left\n"
           "  unconnected because guessing which MIDI source is the pedal would be worse than\n"
           "  not trying.\n",
           path.empty() ? "the settings file" : path.c_str());
}

// Run: carry on. Exit: the user asked for the usage message and got it. Error: they got it too,
// but did not ask. The three are separate because the exit code is not the same.
enum class ArgResult { Run, Exit, Error };

ArgResult parseArgs(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return ArgResult::Exit;
        }
        if (arg == "--no-state") {
            opt.useState = false;
            continue;
        }
        if (arg == "--list") {
            opt.listPlugins = true;
            continue;
        }
        if (arg == "--editors") {
            opt.showEditors = true;
            continue;
        }
        if (arg == "--generic-panel") {
            opt.genericPanel = true;
            continue;
        }
        if (arg == "--rack") {
            if (i + 1 >= argc) {
                fprintf(stderr, "namp-rack: --rack needs the name of a saved rack\n");
                return ArgResult::Error;
            }
            opt.rack = argv[++i];
            continue;
        }
        if (arg == "--editor-cycles") {
            if (i + 1 >= argc) {
                fprintf(stderr, "namp-rack: --editor-cycles needs a count\n");
                return ArgResult::Error;
            }
            opt.editorCycles = std::atoi(argv[++i]);
            if (opt.editorCycles <= 0) {
                fprintf(stderr, "namp-rack: --editor-cycles needs a positive count\n");
                return ArgResult::Error;
            }
            continue;
        }
        if (arg == "--rack-stress-enable") {
            opt.rackStressByEnable = true;
            continue;
        }
        if (arg == "--rack-stress") {
            if (i + 1 >= argc) {
                fprintf(stderr, "namp-rack: --rack-stress needs a number of seconds\n");
                return ArgResult::Error;
            }
            opt.rackStressSeconds = std::atof(argv[++i]);
            continue;
        }
        if (arg == "--pre" || arg == "--post") {
            if (i + 1 >= argc) {
                fprintf(stderr, "namp-rack: %s needs a plug-in name\n", arg.c_str());
                return ArgResult::Error;
            }
            (arg == "--pre" ? opt.pre : opt.post).emplace_back(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "namp-rack: unknown option %s\n", arg.c_str());
            printUsage();
            return ArgResult::Error;
        }
        opt.bundle = arg;
    }
    return ArgResult::Run;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // The built-in art and fonts, before anything can ask for one. There is no bundle beside this
    // binary to read them from. A file on disk still WINS where one exists, so an external bundle
    // named on the command line and the resource-directory override both keep working.
    Rations::installBuiltinResources();

    // This binary is its own scan helper, so a bundle is probed by a re-exec of it in a process
    // whose only job is to be expendable. Before anything else, because the child must do nothing
    // but the probe: no window, no audio, no state file.
    int scanExit = 0;
    if (NAMp::host::runScanChildIfRequested(argc, argv, scanExit))
        return scanExit;

    Options opt;
    switch (parseArgs(argc, argv, opt)) {
        case ArgResult::Exit:
            return 0;
        case ArgResult::Error:
            return 2;
        case ArgResult::Run:
            break;
    }

    // Where plug-ins are looked for, and what was found there. Both outlive everything that reads
    // them, which is why they are here rather than beside the chain: the rack's model holds a
    // pointer to the catalogue's entries and to the path rows, and a scan asked for from the strip
    // refills these same objects in place.
    NAMp::host::PluginPaths pluginPaths;
    const std::string pluginPathsFile = NAMp::host::PluginPaths::defaultFile();
    pluginPaths.load(pluginPathsFile);
    NAMp::host::Catalog catalog;
    catalog.setSearchPaths(&pluginPaths);
    std::vector<NAMp::rack::SearchPathRow> searchPathRows = buildSearchPathRows(pluginPaths);

    // Answered before the amp, the audio device or the window exist, because it needs none of them
    // and a scan that had to open a JACK connection first would be a scan nobody could run.
    if (opt.listPlugins) {
        catalog.rescan();
        for (const auto &entry : catalog.entries())
            printf("%-5s %-40s %s\n", NAMp::host::formatTag(entry.ref.format), entry.name.c_str(),
                   entry.ref.key.c_str());
        printf("\n%zu plug-in(s); %d probed; %d new since the last scan\n",
               catalog.entries().size(), catalog.probedCount(), catalog.newCount());
        return 0;
    }

    // Say which plug-in this is, always. It is one line, and it is the line that distinguishes
    // "the build I just made" from "something installed months ago" - a distinction that is
    // otherwise invisible and that silently invalidates any measurement taken against the wrong
    // one.
    if (opt.bundle.empty())
        printf("namp-rack: amp built in (%s)\n", FULL_VERSION_STR);
    else
        printf("namp-rack: amp from %s\n", opt.bundle.c_str());

    // The host context must be published BEFORE the plug-in is instantiated:
    // ComponentBase::allocateMessage() asks it for IMessage instances, so without one every
    // controller->processor message (the four capture banks, the two IRs, Slim) is silently
    // dropped and only parameter changes get through. That presents as a plug-in whose knobs work
    // and which never loads a capture.
    Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    // Held only in the external-bundle case, and declared out here so it outlives everything the
    // factory below hands out. A module destroyed while its objects are still alive is a dlclose
    // under live vtables.
    VST3::Hosting::Module::Ptr module;
    std::unique_ptr<VST3::Hosting::PluginFactory> factory;

    if (opt.bundle.empty()) {
        factory = std::make_unique<VST3::Hosting::PluginFactory>(builtInFactory());
    } else {
        std::string error;
        module = VST3::Hosting::Module::create(opt.bundle, error);
        if (!module) {
            fprintf(stderr, "namp-rack: cannot load %s\n  %s\n", opt.bundle.c_str(), error.c_str());
            return 1;
        }
        factory = std::make_unique<VST3::Hosting::PluginFactory>(module->getFactory());
    }

    // --- instantiate the plug-in -------------------------------------
    IPtr<Vst::PlugProvider> provider;
    for (auto &classInfo : factory->classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        provider = owned(new Vst::PlugProvider(*factory, classInfo, true));
        if (provider->initialize())
            break;
        provider = nullptr;
    }
    if (!provider) {
        fprintf(stderr, "namp-rack: no audio effect class to instantiate\n");
        return 1;
    }

    Vst::IComponent *component = provider->getComponent();
    Vst::IEditController *controller = provider->getController();
    if (!component || !controller) {
        fprintf(stderr, "namp-rack: the plug-in did not provide both parts\n");
        return 1;
    }

    FUnknownPtr<Vst::IAudioProcessor> processor(component);
    if (!processor) {
        fprintf(stderr, "namp-rack: the plug-in has no IAudioProcessor\n");
        return 1;
    }

    // --- audio -------------------------------------------------------
    // Where each kind of MIDI message has to be delivered, worked out here on the main thread
    // because both lookups are IEditController calls and the audio thread may never make one.
    //
    // Declared BEFORE the backend on purpose: the backend reads it from the audio thread, so it
    // has to outlive the backend, and destruction runs in reverse declaration order. That matters
    // on the early-return paths below, where nothing has called close() and the destructor is what
    // stops the audio thread.
    Rations::MidiRoute route;
    route.resolve(controller);

    // The one place a platform is named. Everything below this line talks to AudioBackend, so the
    // Windows build changes this declaration and nothing else.
    Rations::JackClient jackBackend;
    Rations::AudioBackend &audio = jackBackend;

    // A first connection just to learn the server's rate and block size, so setupProcessing can be
    // told the truth before the component is activated.
    jack_status_t status = static_cast<jack_status_t>(0);
    jack_client_t *probe = jack_client_open("NAMp-Rack-probe", JackNoStartServer, &status);
    double sampleRate = 48000.0;
    int blockSize = 1024;
    if (probe) {
        sampleRate = static_cast<double>(jack_get_sample_rate(probe));
        blockSize = static_cast<int>(jack_get_buffer_size(probe));
        jack_client_close(probe);
    } else {
        fprintf(stderr, "namp-rack: no JACK server; continuing with the editor only\n");
    }

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "namp-rack: the plug-in rejected the process setup\n");
        return 1;
    }

    // Before setActive, which is where a host puts it: the four banks then start building from the
    // paths the blob names, and each channel sits at its ramped-silence gate until its first entry
    // lands.
    if (opt.useState)
        loadState(component, controller);

    component->setActive(true);
    processor->setProcessing(true);

    ComponentHandler handler(audio);
    controller->setComponentHandler(&handler);

    // --- the rack ----------------------------------------------------
    // Built BEFORE the audio device is opened, so the first process callback already sees the whole
    // chain. Publishing against a running audio thread is safe by construction and is what every
    // later edit does — but doing it just to get started would mean the first block ran a chain
    // that was still being assembled.
    NAMp::host::ChainEngine chainEngine;
    NAMp::host::ChainBuilder chainBuilder;
    // The first --post plug-in, kept for --rack-stress. Resolving it again there would mean a
    // second catalogue scan for a reference this loop already has.
    NAMp::host::PluginRef stressRef;
    chainEngine.prepare(blockSize);
    chainBuilder.setEngine(&chainEngine);
    chainBuilder.configure(sampleRate, blockSize);

    // One scan for the whole run, whether or not the command line names a plug-in: the rack's
    // picker lists what this finds, and a warm rescan probes zero bundles, so the cost of doing it
    // unconditionally is reading a cache file.
    catalog.rescan();
    printf("namp-rack: %zu plug-in(s); %d probed; %d new\n", catalog.entries().size(),
           catalog.probedCount(), catalog.newCount());

    if (!opt.pre.empty() || !opt.post.empty()) {
        const struct {
            const std::vector<std::string> &specs;
            NAMp::host::ChainSection section;
            const char *label;
        } sections[] = {{opt.pre, NAMp::host::ChainSection::Pre, "before"},
                        {opt.post, NAMp::host::ChainSection::Post, "after"}};

        for (const auto &group : sections) {
            for (const auto &spec : group.specs) {
                NAMp::host::PluginRef ref;
                if (!resolvePlugin(catalog, spec, ref))
                    continue;
                std::string loadError;
                if (chainBuilder.add(group.section, ref, loadError) < 0) {
                    // One plug-in failing to load is not a reason to refuse to make sound.
                    fprintf(stderr, "namp-rack: %s: %s\n", spec.c_str(), loadError.c_str());
                    continue;
                }
                if (group.section == NAMp::host::ChainSection::Post && !stressRef.valid())
                    stressRef = ref;
                printf("namp-rack: %s the amp, %s\n", group.label, spec.c_str());
            }
        }
        chainBuilder.publish();
    }

    // --- the saved rack ----------------------------------------------
    // The command line wins. --pre/--post describe a chain explicitly, and merging a saved rack
    // into it would produce something the user did not ask for and cannot see the reason for.
    const std::string rackName = opt.rack.empty() ? std::string("default") : opt.rack;
    std::vector<std::string> savedRacks = NAMp::host::listRacks();
    const bool rackFromCommandLine = !opt.pre.empty() || !opt.post.empty();

    auto loadRackNamed = [&](const std::string &name) {
        const std::string path = NAMp::host::rackPath(name);
        if (path.empty())
            return false;
        NAMp::host::ApplyReport report;
        std::string error;
        if (!NAMp::host::loadRack(chainBuilder, path, report, error)) {
            fprintf(stderr, "namp-rack: cannot load the rack '%s': %s\n", name.c_str(),
                    error.c_str());
            return false;
        }
        printf("namp-rack: rack '%s' - %d plug-in(s), %d placeholder(s), %d skipped\n",
               name.c_str(), report.loaded, report.placeholders, report.skipped);
        audio.notifyLatencyChanged();
        return true;
    };

    if (opt.useState && !rackFromCommandLine) {
        const bool exists =
            std::find(savedRacks.begin(), savedRacks.end(), rackName) != savedRacks.end();
        if (exists)
            loadRackNamed(rackName);
        else if (!opt.rack.empty())
            fprintf(stderr, "namp-rack: no saved rack called '%s'; starting empty\n",
                    rackName.c_str());
    } else if (rackFromCommandLine && !opt.rack.empty()) {
        fprintf(stderr,
                "namp-rack: --pre/--post were given, so the saved rack '%s' was not loaded; it "
                "will still be written on exit\n",
                rackName.c_str());
    }

    audio.setChainEngine(&chainEngine);

    if (probe && !audio.open("NAMp-Rack", processor, component, &route))
        fprintf(stderr, "namp-rack: continuing without audio\n");

    // --- window and editor -------------------------------------------
    ::Display *display = XOpenDisplay(nullptr);
    if (!display) {
        fprintf(stderr, "namp-rack: cannot open the X display\n");
        return 1;
    }

    // The view is created before the window, so the window can be opened at the size the editor
    // actually wants rather than at a constant that would have to be kept in step with the panel.
    IPtr<IPlugView> view = owned(controller->createView(Vst::ViewType::kEditor));
    if (view && view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        fprintf(stderr, "namp-rack: the plug-in has no X11 editor\n");
        view = nullptr;
    }

    int editorW = kFallbackW;
    int editorH = kFallbackH;
    if (view) {
        ViewRect wanted = {};
        if (view->getSize(&wanted) == kResultTrue && wanted.getWidth() > 0 &&
            wanted.getHeight() > 0) {
            editorW = wanted.getWidth();
            editorH = wanted.getHeight();
        }
    }

    // The loop and the frame are built BEFORE the window, which they do not touch until
    // setEmbedding(): the frame is what knows how tall the strip under the editor is, and the
    // window has to be created at editor-plus-strip or the first thing the user sees is a window
    // that resizes itself. One loop for the process, one frame for this view — separate objects
    // because the two interfaces have different multiplicities; see eventloop.h.
    Rations::EventLoop eventLoop(display);
    Rations::EditorFrame frame(eventLoop);
    Rations::RackWindow rack(eventLoop, chainBuilder);

    // The strip is laid out in the editor's own logical units and drawn at the editor's own scale,
    // which is what makes the two one picture rather than two that agree at 1.0 and nowhere else.
    // From here on the frame owns the whole window's shape: it grants the editor a height, works
    // out the strip's from the width, and calls back to place it.
    frame.setStrip(
        Rations::geo::kWinW, NAMp::rackgeo::kRackH,
        [&rack](int x, int y, int w, int h, double scale) { rack.setGeometry(x, y, w, h, scale); });

    const int winW = editorW;
    const int winH = editorH + frame.stripHeightFor(editorW);

    const int screen = DefaultScreen(display);
    ::Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 0, 0, static_cast<unsigned>(winW),
        static_cast<unsigned>(winH), 0, BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "NAMp Rack");
    // So the desktop entry's StartupWMClass matches and the window gets the right icon.
    XClassHint classHint = {};
    char resName[] = "namp-rack";
    char resClass[] = "NAMp Rack";
    classHint.res_name = resName;
    classHint.res_class = resClass;
    XSetClassHint(display, window, &classHint);
    XSelectInput(display, window, StructureNotifyMask | SubstructureNotifyMask);

    Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDelete, 1);
    XMapWindow(display, window);
    XFlush(display);

    gEventLoop = &eventLoop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (view) {
        view->setFrame(&frame);
        if (view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(window)),
                           kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            fprintf(stderr, "namp-rack: the editor refused to attach\n");
            view = nullptr;
        }
    }
    // --- the rack strip ----------------------------------------------
    // Created before setEmbedding(), because that is what places it for the first time. A child of
    // the top-level and a SIBLING of whatever window the editor made inside it: the editor handles
    // its own input on its own connection, so the two paths never have to be told apart.
    rack.loadFonts(Rations::resourceDir());
    rack.setCatalog(&catalog.entries());
    rack.setSearchPaths(&searchPathRows);
    rack.setPresets(&savedRacks);
    rack.setPresetName(rackName);
    if (!rack.create(window, 0, editorH, winW, frame.stripHeightFor(winW)))
        fprintf(stderr, "namp-rack: the rack strip has no window; the amp still runs\n");

    // Only now: the run loop cannot resize a window the view has not attached to, and every page
    // change arrives as exactly that request. This is also where the window takes its one width,
    // and where the strip is put under the editor for the first time.
    if (view)
        frame.setEmbedding(window, view);

    // --- hosted editors ----------------------------------------------
    // One window per hosted plug-in, created on demand and keyed by the node's stable id. The rack
    // can reorder or remove nodes underneath these, so an index would start naming the wrong
    // plug-in the moment it did.
    std::vector<HostedEditor> editorWindows;

    // A plug-in with no editor this host can show gets the generic parameter panel instead of
    // nothing. Choosing here — once, at the only place a window is created — is what keeps every
    // other path format-agnostic.
    auto makeEditor = [&](uint64_t id, NAMp::host::PluginBackend &backend) {
        HostedEditor hosted;
        hosted.id = id;
        const bool noEditor = backend.editorKind() == NAMp::host::EditorKind::NoEditor;
        if (noEditor || opt.genericPanel) {
            hosted.panel = std::make_unique<Rations::PanelWindow>(eventLoop, backend);
            hosted.panel->loadFonts(Rations::resourceDir());
            if (noEditor)
                fprintf(stderr,
                        "namp-rack: %s has no editor this host can show; using the generic "
                        "parameter panel\n",
                        backend.displayName());
        } else {
            hosted.window = std::make_unique<Rations::PluginWindow>(eventLoop, backend);
        }
        return hosted;
    };

    auto findEditor = [&editorWindows](uint64_t id) -> HostedEditor * {
        for (HostedEditor &hosted : editorWindows)
            if (hosted.id == id)
                return &hosted;
        return nullptr;
    };

    // Called immediately BEFORE a node's instance leaves the chain. A hosted editor window holds a
    // reference to its backend, so a window left open across a removal would be pointing at an
    // object the builder is about to bury and then free.
    auto closeEditorFor = [&](uint64_t id) {
        for (size_t i = 0; i < editorWindows.size(); ++i) {
            if (editorWindows[i].id != id)
                continue;
            editorWindows[i].close();
            editorWindows.erase(editorWindows.begin() + static_cast<long>(i));
            rack.setEditorOpen(id, false);
            return;
        }
    };

    auto toggleEditorFor = [&](NAMp::host::ChainSection section, int index) {
        const uint64_t id = rack.nodeIdAt(section, index);
        if (id == 0)
            return;
        if (HostedEditor *hosted = findEditor(id)) {
            if (hosted->isOpen()) {
                closeEditorFor(id);
                return;
            }
            rack.setEditorOpen(id, hosted->open());
            return;
        }
        NAMp::host::PluginBackend *backend = chainBuilder.backend(section, index);
        if (!backend)
            return;
        HostedEditor hosted = makeEditor(id, *backend);
        const bool opened = hosted.open();
        rack.setEditorOpen(id, opened);
        if (opened)
            editorWindows.push_back(std::move(hosted));
    };

    // --- saving, and where plug-ins are looked for --------------------
    auto refreshRackList = [&]() {
        savedRacks = NAMp::host::listRacks();
        rack.setPresets(&savedRacks);
    };

    auto saveRackAs = [&](const std::string &name) {
        const std::string path = NAMp::host::rackPath(name);
        std::string error;
        if (path.empty() || !NAMp::host::saveRack(chainBuilder, path, error)) {
            fprintf(stderr, "namp-rack: cannot save the rack '%s': %s\n", name.c_str(),
                    error.empty() ? "no usable rack directory" : error.c_str());
            return;
        }
        printf("namp-rack: saved the rack to %s\n", path.c_str());
        refreshRackList();
    };

    auto deleteRackNamed = [&](const std::string &name) {
        const std::string path = NAMp::host::rackPath(name);
        if (path.empty()) {
            fprintf(stderr, "namp-rack: '%s' is not a usable rack name\n", name.c_str());
            return;
        }
        // The preset and the directory of state files beside it. remove_all on the directory
        // because a node's state may have copied whole files into it; the error_code overloads
        // because a preset that is already gone is not a failure worth reporting.
        std::error_code ec;
        const bool had = std::filesystem::remove(path, ec);
        std::filesystem::remove_all(NAMp::host::rackStateDir(path), ec);
        if (!had) {
            fprintf(stderr, "namp-rack: cannot delete the rack '%s': %s\n", name.c_str(),
                    ec ? ec.message().c_str() : "no such rack");
            return;
        }
        printf("namp-rack: deleted the rack %s\n", path.c_str());
        refreshRackList();
    };

    // A scan is synchronous and can take seconds when a bundle has changed, so it draws its own
    // progress: the callback paints and blits the strip directly, because the run loop is inside
    // the scan and will not tick again until it returns.
    auto scanPlugins = [&]() {
        rack.showScanProgress(0, 0, std::string());
        catalog.rescan([&](const NAMp::host::ScanProgress &progress) {
            rack.showScanProgress(progress.index, progress.total, progress.current);
        });
        rack.endScanProgress();
        printf("namp-rack: %zu plug-in(s); %d probed; %d new\n", catalog.entries().size(),
               catalog.probedCount(), catalog.newCount());
        rack.invalidate();
    };

    auto addSearchPath = [&](const std::string &dir) {
        std::string error;
        if (!pluginPaths.add(dir, error)) {
            fprintf(stderr, "namp-rack: cannot search %s: %s\n", dir.c_str(), error.c_str());
            return;
        }
        if (!pluginPaths.save(pluginPathsFile))
            fprintf(stderr, "namp-rack: could not write %s\n", pluginPathsFile.c_str());
        searchPathRows = buildSearchPathRows(pluginPaths);
        rack.setSearchPaths(&searchPathRows);
        // Scanned straight away rather than waiting for the user to press Scan: they have just
        // pointed at a folder, and the only reason to do that is to get what is in it. Requested
        // rather than run, because this is reached from a click and nothing may paint there.
        rack.requestScan();
    };

    auto removeSearchPath = [&](const std::string &dir) {
        if (!pluginPaths.remove(dir))
            return;
        if (!pluginPaths.save(pluginPathsFile))
            fprintf(stderr, "namp-rack: could not write %s\n", pluginPathsFile.c_str());
        searchPathRows = buildSearchPathRows(pluginPaths);
        rack.setSearchPaths(&searchPathRows);
        // A rescan is what actually drops those plug-ins out of the picker; without it the folder
        // is gone from the list and its contents are still offered, which is worse than either.
        rack.requestScan();
    };

    rack.setEditorToggle(toggleEditorFor);
    rack.setEditorClose(closeEditorFor);
    rack.setPresetHandlers(loadRackNamed, saveRackAs, deleteRackNamed);
    rack.setDiscoveryHandlers(scanPlugins, addSearchPath, removeSearchPath);
    rack.setChainChanged([&audio]() { audio.notifyLatencyChanged(); });
    rack.refreshModel();

    if (opt.showEditors || opt.editorCycles > 0) {
        // Pre-created for whatever the command line or the saved rack put in the chain, so
        // --editors and --editor-cycles have something to drive before the rack has been touched.
        for (const auto section : {NAMp::host::ChainSection::Pre, NAMp::host::ChainSection::Post}) {
            const int nodes = chainBuilder.count(section);
            for (int i = 0; i < nodes; ++i) {
                NAMp::host::PluginBackend *backend = chainBuilder.backend(section, i);
                if (!backend)
                    continue;
                editorWindows.push_back(makeEditor(rack.nodeIdAt(section, i), *backend));
            }
        }
    }
    if (opt.showEditors) {
        for (auto &hosted : editorWindows)
            rack.setEditorOpen(hosted.id, hosted.open());
    }

    FeedbackPump feedback(audio, controller);
    eventLoop.registerTimer(&feedback, kUiTickMs);

    RackTicker rackTicker(rack, audio);
    eventLoop.registerTimer(&rackTicker, kUiTickMs);

    EditorPump editorPump(editorWindows, &rack);
    eventLoop.registerTimer(&editorPump, kUiTickMs);

    ChainCollector collector(chainBuilder);
    eventLoop.registerTimer(&collector, kUiTickMs);

    RackStress rackStress(chainBuilder, audio, eventLoop, stressRef, opt.rackStressSeconds,
                          opt.rackStressByEnable);
    if (opt.rackStressSeconds > 0.0) {
        if (!stressRef.valid()) {
            fprintf(stderr, "namp-rack: --rack-stress needs a --post plug-in to churn\n");
            return 2;
        }
        if (!audio.isOpen()) {
            fprintf(stderr, "namp-rack: --rack-stress needs audio; there is no JACK server\n");
            return 2;
        }
        eventLoop.registerTimer(&rackStress, kUiTickMs);
    }

    BufferSizeWatcher blockWatcher(audio, component, processor, setup, chainEngine, chainBuilder);
    if (audio.isOpen())
        eventLoop.registerTimer(&blockWatcher, kUiTickMs);

    EditorCycler cycler(editorWindows, eventLoop, opt.editorCycles);
    if (opt.editorCycles > 0) {
        if (editorWindows.empty()) {
            fprintf(stderr, "namp-rack: --editor-cycles needs at least one plug-in in the rack\n");
            return 2;
        }
        eventLoop.registerTimer(&cycler, kUiTickMs);
    }

    // Registered against THIS window rather than as a single global callback: the loop dispatches
    // by XEvent::xany.window, so a second top-level cannot end up in the same handler.
    //
    // THE ConfigureNotify CHECK IS STILL NEEDED, and dropping it as redundant is a bug that
    // presents as an infinite resize loop. Dispatch matches xany.window, which for a
    // ConfigureNotify is the xconfigure.EVENT field — "window on which event was requested in event
    // mask" (X11/Xlib.h) — and this window selected SubstructureNotifyMask, so it is also told when
    // its CHILDREN resize. On those the event field is this window and matches, while
    // xconfigure.window is the child and xconfigure.width/height are the CHILD's new size. Feeding
    // that back as the top-level's size makes the editor resize its child to fit a size that was
    // its child's, and the two then oscillate for as long as the program runs. Measured: 800x285
    // and 748x266 alternating forever.
    eventLoop.addWindow(window, [&](const XEvent &event) {
        if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wmDelete)
            eventLoop.stop();
        else if (event.type == ConfigureNotify && event.xconfigure.window == window)
            frame.windowConfigured(event.xconfigure.width, event.xconfigure.height);
    });

    eventLoop.run();

    // --- teardown ----------------------------------------------------
    if (opt.editorCycles > 0)
        eventLoop.unregisterTimer(&cycler);
    if (audio.isOpen())
        eventLoop.unregisterTimer(&blockWatcher);
    if (opt.rackStressSeconds > 0.0)
        eventLoop.unregisterTimer(&rackStress);
    eventLoop.unregisterTimer(&editorPump);
    eventLoop.unregisterTimer(&rackTicker);
    eventLoop.unregisterTimer(&collector);
    eventLoop.unregisterTimer(&feedback);

    // EVERY HOSTED EDITOR GOES FIRST, AND IN ITS OWN ORDER. close() tells the plug-in to shut its
    // editor down before this host stops dispatching to the window and long before the window is
    // destroyed — the view unregisters its own run-loop handlers from inside that call, so the loop
    // and the frame have to still be alive when it happens. Destroying the window first leaves a
    // plug-in's timer firing against a window that no longer exists, which is a crash inside
    // somebody else's code with our stack nowhere in the backtrace.
    for (auto &hosted : editorWindows)
        hosted.close();
    editorWindows.clear();

    // The strip lets go of its own window before the top-level it is a child of is destroyed, for
    // the same reason and in the same order.
    rack.destroy();

    if (view) {
        view->removed();
        view = nullptr;
    }
    // The measurement, on every run rather than behind a flag. It is one line, it costs nothing,
    // and whether the amp fits inside the audio callback at this machine's buffer size is not a
    // question anyone can answer by reading the source.
    if (audio.isOpen()) {
        const uint32_t drops = audio.dropouts();
        printf("namp-rack: %u dropout%s at %d frames\n", drops, drops == 1 ? "" : "s",
               audio.blockSize());
    }

    if (opt.editorCycles > 0)
        printf("namp-rack: %d editor open/close cycle%s, %d editor%s per cycle%s\n",
               cycler.completedCycles(), cycler.completedCycles() == 1 ? "" : "s",
               cycler.openedPerCycle(), cycler.openedPerCycle() == 1 ? "" : "s",
               cycler.finished() ? "" : " (interrupted)");

    audio.close();
    // The audio thread is gone, so nothing can still be holding a snapshot: collectAll frees
    // everything outstanding whether or not the engine handed it back, which collect() alone
    // cannot do because it has no way to know the thread has stopped.
    audio.setChainEngine(nullptr);
    chainEngine.abandon();
    chainBuilder.collectAll();

    processor->setProcessing(false);
    component->setActive(false);
    // With the audio thread gone and the component inactive, so nothing is moving underneath the
    // blob being written.
    if (opt.useState) {
        saveState(component);
        // The rack too, and after the audio thread has stopped for the same reason: nothing is
        // moving underneath what is being written. A chain the user built and did not explicitly
        // save is still the chain they were using.
        std::string rackError;
        const std::string path = NAMp::host::rackPath(rackName);
        if (path.empty() || !NAMp::host::saveRack(chainBuilder, path, rackError))
            fprintf(stderr, "namp-rack: could not save the rack '%s': %s\n", rackName.c_str(),
                    rackError.empty() ? "no usable rack directory" : rackError.c_str());
    }
    controller->setComponentHandler(nullptr);

    // The window stops being dispatched to BEFORE it is destroyed, so nothing can be handed an
    // event for a window id that no longer names anything.
    eventLoop.removeWindow(window);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    gEventLoop = nullptr;
    // The provider owns the component and the controller, and both must be gone before the module
    // that produced them is unloaded. Released here rather than left to scope exit, because
    // `module` is declared above it and would otherwise be destroyed first.
    provider = nullptr;
    factory.reset();
    module = nullptr;
    // Retract the host context before it leaves scope, so nothing can reach a dangling pointer
    // during static destruction.
    Vst::PluginContextFactory::instance().setPluginContext(nullptr);
    return 0;
}
