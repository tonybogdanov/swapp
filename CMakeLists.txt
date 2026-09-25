cmake_minimum_required(VERSION 3.16)
project(swapp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The bundled fonts and icons are compiled in, so the binary is the whole
# release: nothing has to sit next to it at runtime.
file(GLOB_RECURSE SWAPP_ASSET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
set(SWAPP_ASSETS_C "${CMAKE_CURRENT_BINARY_DIR}/assets_data.c")
add_custom_command(
    OUTPUT "${SWAPP_ASSETS_C}"
    COMMAND ${CMAKE_COMMAND}
            "-DASSETS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "-DOUTPUT=${SWAPP_ASSETS_C}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake"
    DEPENDS ${SWAPP_ASSET_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake"
    COMMENT "Embedding assets")

add_executable(swapp src/main.c src/monitors_common.c src/assets.c "${SWAPP_ASSETS_C}")
target_include_directories(swapp PRIVATE src)

# The short commit hash is the version (no semver): About shows it, and the
# update check compares it with the release's version.txt. Resolved at
# configure time; .git/logs/HEAD moves on every commit and checkout, so
# depending on it re-runs the configure when the hash changes.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE SWAPP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD")
    endif()
endif()
if(NOT SWAPP_COMMIT)
    set(SWAPP_COMMIT "unknown")
endif()
target_compile_definitions(swapp PRIVATE SWAPP_COMMIT="${SWAPP_COMMIT}")

if(WIN32)
    # The manifest opts into comctl32 v6, without which the monitor table
    # renders in the pre-XP classic style.
    target_sources(swapp PRIVATE src/tray_win.c src/monitors_win.c src/net_win.c src/install_win.c
                   src/update_win.c src/swapp.manifest)
    target_link_libraries(swapp PRIVATE shell32 shlwapi winhttp user32 advapi32 dxva2 ws2_32 comctl32 uxtheme)

    # GUI subsystem so no console window is created when the app launches,
    # while keeping a plain main() entry point (no WinMain needed).
    set_target_properties(swapp PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(swapp PRIVATE /ENTRY:mainCRTStartup)
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(LIBNOTIFY REQUIRED libnotify)

    # For the F16 global hotkey grab (XGrabKey) -- an X11-only mechanism,
    # separate from GTK's own X11/Wayland backend.
    pkg_check_modules(X11 REQUIRED x11)

    # monitors_linux.c uses GLib directly (g_unix_fd_add/g_timeout_add for
    # the monitor hotplug watch) independently of GTK.
    pkg_check_modules(GLIB2 REQUIRED glib-2.0)

    # The bundled Inter face is registered with fontconfig for this process
    # only, rather than installed for the whole machine.
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

    # HTTPS for the update check and download.
    pkg_check_modules(CURL REQUIRED libcurl)

    # Prefer the actively maintained Ayatana fork (what current Ubuntu ships);
    # fall back to the original libappindicator3 elsewhere.
    pkg_check_modules(AYATANA_APPINDICATOR ayatana-appindicator3-0.1)
    if(AYATANA_APPINDICATOR_FOUND)
        set(APPINDICATOR_INCLUDE_DIRS ${AYATANA_APPINDICATOR_INCLUDE_DIRS})
        set(APPINDICATOR_LIBRARY_DIRS ${AYATANA_APPINDICATOR_LIBRARY_DIRS})
        set(APPINDICATOR_LIBRARIES ${AYATANA_APPINDICATOR_LIBRARIES})
        set(APPINDICATOR_CFLAGS_OTHER ${AYATANA_APPINDICATOR_CFLAGS_OTHER})
        set(APPINDICATOR_HEADER_DEFINE SWAPP_USE_AYATANA_APPINDICATOR)
    else()
        pkg_check_modules(LEGACY_APPINDICATOR REQUIRED appindicator3-0.1)
        set(APPINDICATOR_INCLUDE_DIRS ${LEGACY_APPINDICATOR_INCLUDE_DIRS})
        set(APPINDICATOR_LIBRARY_DIRS ${LEGACY_APPINDICATOR_LIBRARY_DIRS})
        set(APPINDICATOR_LIBRARIES ${LEGACY_APPINDICATOR_LIBRARIES})
        set(APPINDICATOR_CFLAGS_OTHER ${LEGACY_APPINDICATOR_CFLAGS_OTHER})
    endif()

    target_sources(swapp PRIVATE src/tray_linux.c src/monitors_linux.c src/net_linux.c src/install_linux.c src/update_linux.c)
    target_include_directories(swapp PRIVATE ${GTK3_INCLUDE_DIRS} ${APPINDICATOR_INCLUDE_DIRS} ${LIBNOTIFY_INCLUDE_DIRS} ${X11_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})
    target_link_directories(swapp PRIVATE ${GTK3_LIBRARY_DIRS} ${APPINDICATOR_LIBRARY_DIRS} ${LIBNOTIFY_LIBRARY_DIRS} ${X11_LIBRARY_DIRS} ${GLIB2_LIBRARY_DIRS} ${FONTCONFIG_LIBRARY_DIRS} ${CURL_LIBRARY_DIRS})
    target_link_libraries(swapp PRIVATE ${GTK3_LIBRARIES} ${APPINDICATOR_LIBRARIES} ${LIBNOTIFY_LIBRARIES} ${X11_LIBRARIES} ${GLIB2_LIBRARIES} ${FONTCONFIG_LIBRARIES} ${CURL_LIBRARIES})
    target_compile_options(swapp PRIVATE ${GTK3_CFLAGS_OTHER} ${APPINDICATOR_CFLAGS_OTHER} ${LIBNOTIFY_CFLAGS_OTHER} ${X11_CFLAGS_OTHER} ${GLIB2_CFLAGS_OTHER} ${FONTCONFIG_CFLAGS_OTHER} ${CURL_CFLAGS_OTHER})
    if(APPINDICATOR_HEADER_DEFINE)
        target_compile_definitions(swapp PRIVATE ${APPINDICATOR_HEADER_DEFINE})
    endif()
endif()
