# NAMp-Rack — an LV2/VST3 plug-in host with a four-channel Neural Amp Modeler amp head built in.
#
# No plug-in or GUI framework: the amp is a raw Steinberg IPlugView painted with Cairo/FreeType,
# and the rack hosts other people's plug-ins around it in one serial chain. The DSP core is the
# MIT-licensed NeuralAmpModelerCore + AudioDSPTools by Steven Atkinson, with Eigen (MPL2) as the
# linear-algebra backend.
#
# PLATFORMS. Linux is the whole product: the standalone host, LV2 and VST3 hosting, JACK audio.
# Windows hosts VST3 only and takes its audio from WASAPI — the LV2 stack (lilv/suil/serd/sord/
# sratom/zix) is Linux-shaped here, so it sits behind its own switch rather than being ifdef'd
# through the source. That split is structural, not a convenience.
#
# BUILD STATE: scaffolding phase. This file resolves and validates every dependency and declares
# the build's shape; the targets themselves arrive with the source in the next phase. Configuring
# it is the check that the tree a fresh clone gets is one the build can actually find its way
# around.

cmake_minimum_required(VERSION 3.25.0)

project(namp-rack
    VERSION 0.1.0
    DESCRIPTION "NAMp-Rack — plug-in host with a four-channel NAM amp head"
    LANGUAGES C CXX
)

set(NAMPRACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# LV2 hosting is Linux-only by construction, so the default follows the platform rather than being
# a preference anyone has to remember to set.
if(UNIX AND NOT APPLE AND NOT WIN32)
    set(_namprack_lv2_default ON)
else()
    set(_namprack_lv2_default OFF)
endif()

option(NAMPRACK_BUILD_LV2_HOST "Host LV2 plug-ins (Linux only)" ${_namprack_lv2_default})
option(NAMPRACK_BUILD_TOOLS    "Build the offline proof and render tools" ON)
option(NAMPRACK_BUILD_PEDALS   "Build the bundled pedals from the submodule" ON)

# The amp is also built as a VST3 bundle that is NOT part of the shipped product. It exists so the
# SDK validator has something to validate, which is by a wide margin the cheapest gate on the amp;
# the release script does not package it. Turning this off costs that gate and leaves the offline
# proofs as the amp's only check.
option(NAMPRACK_BUILD_VST3_BUNDLE "Build the amp as a VST3 bundle for validator coverage" ON)

if(NAMPRACK_BUILD_LV2_HOST AND WIN32)
    message(FATAL_ERROR
        "NAMPRACK_BUILD_LV2_HOST is ON but the target is Windows. LV2 hosting needs lilv, suil "
        "and jalv's event buffer, none of which is part of the Windows build; Windows hosts VST3 "
        "only. Configure with -DNAMPRACK_BUILD_LV2_HOST=OFF.")
endif()

# ---------------------------------------------------------------------------
# Dependency source trees.
#
# Each defaults to the in-tree pinned directory and can be pointed at an external checkout
# instead, which is how development against pristine reference trees works without initialising
# submodules. These trees are read-only: a local edit inside one is silently reverted by the next
# update and leaves a fresh clone unable to reproduce the build.
# ---------------------------------------------------------------------------
set(NAM_CORE_DIR "${NAMPRACK_ROOT}/NeuralAmpModelerCore"
    CACHE PATH "NeuralAmpModelerCore source tree (v0.5.5 or newer)")
set(AUDIO_DSP_TOOLS_DIR "${NAMPRACK_ROOT}/AudioDSPTools"
    CACHE PATH "AudioDSPTools source tree")
set(EIGEN_DIR "${NAMPRACK_ROOT}/eigen"
    CACHE PATH "Eigen source tree (the directory containing Eigen/)")

foreach(_dep NAM_CORE_DIR AUDIO_DSP_TOOLS_DIR EIGEN_DIR)
    if(NOT IS_DIRECTORY "${${_dep}}")
        message(FATAL_ERROR
            "${_dep} does not name a directory ('${${_dep}}') - run "
            "'git submodule update --init --recursive' or set -D${_dep}=<path>")
    endif()
endforeach()

foreach(_probe "${NAM_CORE_DIR}/NAM/model_config.h"
               "${AUDIO_DSP_TOOLS_DIR}/dsp/ImpulseResponse.h"
               "${EIGEN_DIR}/Eigen/Core")
    if(NOT EXISTS "${_probe}")
        message(FATAL_ERROR
            "expected dependency header not found: ${_probe}\n"
            "The tree is present but is not the one this build expects. Check the path, and see "
            "the NAM core version requirement below.")
    endif()
endforeach()

# The NAM core must be v0.5.5 or newer. The amp builds submodels directly through the public
# parse_model_config_json()/create_dsp() pair and reads DSP::GetPrewarmSamples(); in v0.5.3 that
# accessor is named PrewarmSamples(), sits in a protected section, and NAM/linear.h,
# DspLoadOptions and ScopedPrewarmOnResetDefault do not exist. Both versions are commonly sitting
# side by side on a development machine, and pointing this at the wrong one produces code that
# reads correctly and does not compile — or, worse, compiles against different semantics.
#
# Probe for what ACTUALLY differs. NAM/model_config.h is NOT a discriminator: it is present in
# v0.5.3 too, which was measured on two v0.5.3 checkouts rather than assumed. The two reliable
# markers are NAM/linear.h and the GetPrewarmSamples rename.
if(NOT EXISTS "${NAM_CORE_DIR}/NAM/linear.h")
    message(FATAL_ERROR
        "NAM_CORE_DIR ('${NAM_CORE_DIR}') has no NAM/linear.h, so it is older than v0.5.5. "
        "This build requires the public parse_model_config_json()/create_dsp() pair, "
        "DspLoadOptions, and a public DSP::GetPrewarmSamples(). Point -DNAM_CORE_DIR at a "
        "v0.5.5-or-newer checkout.")
endif()
file(STRINGS "${NAM_CORE_DIR}/NAM/dsp.h" _namprack_prewarm REGEX "GetPrewarmSamples")
if(NOT _namprack_prewarm)
    message(FATAL_ERROR
        "NAM_CORE_DIR ('${NAM_CORE_DIR}') declares no GetPrewarmSamples() in NAM/dsp.h - it has "
        "the older protected PrewarmSamples() instead, so this is a pre-0.5.5 tree. The "
        "crossfade and channel-switch engines need the prewarm length to prime an incoming "
        "model; without it a channel switch is a click. Point -DNAM_CORE_DIR at a "
        "v0.5.5-or-newer tree.")
endif()

# ---------------------------------------------------------------------------
# VST 3 SDK — used on BOTH sides here, unlike in either parent project: the plug-in-side classes
# build the amp, and the hosting classes (Module, PlugProvider, ParameterChanges, EventList,
# HostClasses) run the rack.
# ---------------------------------------------------------------------------
if(NOT TARGET sdk)
    set(VST3_SDK_DIR "${NAMPRACK_ROOT}/vst3sdk"
        CACHE PATH "VST 3 SDK source tree")
    if(NOT EXISTS ${VST3_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "VST 3 SDK not found at '${VST3_SDK_DIR}' - run "
            "'git submodule update --init --recursive' or set -DVST3_SDK_DIR=<path>")
    endif()

    # Headless SDK: core libraries plus the validator (part of the hosting examples), no VSTGUI
    # (the editor is drawn directly), no plug-in examples.
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "editor is drawn directly" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "plug-in examples not needed" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install manually instead" FORCE)

    # Defaults ON, and its Windows implementation runs a freshly built moduleinfotool.exe as a
    # POST_BUILD step — a Windows binary when cross-compiling, so it cannot execute on the build
    # host. The file is optional (Module::getModuleInfoPath returns an empty optional when it is
    # absent, and the validator does not require it), and the release script generates it under
    # Wine instead, where the same binary does run.
    if(CMAKE_CROSSCOMPILING)
        set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "generated under Wine instead" FORCE)
    endif()

    add_subdirectory(${VST3_SDK_DIR} vst3sdk)

    # The SDK's plugin helper functions read these directory-scoped variables
    # (smtg_target_add_library_main silently adds no module entry point without
    # public_sdk_SOURCE_DIR); the SDK sets them only in its own scope.
    set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    set(pluginterfaces_SOURCE_DIR "${VST3_SDK_DIR}/pluginterfaces")

    # public.sdk/source/vst/utility/dataexchange.cpp does not compile with MinGW. It reaches
    # alignedalloc.h, which picks its allocator on `#elif defined(_MSC_VER)` -> _aligned_malloc,
    # `#else` -> std::aligned_alloc. MinGW is neither: it defines no _MSC_VER, and its msvcrt has
    # no C11 aligned_alloc (verified — the CRT offers _aligned_malloc and __mingw_aligned_malloc
    # only), so the else branch fails to compile. The test is really "is this the Microsoft CRT",
    # not "is this the Microsoft compiler", which is an upstream bug we must not fix in place: the
    # SDK is a pinned dependency and a local edit would be reverted by the next update, leaving a
    # fresh clone unable to build for Windows.
    #
    # Dropping the file instead is safe because nothing here uses the feature it implements:
    # DataExchangeHandler is the plug-in side of VST 3.7.9's IDataExchangeHandler, and no source
    # in this tree mentions it. vstinitiids.cpp still defines the IIDs, so the interface itself
    # remains declared.
    if(WIN32)
        get_target_property(_sdk_sources sdk SOURCES)
        list(FILTER _sdk_sources EXCLUDE REGEX "utility/dataexchange\\.cpp$")
        set_target_properties(sdk PROPERTIES SOURCES "${_sdk_sources}")
        message(STATUS "[NAMp-Rack] dropped dataexchange.cpp from the SDK "
                       "(unused, and MinGW has no std::aligned_alloc)")

        # The SDK's Windows command-line tools define wmain, not main. MinGW only wires wmain up
        # to the CRT entry point when -municode is on the LINK line; without it the linker falls
        # back to libmingw32's crtexewin.o, whose main() calls WinMain, and the build dies on an
        # undefined WinMain. MSVC needs none of this, which is why upstream does not set it.
        foreach(_tool validator moduleinfotool)
            if(TARGET ${_tool})
                target_link_options(${_tool} PRIVATE -municode)
            endif()
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform libraries
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

if(NOT WIN32)
    # The graphics stack and JACK are system shared libraries on Linux: linked at run time and not
    # redistributed. See NOTICE.
    pkg_check_modules(NAMPRACK_GFX REQUIRED IMPORTED_TARGET
        cairo freetype2 fontconfig x11)
    pkg_check_modules(NAMPRACK_JACK REQUIRED IMPORTED_TARGET jack)
endif()

# The SDK's bundle loader, which is per-platform. Named once here rather than at each consumer:
# the standalone and the three bundle-driving tools all need it, and picking it in two places is
# how they end up disagreeing.
if(WIN32)
    set(NAMPRACK_MODULE_LOADER_SRC
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
else()
    set(NAMPRACK_MODULE_LOADER_SRC
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

if(NAMPRACK_BUILD_LV2_HOST)
    # LV2 hosting. lilv reads the RDF world, suil instantiates a plug-in's own UI, and the lv2
    # headers are the vocabulary both speak.
    #
    # The SYSTEM libraries are linked, not the local reference sources, and that is a decision
    # rather than a default. There is real version skew between the two — the reference tree runs
    # ahead of the packaged libraries by four minor versions on lilv — so one had to be picked
    # rather than straddled. Picking the system copy is safe because the delta was MEASURED, not
    # assumed: the two lilv headers declare an identical function set except for
    # lilv_state_get_bundle_path, which is newer-only and is not used here; suil's public header
    # is byte-identical; and every LV2 URI define this code uses is the same in both, the only
    # differences being include-guard renames and the Gtk4UI/Qt6UI additions of a later lv2. The
    # reference sources stay the read-only authority for semantics and for jalv's implementation
    # patterns, in the same role the pristine checkout plays for the VST3 SDK. Re-measure if
    # either side moves.
    #
    # Prefer lilv_file_uri_parse over the deprecated lilv_uri_to_path.
    pkg_check_modules(LV2HOST REQUIRED IMPORTED_TARGET
        lv2>=1.18.0 lilv-0>=0.24.0 suil-0>=0.10.0)
endif()

# ---------------------------------------------------------------------------
# Visibility, and one GCC flag the presets cannot reach
# ---------------------------------------------------------------------------
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# The presets above cannot reach the standard library, and on GCC 12 that is not hypothetical:
# libstdc++ declares `namespace std` with _GLIBCXX_VISIBILITY(default), an explicit attribute, and
# an explicit attribute beats -fvisibility=hidden. So std::__detail::__to_chars_10_impl<T>::__digits
# — a function-local static in an inline template — reaches the dynamic table with STB_GNU_UNIQUE
# binding no matter what the presets say, and the packaging script's unique-symbol gate rejects the
# result.
#
# -fno-gnu-unique makes GCC emit STB_GLOBAL for those statics instead. That removes the binding
# itself rather than hiding one instance of it, which is the property that matters here: a unique
# symbol is resolved through a table dlopen's RTLD_LOCAL does not scope. That is a far sharper
# concern in this project than in the one it was written for, because this binary dlopen's other
# people's plug-ins — two copies of one symbol resolving to each other across that boundary is a
# crash with nothing in the backtrace to explain it.
#
# GCC 13 reworked __to_chars_10_impl and the statics went away, so a newer toolchain never produces
# them; the flag is a no-op there rather than a behaviour change. Clang never emits STB_GNU_UNIQUE
# and does not take the flag.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-fno-gnu-unique)
endif()

# ---------------------------------------------------------------------------
# The graphics library: the hand-painted editor's Cairo/FreeType canvas.
# ---------------------------------------------------------------------------
pkg_check_modules(CAIROFT REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2 fontconfig)
if(NOT WIN32)
    pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)
    set(NAMPRACK_PLATFORM_GUI_LIBS PkgConfig::CAIROX11)
    set(NAMPRACK_PLATFORM_VIEW_SOURCES
        ${NAMPRACK_ROOT}/src/platform/x11plugview.h
        ${NAMPRACK_ROOT}/src/platform/x11plugview.cpp)
else()
    set(NAMPRACK_PLATFORM_GUI_LIBS "")
    set(NAMPRACK_PLATFORM_VIEW_SOURCES
        ${NAMPRACK_ROOT}/src/platform/win32plugview.h
        ${NAMPRACK_ROOT}/src/platform/win32plugview.cpp)
endif()

add_library(NampRackGfx STATIC
    ${NAMPRACK_ROOT}/src/gfx/canvas.cpp
    ${NAMPRACK_ROOT}/src/gfx/fontstack.cpp
    ${NAMPRACK_ROOT}/src/gfx/image.cpp
    ${NAMPRACK_ROOT}/src/gfx/svg.cpp
    ${NAMPRACK_ROOT}/src/gfx/resourcestore.h
    ${NAMPRACK_ROOT}/src/gfx/resourcestore.cpp
    ${NAMPRACK_ROOT}/src/platform/respath.cpp
)
target_include_directories(NampRackGfx PUBLIC ${NAMPRACK_ROOT}/src)
target_link_libraries(NampRackGfx PUBLIC PkgConfig::CAIROFT ${CMAKE_DL_LIBS})
target_compile_features(NampRackGfx PUBLIC cxx_std_17)
set_target_properties(NampRackGfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -Wall for our own code; the vendored NanoSVG headers are compiled as-is.
target_compile_options(NampRackGfx PRIVATE -Wall)

# ---------------------------------------------------------------------------
# The DSP: the NAM core, AudioDSPTools and the tone stack.
# ---------------------------------------------------------------------------
add_library(NampRackDsp STATIC
    ${NAM_CORE_DIR}/NAM/activations.cpp
    ${NAM_CORE_DIR}/NAM/container.cpp
    ${NAM_CORE_DIR}/NAM/conv1d.cpp
    ${NAM_CORE_DIR}/NAM/convnet.cpp
    ${NAM_CORE_DIR}/NAM/dsp.cpp
    ${NAM_CORE_DIR}/NAM/get_dsp.cpp
    ${NAM_CORE_DIR}/NAM/linear.cpp
    ${NAM_CORE_DIR}/NAM/lstm.cpp
    ${NAM_CORE_DIR}/NAM/ring_buffer.cpp
    ${NAM_CORE_DIR}/NAM/util.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/model.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/a2_fast.cpp
    ${NAM_CORE_DIR}/NAM/wavenet/slimmable.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/dsp.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/ImpulseResponse.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/NoiseGate.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/RecursiveLinearFilter.cpp
    ${AUDIO_DSP_TOOLS_DIR}/dsp/wav.cpp
    ${NAMPRACK_ROOT}/deps/tonestack/ToneStack.cpp
)

# What it takes to COMPILE against those headers, held on an INTERFACE library separate from the
# archive. Keeping the two apart is what lets a consumer that links the archive with
# --whole-archive still receive the include paths, the definitions and the language level: the
# whole-archive spelling puts a FILE on the link line rather than naming a target, and every usage
# requirement attached to the target would otherwise be silently dropped. The first symptom is a
# missing NAM/dsp.h, which merely fails to build. The second is worse and would fail nothing:
# without NAM_ENABLE_A2_FAST the A2 fast path is compiled out, and that path is the whole basis of
# the click-free gain crossfade and the instant channel switch.
add_library(NampRackDspHeaders INTERFACE)
target_compile_definitions(NampRackDspHeaders INTERFACE NAM_ENABLE_A2_FAST)
target_include_directories(NampRackDspHeaders INTERFACE
    ${NAM_CORE_DIR}
    ${NAM_CORE_DIR}/NAM
    ${NAM_CORE_DIR}/Dependencies/nlohmann
    ${AUDIO_DSP_TOOLS_DIR}/dsp
    # And its parent, so a header outside that directory can say "dsp/dsp.h" and mean
    # AudioDSPTools' rather than NAM's. Both trees have a dsp.h and NAM's include dir is listed
    # first, so a bare "dsp.h" reaches the wrong one — with an error about DSP_SAMPLE that says
    # nothing about which of two identically-named headers arrived.
    ${AUDIO_DSP_TOOLS_DIR}
    ${EIGEN_DIR}
    ${NAMPRACK_ROOT}/deps/tonestack
)

# C++20: NAM's slimmable wavenet uses std::atomic<std::shared_ptr>, a C++20 library feature. The
# plug-in and the host stay at the SDK's C++17 baseline.
target_compile_features(NampRackDspHeaders INTERFACE cxx_std_20)

# AudioDSPTools' ResamplingContainer.h declares, at class scope,
#
#     using LanczosResampler = LanczosResampler<T, NCHANS, A>;
#
# which redeclares a name already used in that scope to mean something else. [basic.scope.class]p2
# forbade that, and GCC enforces it as a permerror — an error only -fpermissive downgrades. GCC 14
# implemented P1787R6, which deleted the rule, so the same header compiles clean there and the
# diagnostic is a pre-14 artefact rather than a defect in the code.
#
# INTERFACE because it belongs to the header, not to this library: the AudioDSPTools include path
# above is published here, so every consumer that includes the resampler meets the same permerror
# and each would otherwise have to rediscover the flag. Whoever publishes a header publishes what
# compiling it takes.
#
# Version-gated rather than unconditional, so the loosened checking disappears on a compiler that
# does not need it instead of quietly outliving its reason. The upstream tree is pinned, so
# patching the line there would be undone by the next update.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    target_compile_options(NampRackDspHeaders INTERFACE -fpermissive)
endif()

target_link_libraries(NampRackDsp PUBLIC NampRackDspHeaders)
if(WIN32)
    # -fPIC has no meaning for PE and MinGW warns that it is being ignored; code in a Windows DLL
    # is relocated by the loader, not by position-independent codegen.
    target_compile_options(NampRackDsp PRIVATE -ffast-math -Wno-unused-parameter)
else()
    target_compile_options(NampRackDsp PRIVATE -ffast-math -fPIC -Wno-unused-parameter)
endif()

# Link the DSP archive WHOLE. Each network architecture registers itself through a file-scope
# static initialiser, and the linker drops those object files from a normal static link because
# nothing references them by name. The symptom is a clean build that fails at run time with
# "No config parser registered for architecture: WaveNet".
function(namprack_link_whole_dsp target)
    target_link_libraries(${target}
        PRIVATE -Wl,--whole-archive NampRackDsp -Wl,--no-whole-archive)
    target_link_libraries(${target} PRIVATE NampRackDspHeaders)
endfunction()

# ---------------------------------------------------------------------------
# The amp, as a VST3 bundle.
#
# NOT A SHIPPED PRODUCT. The product is the standalone host; this target exists so the SDK
# validator has something to validate, which is by a wide margin the cheapest gate on the amp, and
# the release script does not package it. See NAMPRACK_BUILD_VST3_BUNDLE.
# ---------------------------------------------------------------------------
set(NAMPRACK_PLUGIN_SOURCES
    src/rationsids.h
    src/version.h
    src/geometry.h
    src/rationsprocessor.h
    src/rationsprocessor.cpp
    src/rationscontroller.h
    src/rationscontroller.cpp
    # The state blob's string reads, shared by the two functions above that decode one.
    src/statestream.h
    src/rationsfactory.cpp
    src/engineconfig.h
    src/spscqueue.h
    src/bank.h
    src/nativeresampler.h
    src/capturesource.h
    src/capturesource.cpp
    src/modelbank.h
    src/modelbank.cpp
    src/crossfadeengine.h
    src/crossfadeengine.cpp
    src/channelrack.h
    src/channelrack.cpp
    src/irblend.h
    src/irblend.cpp
    src/midilearn.h
    src/midilearn.cpp
    src/rationsview.h
    src/rationsview.cpp
    src/filebrowser.h
    src/filebrowser.cpp
    src/platform/plugview.h
    ${NAMPRACK_PLATFORM_VIEW_SOURCES}
)

# ---------------------------------------------------------------------------
# Art and fonts, named once.
#
# ONE list, three consumers: the validator bundle copies these into Contents/Resources, the embed
# generator turns the same files into the standalone's built-in fallback set, and panelrender's
# audit fails if one is missing. Naming them once is what keeps a layer that is drawn from ever
# being absent from the binary that draws it.
#
# The *-base.png files in the same directory are the untrimmed source exports that gui/ trims these
# out of. They are not listed because they are not shipped.
# ---------------------------------------------------------------------------
set(NAMPRACK_FONT_FILES
    fonts/Michroma-Regular.ttf
    fonts/Roboto-Regular.ttf
)
set(NAMPRACK_IMG_FILES
    img/head.png
    img/cabinet.png
    img/namp-badge.png
    img/dial.png
    img/led_on.png
    img/led_off.png
    img/switch_up_ring.png
    img/switch_down_ring.png
    img/meter_track.png
    img/Folder.svg
    img/File.svg
    img/Cross.svg
    img/ArrowLeft.svg
    img/ArrowRight.svg
    img/SlimmableIcon.svg
    # Not drawn by the amp's editor. It is the per-node settings icon in the rack strip, which is
    # a later phase; it ships now because it is a committed asset of this project and leaving it
    # out would make landing the rack a packaging change as well as a code one.
    img/Gear.svg
)
set(NAMPRACK_RESOURCE_FILES ${NAMPRACK_FONT_FILES} ${NAMPRACK_IMG_FILES})

set(NAMPRACK_FONT_PATHS "")
foreach(_res IN LISTS NAMPRACK_FONT_FILES)
    list(APPEND NAMPRACK_FONT_PATHS "${NAMPRACK_ROOT}/resources/${_res}")
endforeach()
set(NAMPRACK_IMG_PATHS "")
foreach(_res IN LISTS NAMPRACK_IMG_FILES)
    list(APPEND NAMPRACK_IMG_PATHS "${NAMPRACK_ROOT}/resources/${_res}")
endforeach()

# ---------------------------------------------------------------------------
# The amp's own code, as a library.
#
# It is a library rather than a source list belonging to the bundle because the standalone links it
# directly. The alternative — having the standalone dlopen an INSTALLED bundle, which is what the
# amp's own parent standalone did — makes a self-contained product impossible: nothing in ~/.vst3,
# nothing to run. Linking the same sources into both targets means the standalone is one file and
# can never load a DIFFERENT, older amp than the one it was just built from.
#
# Compiled twice rather than shared as objects on purpose: the bundle and the standalone want
# different link-time treatment (the bundle re-exports three entry points and hides everything
# else), and these are a few thousand lines.
# ---------------------------------------------------------------------------
add_library(NampRackPlugin STATIC ${NAMPRACK_PLUGIN_SOURCES})
target_include_directories(NampRackPlugin PUBLIC ${NAMPRACK_ROOT}/src)
target_link_libraries(NampRackPlugin PUBLIC sdk NampRackGfx ${NAMPRACK_PLATFORM_GUI_LIBS})
target_compile_features(NampRackPlugin PUBLIC cxx_std_17)
target_compile_options(NampRackPlugin PRIVATE -ffast-math -Wall)
set_target_properties(NampRackPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
namprack_link_whole_dsp(NampRackPlugin)

if(NAMPRACK_BUILD_VST3_BUNDLE)
    smtg_add_vst3plugin(NampRackAmp PACKAGE_NAME "NAMp-Rack-Amp"
                        SOURCES_LIST ${NAMPRACK_PLUGIN_SOURCES})
    smtg_target_configure_version_file(NampRackAmp)
    target_compile_features(NampRackAmp PUBLIC cxx_std_17)
    target_compile_options(NampRackAmp PRIVATE -ffast-math -Wall)
    target_include_directories(NampRackAmp PRIVATE ${NAMPRACK_ROOT}/src)
    namprack_link_whole_dsp(NampRackAmp)
    target_link_libraries(NampRackAmp PRIVATE sdk NampRackGfx ${NAMPRACK_PLATFORM_GUI_LIBS})

    # The bundle carries its art in Contents/Resources rather than embedded, so it can be replaced
    # without a rebuild and so respath.cpp resolves it the same way here and in the offline render
    # rig. The standalone has no bundle to look in and carries the same files inside itself.
    smtg_target_add_plugin_resources(NampRackAmp
        RESOURCES ${NAMPRACK_FONT_PATHS}
        OUTPUT_SUBDIRECTORY fonts
    )
    smtg_target_add_plugin_resources(NampRackAmp
        RESOURCES ${NAMPRACK_IMG_PATHS}
        OUTPUT_SUBDIRECTORY img
    )
endif()

# ---------------------------------------------------------------------------
# NampRackResources — the art and fonts as byte arrays, for a binary with no resource directory
# beside it.
#
# ONLY THE STANDALONE LINKS THIS. The plug-in must not: it already carries the same files in
# Contents/Resources, where a user can replace them, and linking both would put two copies of every
# layer in one bundle. See src/gfx/resourcestore.h for the disk-first lookup order that makes the
# built-in set a fallback rather than a lock-in.
#
# The generated file depends on the resource files themselves, so replacing a layer rebuilds it and
# nothing else.
# ---------------------------------------------------------------------------
set(NAMPRACK_EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/namprackembeddedresources.cpp")
add_custom_command(
    OUTPUT "${NAMPRACK_EMBED_CPP}"
    COMMAND ${CMAKE_COMMAND}
            "-DNAMPRACK_EMBED_ROOT=${NAMPRACK_ROOT}/resources"
            "-DNAMPRACK_EMBED_FILES=${NAMPRACK_RESOURCE_FILES}"
            "-DNAMPRACK_EMBED_OUTPUT=${NAMPRACK_EMBED_CPP}"
            -P "${NAMPRACK_ROOT}/cmake/embedresources.cmake"
    DEPENDS ${NAMPRACK_FONT_PATHS} ${NAMPRACK_IMG_PATHS}
            "${NAMPRACK_ROOT}/cmake/embedresources.cmake"
    COMMENT "Embedding the resource directory into the standalone"
    VERBATIM
)
add_library(NampRackResources STATIC "${NAMPRACK_EMBED_CPP}")
# NampRackGfx for the include path and for installEmbeddedResources(), which the generated file
# calls.
target_link_libraries(NampRackResources PUBLIC NampRackGfx)
target_compile_features(NampRackResources PRIVATE cxx_std_17)
set_target_properties(NampRackResources PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Tools: the offline proofs and the panel render.
# ---------------------------------------------------------------------------
if(NAMPRACK_BUILD_TOOLS)
    set(NAMPRACK_MODULE_LOADER ${NAMPRACK_MODULE_LOADER_SRC})

    # The three tools that drive the BUILT BUNDLE through the SDK's own hosting classes, rather
    # than linking the DSP directly. That is the point of them: they answer questions about the
    # product a host loads, not about the sources.
    foreach(_tool rations_offline rations_ircheck rations_midicheck)
        add_executable(${_tool}
            ${NAMPRACK_ROOT}/tools/${_tool}.cpp
            ${NAMPRACK_MODULE_LOADER}
            # For MemoryStream, which the state round-trips need and which sdk_hosting does not
            # carry.
            ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
            ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
        )
        # The nlohmann header is here so the offline proof can read a capture's stated loudness
        # straight out of the .nam and assert that Normalized moves the output by exactly the
        # amount that number says it should. Header-only; nothing else of the DSP is linked in.
        target_include_directories(${_tool} PRIVATE ${NAMPRACK_ROOT}/src
            ${NAM_CORE_DIR}/Dependencies/nlohmann)
        target_link_libraries(${_tool} PRIVATE sdk_hosting)
        target_compile_features(${_tool} PRIVATE cxx_std_17)
        target_compile_options(${_tool} PRIVATE -Wall)
        if(NAMPRACK_BUILD_VST3_BUNDLE)
            add_dependencies(${_tool} NampRackAmp)
        endif()
    endforeach()

    # The crossfade proof: one bank, one dial, and the convergence assertion.
    add_executable(rations_fadecheck
        ${NAMPRACK_ROOT}/tools/rations_fadecheck.cpp
        ${NAMPRACK_ROOT}/src/capturesource.cpp
    )
    target_include_directories(rations_fadecheck PRIVATE ${NAMPRACK_ROOT}/src)
    namprack_link_whole_dsp(rations_fadecheck)
    target_compile_features(rations_fadecheck PRIVATE cxx_std_20)
    target_compile_options(rations_fadecheck PRIVATE -Wall)

    # The channel-switch proof and the race gate: the same engine sources, driven differently.
    foreach(_tool rations_switchcheck rations_racecheck)
        add_executable(${_tool}
            ${NAMPRACK_ROOT}/tools/${_tool}.cpp
            ${NAMPRACK_ROOT}/src/capturesource.cpp
            ${NAMPRACK_ROOT}/src/modelbank.cpp
            ${NAMPRACK_ROOT}/src/crossfadeengine.cpp
            ${NAMPRACK_ROOT}/src/channelrack.cpp
            # For utf8ToPath, which modelbank.cpp uses to keep an unrepresentable path from
            # throwing out of the worker thread.
            ${NAMPRACK_ROOT}/src/platform/respath.cpp
            ${NAMPRACK_ROOT}/src/gfx/resourcestore.cpp
        )
        target_include_directories(${_tool} PRIVATE ${NAMPRACK_ROOT}/src ${VST3_SDK_DIR})
        namprack_link_whole_dsp(${_tool})
        target_compile_features(${_tool} PRIVATE cxx_std_20)
        target_compile_options(${_tool} PRIVATE -Wall)
    endforeach()

    # ThreadSanitizer is a Linux gate and always was. MinGW ships no TSan runtime, so asking for
    # it in a cross build fails at link with an unresolved __tsan_init rather than saying what is
    # wrong. Refuse it here instead, and say so, rather than letting the option look as though it
    # did something.
    option(NAMPRACK_ENABLE_TSAN "Build the race gate under ThreadSanitizer" OFF)
    if(NAMPRACK_ENABLE_TSAN AND WIN32)
        message(WARNING
            "[NAMp-Rack] NAMPRACK_ENABLE_TSAN is ignored for the Windows cross build: MinGW has "
            "no ThreadSanitizer runtime. Run the race gate natively on Linux.")
    elseif(NAMPRACK_ENABLE_TSAN)
        target_compile_options(rations_racecheck PRIVATE -fsanitize=thread -g -O1)
        target_link_options(rations_racecheck PRIVATE -fsanitize=thread)
    endif()

    # The allocation gate. Deliberately links NO graphics at all — it is measuring allocations on
    # the audio path, and a Cairo dependency would put an allocator on the link line that has no
    # business being counted.
    add_executable(rations_rtcheck
        ${NAMPRACK_ROOT}/tools/rations_rtcheck.cpp
        ${NAMPRACK_ROOT}/src/irblend.cpp
        ${NAMPRACK_ROOT}/src/midilearn.cpp
        ${NAMPRACK_ROOT}/src/capturesource.cpp
        ${NAMPRACK_ROOT}/src/modelbank.cpp
        ${NAMPRACK_ROOT}/src/crossfadeengine.cpp
        ${NAMPRACK_ROOT}/src/channelrack.cpp
        ${NAMPRACK_ROOT}/src/platform/respath.cpp
        ${NAMPRACK_ROOT}/src/gfx/resourcestore.cpp
        ${NAM_CORE_DIR}/tools/test/allocation_tracking.cpp
    )
    # The SDK is on the include path for HEADERS only — nothing from it is linked and no VST3
    # object is created. midilearn.h names ParamID, because what a learned row performs is a
    # parameter, and a counted path that could not see that header would be counting a copy of the
    # matcher rather than the one that ships.
    target_include_directories(rations_rtcheck
        PRIVATE ${NAMPRACK_ROOT}/src ${NAM_CORE_DIR}/tools ${VST3_SDK_DIR})
    namprack_link_whole_dsp(rations_rtcheck)
    target_compile_features(rations_rtcheck PRIVATE cxx_std_20)
    # -O0 and -UNDEBUG, and both are load-bearing rather than cautious. The harness works by
    # replacing global operator new, so it can only count calls that are actually MADE — and C++14
    # explicitly permits the optimiser to elide a new/delete pair it can prove is redundant, which
    # at -O3 is exactly what happens to the harness's own self-test. The result is a checker that
    # reports zero allocations because it can no longer see any, which is the most dangerous
    # possible failure for a gate: it looks like a pass. -UNDEBUG keeps the asserts that would
    # otherwise be compiled out of the very code being measured.
    #
    # This costs nothing that matters. The tool is measuring WHETHER an allocation happens, not how
    # fast the audio path is, and the DSP it drives is in NampRackDsp and is still built at -O3.
    target_compile_options(rations_rtcheck PRIVATE -Wall -O0 -UNDEBUG)

    # The offline panel render: every page drawn without a host, which is the cheapest UI
    # regression test in the tree and the gate on every layout change.
    add_executable(panelrender
        ${NAMPRACK_ROOT}/tools/panelrender.cpp
        ${NAMPRACK_ROOT}/src/midilearn.cpp)
    target_link_libraries(panelrender PRIVATE NampRackGfx)
    target_include_directories(panelrender PRIVATE ${VST3_SDK_DIR})
    target_compile_options(panelrender PRIVATE -Wall)
endif()

# ---------------------------------------------------------------------------
# namp-rack — the product. The amp without a DAW: one X window, its own editor embedded in it, a
# Linux::IRunLoop the editor registers with, and an audio backend driving the processor.
#
# ONE FILE, NO INSTALLATION. The amp is linked in (NampRackPlugin) and its art and fonts are linked
# in (NampRackResources), so this runs on a machine with nothing installed and can never silently
# run an older installed build than the one just made. A bundle path can still be given on the
# command line to drive an external build deliberately.
#
# NOT BUILT WITHOUT JACK, and that is a hard dependency rather than a degradation: the standalone
# with no audio is the editor and nothing else, which is what panelrender already covers offline
# and covers better.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    # usediids.cpp stays HERE and nowhere else: it defines the SDK's interface IIDs and must appear
    # exactly once in a final binary. The plug-in-side classes come in through NampRackPlugin,
    # which does not carry them.
    add_executable(namp-rack
        ${NAMPRACK_ROOT}/standalone/main.cpp
        ${NAMPRACK_ROOT}/standalone/audiobackend.h
        ${NAMPRACK_ROOT}/standalone/runloop.h
        ${NAMPRACK_ROOT}/standalone/runloop.cpp
        ${NAMPRACK_ROOT}/standalone/midiroute.h
        ${NAMPRACK_ROOT}/standalone/midiroute.cpp
        ${NAMPRACK_ROOT}/standalone/jackclient.h
        ${NAMPRACK_ROOT}/standalone/jackclient.cpp
        ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${NAMPRACK_MODULE_LOADER_SRC}
        ${VST3_SDK_DIR}/public.sdk/samples/vst-hosting/editorhost/source/usediids.cpp
    )
    target_include_directories(namp-rack
        PRIVATE ${NAMPRACK_ROOT}/src ${NAMPRACK_ROOT}/standalone)
    target_link_libraries(namp-rack
        PRIVATE
            NampRackPlugin
            NampRackResources
            sdk_hosting
            PkgConfig::NAMPRACK_JACK
            ${NAMPRACK_PLATFORM_GUI_LIBS}
    )
    target_compile_features(namp-rack PRIVATE cxx_std_17)
    target_compile_options(namp-rack PRIVATE -Wall)

    # Same reasoning as the bundle's --exclude-libs, one layer up. This binary contains the NAM DSP
    # core and the SDK's plug-in-side classes, and it will dlopen OTHER people's plug-ins —
    # including other NAM-based ones. Anything it exported would interpose on theirs, and two
    # copies of a Steinberg symbol in one process is a crash nobody can debug. It exports nothing,
    # so bind every static archive's symbols locally and keep the dynamic table empty.
    #
    # --whole-archive still applies where it is asked for: it decides which OBJECTS are pulled in,
    # and the DSP core's self-registering architectures are registered from .init_array, which
    # needs no exported symbol.
    target_link_options(namp-rack PRIVATE -Wl,--exclude-libs,ALL)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "[NAMp-Rack] ${PROJECT_VERSION} — ${CMAKE_BUILD_TYPE}")
message(STATUS "[NAMp-Rack]   VST3 SDK        ${VST3_SDK_DIR}")
message(STATUS "[NAMp-Rack]   NAM core        ${NAM_CORE_DIR}")
message(STATUS "[NAMp-Rack]   AudioDSPTools   ${AUDIO_DSP_TOOLS_DIR}")
message(STATUS "[NAMp-Rack]   Eigen           ${EIGEN_DIR}")
if(NAMPRACK_BUILD_LV2_HOST)
    message(STATUS "[NAMp-Rack]   hosting         VST3 + LV2 (lv2 ${LV2HOST_lv2_VERSION}, "
                   "lilv ${LV2HOST_lilv-0_VERSION}, suil ${LV2HOST_suil-0_VERSION})")
else()
    message(STATUS "[NAMp-Rack]   hosting         VST3 only")
endif()
message(STATUS "[NAMp-Rack]   validator bundle ${NAMPRACK_BUILD_VST3_BUNDLE} (never packaged)")
message(STATUS "[NAMp-Rack]   tools           ${NAMPRACK_BUILD_TOOLS}")
message(STATUS "[NAMp-Rack]   bundled pedals  ${NAMPRACK_BUILD_PEDALS}")
message(STATUS "[NAMp-Rack] amp + standalone + tools; the host layer is not built yet.")
message(STATUS "")
