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
#include "runloop.h"

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

Rations::RunLoop *gRunLoop = nullptr;

void onSignal(int)
{
    if (gRunLoop)
        gRunLoop->stop();
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
                      Vst::IAudioProcessor *processor, const Vst::ProcessSetup &setup)
        : mAudio(audio), mComponent(component), mProcessor(processor), mSetup(setup)
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
        // Only adopt the new chunk size if the plug-in accepted it. If it did not, the old size is
        // still what it is prepared for, and the chunk loop must keep honouring that.
        mAudio.resumeProcessing(ok ? size : mSetup.maxSamplesPerBlock);

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
struct Options {
    // An external .vst3 to host INSTEAD of the amp linked in. Empty is the normal case. It exists
    // so a build of the amp made somewhere else can be driven by this rig without reinstalling
    // anything, which is how a bug that only appears in the bundle gets reproduced.
    std::string bundle;
    bool useState = true;
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

    Options opt;
    switch (parseArgs(argc, argv, opt)) {
        case ArgResult::Exit:
            return 0;
        case ArgResult::Error:
            return 2;
        case ArgResult::Run:
            break;
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

    Rations::RunLoop runLoop(display);
    gRunLoop = &runLoop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (view) {
        view->setFrame(&runLoop);
        if (view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(window)),
                           kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            fprintf(stderr, "namp-rack: the editor refused to attach\n");
            view = nullptr;
        }
    }
    // Only now: the run loop cannot resize a window the view has not attached to, and every page
    // change arrives as exactly that request. This is also where the window takes its one width.
    if (view)
        runLoop.setEmbedding(window, view);

    FeedbackPump feedback(audio, controller);
    runLoop.registerTimer(&feedback, kUiTickMs);

    BufferSizeWatcher blockWatcher(audio, component, processor, setup);
    if (audio.isOpen())
        runLoop.registerTimer(&blockWatcher, kUiTickMs);

    runLoop.setXEventCallback([&](const XEvent &event) {
        if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wmDelete)
            runLoop.stop();
        else if (event.type == ConfigureNotify && event.xconfigure.window == window)
            runLoop.windowConfigured(event.xconfigure.width, event.xconfigure.height);
    });

    runLoop.run();

    // --- teardown ----------------------------------------------------
    if (audio.isOpen())
        runLoop.unregisterTimer(&blockWatcher);
    runLoop.unregisterTimer(&feedback);
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
    processor->setProcessing(false);
    component->setActive(false);
    // With the audio thread gone and the component inactive, so nothing is moving underneath the
    // blob being written.
    if (opt.useState)
        saveState(component);
    controller->setComponentHandler(nullptr);

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    gRunLoop = nullptr;
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
