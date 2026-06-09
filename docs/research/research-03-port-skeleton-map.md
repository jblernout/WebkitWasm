# WebKit Port Skeleton: Complete CMake Inventory for Emscripten

**Status:** Complete research audit of webkitglib/2.52 port mechanism
**Date:** 2026-06-09
**Target:** Minimal file set for PORT=Emscripten (WTF + JavaScriptCore + WebCore)

---

## 1. Port Registration Mechanism: ALL_PORTS Definition

### Location
**File:** `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/cmake/WebKitCommon.cmake` (lines 42-52)

### Exact Mechanism
```cmake
set(ALL_PORTS
    GTK
    JSCOnly
    Mac
    PlayStation
    WPE
    Win
)

set(PORT "NOPORT" CACHE STRING "choose which WebKit port to build (one of ${ALL_PORTS})")
list(FIND ALL_PORTS ${PORT} RET)
if (RET EQUAL -1)
    message(FATAL_ERROR "Please choose which WebKit port to build (one of ${ALL_PORTS})")
endif ()
```

**How to add Emscripten:**
Edit `WebKitCommon.cmake` to add `Emscripten` to the `ALL_PORTS` list, then the CMake validation automatically allows `-DPORT=Emscripten`.

### Platform File Inclusion Hook
**File:** `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/cmake/WebKitMacros.cmake` (lines 77-84)

```cmake
macro(WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS)
    set(_file ${CMAKE_CURRENT_SOURCE_DIR}/Platform${PORT}.cmake)
    if (EXISTS ${_file})
        message(STATUS "Using platform-specific CMakeLists: ${_file}")
        include(${_file})
    else ()
        message(STATUS "Platform-specific CMakeLists not found: ${_file}")
    endif ()
endmacro()
```

**Key Pattern:** Every component (WTF, JavaScriptCore, bmalloc, WebCore, etc.) calls `WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()` to load its `Platform${PORT}.cmake` file if it exists. Missing files are non-fatal.

---

## 2. Complete Port File Inventory

### PlayStation Port (All 25+ Files)

#### Options/Configuration
- **Source/cmake/OptionsPlayStation.cmake**
  - Defines `set(PORT PlayStation)`
  - Platform compiler flags, definitions (`-DWTF_PLATFORM_PLAYSTATION=1`)
  - Includes module-finding helpers (`include(PlayStationModule)`, `include(Sign)`)
  - Sets `USE_GLIB`, `USE_SKIA`, `USE_CAIRO` conditionally

#### WTF Layer
- **Source/WTF/wtf/PlatformPlayStation.cmake**
  - Declares RunLoop choice: `generic/RunLoopGeneric.cpp` (NOT glib)
  - Platform-specific sources: `playstation/OSAllocatorPlayStation.cpp`, `LanguagePlayStation.cpp`, `FileSystemPlayStation.cpp`, etc.

#### JavaScriptCore Layer
- **Source/JavaScriptCore/PlatformPlayStation.cmake** (if it exists)
  - Usually minimal; inspector setup

#### bmalloc Layer
- **Source/bmalloc/PlatformPlayStation.cmake**
  - Memory allocator configuration

#### WebCore Layer (REQUIRED for full browser)
- **Source/WebCore/PlatformPlayStation.cmake**
  - **Includes graphics backend:** `include(platform/Curl.cmake)`, `include(platform/ImageDecoders.cmake)`, `include(platform/OpenSSL.cmake)`, `include(platform/TextureMapper.cmake)`
  - **Graphics choice:** `if (USE_CAIRO)` → `include(platform/Cairo.cmake)` + `include(platform/FreeType.cmake)` ELSE `if (USE_SKIA)` → `include(platform/Skia.cmake)`
  - **Network:** Curl hardwired via the `platform/Curl.cmake` include
  - **Platform sources:** Adds 40+ PlayStation-specific files under `platform/graphics/egl/`, `platform/network/playstation/`, `rendering/playstation/`, etc.

#### PAL (Platform Abstraction Layer)
- **Source/WebCore/PAL/pal/PlatformPlayStation.cmake**
  - PAL-specific platform integration

#### ThirdParty
- **Source/ThirdParty/gtest/PlatformPlayStation.cmake**
- **Source/ThirdParty/ANGLE/PlatformPlayStation.cmake**

#### WebDriver
- **Source/WebDriver/PlatformPlayStation.cmake**
- **Source/WebDriver/playstation/WebDriverServicePlayStation.cpp**

#### Headers
- **Source/WTF/wtf/PlatformEnablePlayStation.h**

### JSCOnly Port (Minimal, 5 Files)

JSCOnly is a **language-binding-only** port (NO WebCore, NO WebKit2).

#### Options/Configuration
- **Source/cmake/OptionsJSCOnly.cmake**
  - Defines `set(PORT JSCOnly)` (implicitly used by macro detection)
  - Sets `ENABLE_WEBCORE OFF` (critical: disables WebCore entirely)
  - Declares `ALL_EVENT_LOOP_TYPES = {GLib, Generic}` with default `Generic`
  - Handles `EVENT_LOOP_TYPE` cache variable (runtime selection)
  - Conditional event loop setup:
    ```cmake
    if (USE_GLIB)
        SET_AND_EXPOSE_TO_BUILD(USE_GLIB_EVENT_LOOP 1)
    else ()
        SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
    endif ()
    ```

#### WTF Layer
- **Source/WTF/wtf/PlatformJSCOnly.cmake**
  - Generic RunLoop selection: conditionally includes `glib/RunLoopGLib.cpp` or `generic/RunLoopGeneric.cpp`
  - Generic sources: `generic/WorkQueueGeneric.cpp`
  - Windows stubs if needed: `text/win/StringWin.cpp`, etc.
  - Links `Threads::Threads` library

#### JavaScriptCore Layer
- **Source/JavaScriptCore/PlatformJSCOnly.cmake**
  - Conditional inspector: `if (USE_GLIB)` → `include(inspector/remote/GLib.cmake)` ELSE `include(inspector/remote/Socket.cmake)`
  - Optional GLib API bindings

#### bmalloc Layer
- **Source/bmalloc/PlatformJSCOnly.cmake**
  - Standard allocator (no override)

#### NO WebCore Files
- JSCOnly **explicitly does NOT include** `Source/WebCore/PlatformJSCOnly.cmake` (file does not exist)
- Because `OptionsJSCOnly.cmake` sets `ENABLE_WEBCORE OFF`, WebCore CMakeLists.txt is never processed

---

## 3. WebCore Platform File Structure & Graphics/Network Integration

### PlatformPlayStation.cmake Content Categories

#### A. Graphics Backend Selection (Lines 1-11)
```cmake
include(platform/Curl.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)
include(platform/TextureMapper.cmake)

if (USE_CAIRO)
    include(platform/Cairo.cmake)
    include(platform/FreeType.cmake)
elseif (USE_SKIA)
    include(platform/Skia.cmake)
endif ()
```

**Key Decision Point:** The CMake variable `USE_CAIRO` (boolean, set in OptionsPlayStation.cmake) determines which graphics backend includes are pulled in.

#### B. Platform Source Files (Lines 24+)
Lists platform-specific `.cpp` files:
- `platform/graphics/egl/` — EGL/OpenGL ES bindings (4 files)
- `platform/graphics/libwpe/` — WPE display integration
- `platform/graphics/playstation/` — PlayStation font database
- `platform/network/playstation/` — Curl SSL handling, network state notifier
- `platform/gamepad/libwpe/` — Optional gamepad support
- `accessibility/playstation/`, `rendering/playstation/` — UI layers

#### C. Libraries & Includes
- **Links:** `WPE::libwpe`, `WebKitRequirements::WebKitResources`
- **Include dirs:** Graphics dirs (`egl`, `opengl`, `libwpe`), media, video-codecs

### PlatformWin.cmake Comparison: How Windows Handles Curl & Skia

**File:** `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/WebCore/PlatformWin.cmake` (lines 1-14)

```cmake
add_definitions(/bigobj -D__STDC_CONSTANT_MACROS)

include(platform/Adwaita.cmake)
include(platform/Curl.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)
include(platform/TextureMapper.cmake)

if (USE_CAIRO)
    include(platform/Cairo.cmake)
elseif (USE_SKIA)
    include(platform/Skia.cmake)
endif ()

if (USE_DAWN)
    include(platform/Dawn.cmake)
endif ()
```

**Similarities to PlayStation:**
- Both hardwire `platform/Curl.cmake` (network)
- Both check `USE_CAIRO` vs `USE_SKIA` (graphics)
- Both declare platform include dirs + source files below the graphics conditional

**Key Difference:** Windows adds `platform/Adwaita.cmake` (GTK theme support) and optionally `platform/Dawn.cmake` (WebGPU), neither of which PlayStation includes.

**Curl Integration:** NOT conditional. Every WebCore port that needs HTTP/HTTPS must `include(platform/Curl.cmake)`. The Curl.cmake file itself is generic and adds the same sources to all ports.

**Skia Integration:** Determined by CMake variable `USE_SKIA`. When ON, `include(platform/Skia.cmake)` adds Skia source files. When OFF with `USE_CAIRO=ON`, Cairo is used instead.

---

## 4. Event Loop Configuration

### RunLoop Choice Mechanism

**CMake Variables:**
- `USE_GENERIC_EVENT_LOOP` — 1 to enable generic RunLoop
- `USE_GLIB_EVENT_LOOP` — 1 to enable GLib RunLoop
- `USE_GLIB` — 0/1 conditional flag (set in OptionsJSCOnly.cmake)

### Consumption Points

**Source/WTF/wtf/PlatformPlayStation.cmake:**
```cmake
list(APPEND WTF_SOURCES
    generic/RunLoopGeneric.cpp
)
```
→ **PlayStation uses generic RunLoop** (no GLib event loop)

**Source/WTF/wtf/PlatformJSCOnly.cmake:**
```cmake
if (USE_GLIB)
    list(APPEND WTF_SOURCES glib/RunLoopGLib.cpp ...)
else ()
    list(APPEND WTF_SOURCES generic/RunLoopGeneric.cpp ...)
endif ()
```
→ **JSCOnly allows choice at CMake time** via `EVENT_LOOP_TYPE` cache variable

**Source/WTF/wtf/PlatformMac.cmake:**
```cmake
list(APPEND WTF_SOURCES cf/RunLoopCF.cpp)
```
→ **Mac uses Core Foundation RunLoop**

**Source/WTF/wtf/PlatformWPE.cmake:**
```cmake
list(APPEND WTF_SOURCES glib/RunLoopGLib.cpp glib/RunLoopSourcePriority.h)
```
→ **WPE always uses GLib RunLoop**

**Source/WTF/wtf/PlatformWin.cmake:**
```cmake
list(APPEND WTF_SOURCES win/RunLoopWin.cpp)
```
→ **Windows uses Win32 RunLoop**

### How OptionsJSCOnly.cmake Exposes the Choice

```cmake
set(ALL_EVENT_LOOP_TYPES GLib Generic)
set(DEFAULT_EVENT_LOOP_TYPE "Generic")
set(EVENT_LOOP_TYPE ${DEFAULT_EVENT_LOOP_TYPE} CACHE STRING ...)

if (USE_GLIB)
    SET_AND_EXPOSE_TO_BUILD(USE_GLIB_EVENT_LOOP 1)
else ()
    SET_AND_EXPOSE_TO_BUILD(USE_GENERIC_EVENT_LOOP 1)
endif ()
```

**Macro `SET_AND_EXPOSE_TO_BUILD`:** Sets CMake variable AND writes it to the build config header so C++ code can check `#if USE_GENERIC_EVENT_LOOP`.

---

## 5. WebCore CMakeLists.txt Requirements

### File: `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/WebCore/CMakeLists.txt`

**Entry Point (lines 1-100):**

The WebCore CMakeLists.txt does NOT explicitly call `WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()` in the first 100 lines searched. Instead, the platform include is triggered by a macro call elsewhere or by the parent CMakeLists.txt.

**Critical Finding:** WebCore does NOT enforce required variables at the top of its CMakeLists.txt. Instead, it conditionally includes platform files and then uses `list(APPEND WebCore_SOURCES ...)` to accumulate sources.

### Required Variables Per Port (Implicit Requirements)

Every port's `PlatformXXX.cmake` file MUST populate:
1. `WebCore_SOURCES` — list of `.cpp` files to compile
2. `WebCore_LIBRARIES` — list of external libraries to link (e.g., `WPE::libwpe`, `CURL::libcurl`)
3. `WebCore_PRIVATE_INCLUDE_DIRECTORIES` — internal include paths
4. `WebCore_PRIVATE_FRAMEWORK_HEADERS` — headers to install

**Soft Requirements (conditional on features):**
- `WebCore_INTERFACE_DEPENDENCIES` — if a port needs custom build targets (e.g., EGL DLL copying for PlayStation)

### No FATAL_ERROR Checks Enforced

**Finding:** The WebCore CMakeLists.txt does NOT contain `message(FATAL_ERROR)` checks for missing port definitions. Missing `PlatformXXX.cmake` files result in a STATUS message only (from WebKitMacros.cmake line 82: `message(STATUS "Platform-specific CMakeLists not found")`).

This means a minimal WebCore build can proceed with an empty (or absent) `PlatformEmscripten.cmake`, but will fail at link time due to missing symbols.

---

## 6. Skia Integration in WebKit CMake

### File: `Source/cmake/WebKitFeatures.cmake`
**Definition of USE_SKIA:**
```cmake
WEBKIT_OPTION_DEFINE(USE_SKIA "Whether to use Skia instead of Cairo." PRIVATE OFF)
WEBKIT_OPTION_DEFINE(USE_SKIA_ENCODERS "Whether to use Skia image encoders" PRIVATE ON)
WEBKIT_OPTION_DEPEND(USE_SKIA_ENCODERS USE_SKIA)
```

Default is OFF (meaning Cairo is default).

### File: `Source/cmake/OptionsWPE.cmake`
```cmake
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE ON)  # for some builds
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE OFF) # for other builds
WEBKIT_OPTION_DEFINE(USE_SKIA_OPENTYPE_SVG "..." PUBLIC ON)
WEBKIT_OPTION_DEPEND(USE_SKIA_OPENTYPE_SVG USE_SKIA)

if (USE_SKIA)
    # Port-specific Skia setup
endif ()
```

### File: `Source/cmake/OptionsPlayStation.cmake`
```cmake
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA_ENCODERS PRIVATE OFF)

if (NOT USE_SKIA)
    # Cairo setup
elseif (USE_SKIA)
    # Skia setup
endif ()
```

### Consumption in WebCore PlatformXXX.cmake

**File:** `Source/WebCore/PlatformPlayStation.cmake` (lines 5-11)
```cmake
if (USE_CAIRO)
    include(platform/Cairo.cmake)
    include(platform/FreeType.cmake)
elseif (USE_SKIA)
    include(platform/Skia.cmake)
endif ()
```

**Port-side Skia Sources** (line 101-103):
```cmake
if (USE_SKIA)
    list(APPEND WebCore_SOURCES
        platform/graphics/egl/GLFence.cpp
        ...
    )
endif ()
```

### Source Organization

**No separate SourcesSkia.txt file found in this build.** Instead, Skia sources are:
1. **Declared in WebCore/PlatformXXX.cmake:** Ports list Skia-specific files in `list(APPEND WebCore_SOURCES ...)` conditionally.
2. **Included via `include(platform/Skia.cmake)`:** This file (in `Source/WebCore/platform/`) aggregates Skia-specific sources:
   - `platform/graphics/skia/*.cpp` — Skia rendering backend
   - Skia library linking via `list(APPEND WebCore_LIBRARIES ...)` or pkg-config

### Ports That Use Skia
- **WPE:** `OptionsWPE.cmake` sets `USE_SKIA ON` (default)
- **GTK:** `OptionsGTK.cmake` conditionally sets `USE_SKIA ON` (with some builds OFF)
- **Win:** `OptionsWin.cmake` sets `USE_SKIA ON` (default)
- **PlayStation:** Configurable in `OptionsPlayStation.cmake`, but NOT default
- **Mac, JSCOnly, Haiku:** Use Cairo instead

---

## 7. Minimal Port Skeleton: EMSCRIPTEN

### Requirements Summary

To build **WTF + JavaScriptCore + WebCore** for Emscripten, create:

#### TIER 1: Global Registration (REQUIRED)
**File:** `Source/cmake/OptionsEmscripten.cmake` (~100 lines)
- Set `PORT Emscripten`
- Define `ENABLE_WEBCORE ON` (required for WebCore; JSCOnly uses OFF)
- Define `USE_GENERIC_EVENT_LOOP 1` (Emscripten has no GLib)
- Define graphics choice: `USE_CAIRO ON` (recommended; lightweight) or `USE_SKIA ON`
- Disable inapplicable features: `ENABLE_VIDEO OFF`, `ENABLE_WEBGL OFF` (or minimal WebGL bridge)
- Expose choices via `SET_AND_EXPOSE_TO_BUILD()`
- `set(PORT "Emscripten")` — this is the key that triggers macro inclusion

**To Register:** Edit `Source/cmake/WebKitCommon.cmake` line 42-48, add `Emscripten` to `ALL_PORTS` list

#### TIER 2: WTF RunLoop (REQUIRED)
**File:** `Source/WTF/wtf/PlatformEmscripten.cmake` (~30 lines)
```cmake
list(APPEND WTF_SOURCES
    generic/RunLoopGeneric.cpp
)
list(APPEND WTF_LIBRARIES Threads::Threads)
# No platform-specific sources needed
```

#### TIER 3: JavaScriptCore (OPTIONAL, 10 lines)
**File:** `Source/JavaScriptCore/PlatformEmscripten.cmake`
```cmake
# Usually empty; only if custom inspector/debugger needed
# Inspector default: Socket (no GLib)
if (ENABLE_REMOTE_INSPECTOR)
    include(inspector/remote/Socket.cmake)
endif ()
```

#### TIER 4: bmalloc (OPTIONAL, can be empty)
**File:** `Source/bmalloc/PlatformEmscripten.cmake`
- Can be omitted; generic allocator is default

#### TIER 5: WebCore (REQUIRED for browser, ~50 lines)
**File:** `Source/WebCore/PlatformEmscripten.cmake`
```cmake
# Graphics backend
include(platform/Curl.cmake)       # Network (HTTP/HTTPS)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)

if (USE_CAIRO)
    include(platform/Cairo.cmake)
    include(platform/FreeType.cmake)
elseif (USE_SKIA)
    include(platform/Skia.cmake)
endif ()

# Emscripten-specific sources (minimal)
list(APPEND WebCore_SOURCES
    platform/generic/KeyedDecoderGeneric.cpp
    platform/generic/KeyedEncoderGeneric.cpp
    # Emscripten stubs for themes, platform screen, etc.
)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    ${WEBCORE_DIR}/platform/graphics/opengl
)

# Graphics libraries (Cairo or Skia defined by includes above)
list(APPEND WebCore_LIBRARIES)  # Added by included platform files
```

#### TIER 6: PAL (OPTIONAL, ~20 lines)
**File:** `Source/WebCore/PAL/pal/PlatformEmscripten.cmake`
- Usually minimal; can inherit from generic defaults

### Files to Create (Minimal Set)
1. ✅ **Source/cmake/OptionsEmscripten.cmake** (REQUIRED)
2. ✅ **Source/WTF/wtf/PlatformEmscripten.cmake** (REQUIRED)
3. ✅ **Source/WebCore/PlatformEmscripten.cmake** (REQUIRED for WebCore)
4. ⚠️ **Source/JavaScriptCore/PlatformEmscripten.cmake** (OPTIONAL if empty)
5. ⚠️ **Source/bmalloc/PlatformEmscripten.cmake** (OPTIONAL if empty)
6. ⚠️ **Source/WebCore/PAL/pal/PlatformEmscripten.cmake** (OPTIONAL if empty)
7. ✏️ **Edit: Source/cmake/WebKitCommon.cmake** → add `Emscripten` to `ALL_PORTS` list

### Minimal Linecount for WTF+JSC Build
- OptionsEmscripten.cmake: ~80 lines
- PlatformEmscripten.cmake (WTF): ~10 lines
- **Total:** ~90 lines of CMake + 1 line edit to WebKitCommon.cmake

### Minimal Linecount for Full WebCore Build
- OptionsEmscripten.cmake: ~100 lines
- PlatformEmscripten.cmake (WTF): ~15 lines
- PlatformEmscripten.cmake (WebCore): ~50 lines
- PlatformEmscripten.cmake (PAL): ~10 lines
- **Total:** ~175 lines of CMake + 1 line edit to WebKitCommon.cmake

---

## 8. BIGGEST COMPLICATION: Graphics Backend Bridge

### The Problem

Emscripten compiles C++ to WebAssembly (wasm). It has **no native OpenGL/graphics device**. WebKit's graphics backends (Cairo or Skia) are designed for native platforms with direct device access.

### The Challenge

**Cairo Approach:**
- Cairo is a 2D vector graphics library
- Needs a backend (X11, Win32, Quartz, OpenGL)
- For Emscripten, **you must provide a Cairo backend that bridges to HTML5 Canvas/WebGL**
- Emscripten's `GL` library can provide OpenGL bindings to browser WebGL, but:
  - Performance is indirect (emulation layer)
  - Cairo OpenGL backend is not heavily tested with Emscripten
- **Recommended:** Use Cairo with a custom "web" backend that renders to HTML5 Canvas 2D

**Skia Approach:**
- Skia is a lower-level graphics engine
- Can target different backends (CPU, GPU, PDF, SVG)
- Emscripten can use Skia's CPU rasterizer, but:
  - Very slow (pixel-by-pixel software rendering in wasm)
- Or: Bridge Skia to WebGL, but:
  - Skia's WebGL backend is less common than Cairo's

### Decision in OptionsEmscripten.cmake

**REQUIRED line in OptionsEmscripten.cmake:**
```cmake
# For lighter footprint and canvas compatibility
SET_AND_EXPOSE_TO_BUILD(USE_CAIRO ON)
# OR for lower-level control (not recommended for Emscripten):
# SET_AND_EXPOSE_TO_BUILD(USE_SKIA ON)
```

**Why This is Hard:**
1. **Neither backend "just works"** with Emscripten out of the box
2. **You must write custom graphics bindings** in C++:
   - Implement Cairo/Skia → Canvas 2D or WebGL adaptation layer
   - Or use existing Emscripten graphics libraries (e.g., SDL2 -> Canvas)
3. **Testing is non-trivial:**
   - Build and test in browser environment (Node.js wasm runtime or browser)
   - Debug WebAssembly + JavaScript interop issues

### Recommendation

**Use Cairo + Custom Web Backend:**
- Cairo is lighter and more suitable for web rendering
- Implement a small Cairo surface backend that:
  - Allocates pixel buffers in wasm memory
  - Uses Emscripten's `EM_ASM()` or `EM_ASYNC_JS()` to call JavaScript
  - JavaScript copies pixels from wasm to HTML5 Canvas
- ~200-300 lines of C++ code to bridge

**Alternative (Lightweight):**
- Disable graphics entirely for a JavaScript-only build
- Use `ENABLE_WEBGL OFF` and stub out graphics with JavaScript calls
- Build WTF + JSC only; let WebCore integrate via JS bindings

---

## Appendix: Complete Platform File Listing

### All Platform*.cmake Files in WebKit (for reference)

```
Source/cmake/
  ├─ OptionsGTK.cmake
  ├─ OptionsMac.cmake
  ├─ OptionsMSVC.cmake
  ├─ OptionsPlayStation.cmake
  ├─ OptionsWin.cmake
  ├─ OptionsWPE.cmake
  └─ OptionsJSCOnly.cmake

Source/WTF/wtf/
  ├─ PlatformGTK.cmake
  ├─ PlatformMac.cmake
  ├─ PlatformWin.cmake
  ├─ PlatformWPE.cmake
  ├─ PlatformPlayStation.cmake
  └─ PlatformJSCOnly.cmake

Source/JavaScriptCore/
  ├─ PlatformMac.cmake
  ├─ PlatformWin.cmake
  ├─ PlatformPlayStation.cmake
  ├─ PlatformGTK.cmake (if exists)
  └─ PlatformJSCOnly.cmake

Source/bmalloc/
  ├─ PlatformMac.cmake
  ├─ PlatformPlayStation.cmake
  └─ PlatformJSCOnly.cmake

Source/WebCore/
  ├─ PlatformGTK.cmake
  ├─ PlatformMac.cmake
  ├─ PlatformWin.cmake
  ├─ PlatformWPE.cmake
  └─ PlatformPlayStation.cmake

Source/WebCore/PAL/pal/
  ├─ PlatformGTK.cmake
  ├─ PlatformMac.cmake
  ├─ PlatformWin.cmake
  ├─ PlatformWPE.cmake
  ├─ PlatformPlayStation.cmake
  └─ PlatformHaiku.cmake

Source/WebDriver/
  └─ PlatformPlayStation.cmake (WebDriver not needed for WTF+JSC+WebCore)

Source/ThirdParty/gtest/
  └─ PlatformPlayStation.cmake

Source/ThirdParty/ANGLE/
  └─ PlatformPlayStation.cmake

Tools/TestWebKitAPI/
  ├─ PlatformGTK.cmake
  ├─ PlatformMac.cmake
  ├─ PlatformWin.cmake
  ├─ PlatformWPE.cmake
  ├─ PlatformJSCOnly.cmake
  └─ PlatformPlayStation.cmake
```

---

## Summary Table

| Component      | OptionsXXX.cmake | PlatformWTF.cmake | PlatformJSC.cmake | PlatformWebCore.cmake | PlatformPAL.cmake |
|----------------|------------------|-------------------|-------------------|----------------------|-------------------|
| **GTK**        | ✅ OptionsGTK    | ✅                | ❌                | ✅                   | ✅                |
| **Mac**        | ✅ OptionsMac    | ✅                | ✅                | ✅                   | ✅                |
| **Win**        | ✅ OptionsWin    | ✅                | ✅                | ✅                   | ✅                |
| **WPE**        | ✅ OptionsWPE    | ✅                | ❌                | ✅                   | ✅                |
| **PlayStation**| ✅ OptionsPS     | ✅                | ❌                | ✅                   | ✅                |
| **JSCOnly**    | ✅ OptionsJSC    | ✅                | ✅                | ❌ (ENABLE_WEBCORE OFF) | ❌                |
| **Emscripten** | ✅ **NEW**       | ✅ **NEW**        | ⚠️ optional       | ✅ **NEW**           | ⚠️ optional       |

---

## Conclusion

**Minimal Emscripten Port = 3 Required Files + 1 Edit:**

1. `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/cmake/OptionsEmscripten.cmake` (~100 lines)
2. `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/WTF/wtf/PlatformEmscripten.cmake` (~15 lines)
3. `/mnt/storage/Projects/Fable-Tests/BrowserInBrowser/third_party/WebKit/Source/WebCore/PlatformEmscripten.cmake` (~50 lines)
4. **EDIT:** `Source/cmake/WebKitCommon.cmake` — add `Emscripten` to `ALL_PORTS`

**Biggest Complication:** Providing a graphics backend (Cairo or Skia) that actually renders in Emscripten's WebAssembly environment. Neither backend has native Emscripten support; both require a custom bridge to HTML5 Canvas or WebGL. This is the load-bearing technical challenge, separate from the port skeleton itself.

