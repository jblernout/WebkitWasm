# BrowserInBrowser embedder target (Phase 2 first-paint gate).
#
# Included from Source/WebCore/PlatformEmscripten.cmake via
# cmake_language(DEFER CALL include ...) — i.e. this file executes in
# WebCore's directory scope AFTER the whole WebCore/CMakeLists.txt has been
# processed: the WebCore target exists and the include-dir lists are final.
# Include-dir consumption copies the WebCoreTestSupport pattern (the bottom
# of WebCore/CMakeLists.txt): WebCore's PRIVATE include dirs do not propagate
# through target_link_libraries, so internal-header consumers replicate them.

get_filename_component(BIB_EMBEDDER_DIR "${EMSCRIPTEN_EMBEDDER_CMAKE}" DIRECTORY)

add_executable(BibEmbedder
    ${BIB_EMBEDDER_DIR}/EmbedderStrategies.cpp
    ${BIB_EMBEDDER_DIR}/main.cpp
)
set_target_properties(BibEmbedder PROPERTIES OUTPUT_NAME embedder)

target_include_directories(BibEmbedder PRIVATE
    ${WebCore_INCLUDE_DIRECTORIES}
    ${WebCore_PRIVATE_INCLUDE_DIRECTORIES}
)
target_include_directories(BibEmbedder SYSTEM PRIVATE
    ${WebCore_SYSTEM_INCLUDE_DIRECTORIES}
)

# WebCore is static: its PRIVATE link deps (JSC/WTF/PAL/curl tier/ICU) reach
# the final link as $<LINK_ONLY:> interface entries. Skia is linked directly
# for its INTERFACE include dirs (<skia/...> resolves via symlinked headers).
target_link_libraries(BibEmbedder PRIVATE WebCore Skia::Skia)

# Same engine-sized stack/heap as the Phase 1 jsc shell
# (Source/JavaScriptCore/shell/CMakeLists.txt). NO -pthread: the entire
# library stack (WTF/JSC/WebCore/sysroot) is compiled single-threaded.
target_link_options(BibEmbedder PRIVATE
    "SHELL:-sSTACK_SIZE=8MB"
    "SHELL:-sINITIAL_MEMORY=256MB"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sMAXIMUM_MEMORY=4GB"
    "SHELL:-sEXIT_RUNTIME=1"
    # FS: the node runner (tools/run-embedder.cjs) reads /out.ppm out of
    # MEMFS in Module.onExit, and both hosts pre-create fontconfig's cache
    # dir in preRun. HEAPU8: the browser host page (web/browser.html) wraps
    # bib_render()'s pixel pointer in a Uint8ClampedArray for putImageData.
    # ccall: browser.html marshals bib_key()'s string args. stringToUTF8/
    # lengthBytesUTF8: main.cpp reads Module.bibHTML inside EM_ASM — export
    # forces them into the runtime so the EM_ASM block can call them.
    # ENV: lets the host page set engine env vars in preRun (e.g.
    # DEBUG_CURL=1 turns on libcurl verbose tracing -> printErr).
    "SHELL:-sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,ccall,stringToUTF8,lengthBytesUTF8,ENV"
    # Surface the COMPLETE undefined-symbol list per link attempt instead of
    # wasm-ld's default 20-error cutoff — each stub iteration costs minutes.
    "SHELL:-Wl,--error-limit=0"
    # Keep the wasm name section: abort/crash stacks in the browser show
    # real function names instead of wasm-function[N]. Costs binary size
    # only (no codegen change) — load-bearing for site-abort diagnosis.
    "SHELL:--profiling-funcs"
)

# ICU data archive at the same absolute path ICU compiled in as its default
# data dir (jsc-shell trick — works in node and browser with no env setup).
if (JSC_EMBED_ICU_DATA_FILE)
    target_link_options(BibEmbedder PRIVATE
        "SHELL:--embed-file ${JSC_EMBED_ICU_DATA_FILE}@${JSC_EMBED_ICU_DATA_FILE}")
endif ()

# Fonts are load-bearing: fontconfig config tree at /etc/fonts (compiled-in
# --sysconfdir) and at least one real TTF at /usr/share/fonts (compiled-in
# --with-default-fonts). Without both, text paints nothing.
if (BIB_FONTCONFIG_ETC_DIR)
    target_link_options(BibEmbedder PRIVATE
        "SHELL:--embed-file ${BIB_FONTCONFIG_ETC_DIR}@/etc/fonts")
endif ()
if (BIB_FONTS_DIR)
    target_link_options(BibEmbedder PRIVATE
        "SHELL:--embed-file ${BIB_FONTS_DIR}@/usr/share/fonts")
endif ()

# CA bundle for in-engine TLS verification (Phase 4) — the path is
# compiled into CurlSSLHandleEmscripten.cpp.
if (BIB_CA_BUNDLE)
    target_link_options(BibEmbedder PRIVATE
        "SHELL:--embed-file ${BIB_CA_BUNDLE}@/etc/ssl/cacert.pem")
endif ()
