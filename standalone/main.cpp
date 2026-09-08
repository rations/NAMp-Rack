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
#include "eventloop.h"

#include "host/catalog.h"
#include "host/chainbuilder.h"
#include "host/chainengine.h"
#include "host/scanchild.h"

#include "gfx/resourcestore.h"
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

    // Answered before the amp, the audio device or the window exist, because it needs none of them
    // and a scan that had to open a JACK connection first would be a scan nobody could run.
    if (opt.listPlugins) {
        NAMp::host::Catalog catalog;
        catalog.rescan();
        for (const auto &entry : catalog.entries())
            printf("%-5s %-40s %s\n", NAMp::host::formatTag(entry.ref.format), entry.name.c_str(),
                   entry.ref.key.c_str());
        printf("\n%zu plug-in(s)\n", catalog.entries().size());
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

    if (!opt.pre.empty() || !opt.post.empty()) {
        NAMp::host::Catalog catalog;
        catalog.rescan();

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

    int winW = kFallbackW;
    int winH = kFallbackH;
    if (view) {
        ViewRect wanted = {};
        if (view->getSize(&wanted) == kResultTrue && wanted.getWidth() > 0 &&
            wanted.getHeight() > 0) {
            winW = wanted.getWidth();
            winH = wanted.getHeight();
        }
    }

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

    // One loop for the process, one frame for this view. They are separate objects because the
    // two interfaces have different multiplicities — see eventloop.h.
    Rations::EventLoop eventLoop(display);
    Rations::EditorFrame frame(eventLoop);
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
    // Only now: the run loop cannot resize a window the view has not attached to, and every page
    // change arrives as exactly that request. This is also where the window takes its one width.
    if (view)
        frame.setEmbedding(window, view);

    FeedbackPump feedback(audio, controller);
    eventLoop.registerTimer(&feedback, kUiTickMs);

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
    if (audio.isOpen())
        eventLoop.unregisterTimer(&blockWatcher);
    if (opt.rackStressSeconds > 0.0)
        eventLoop.unregisterTimer(&rackStress);
    eventLoop.unregisterTimer(&collector);
    eventLoop.unregisterTimer(&feedback);
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
    if (opt.useState)
        saveState(component);
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
