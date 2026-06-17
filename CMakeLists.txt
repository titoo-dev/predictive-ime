# Top-level superbuild — single entry point for a standalone (non-Nix) build.
#
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build -j
#   sudo cmake --install build
#
# Each component keeps its own CMakeLists (buildable in isolation too); this
# file just aggregates them under toggles and installs the runtime glue
# (systemd user unit). The Nix flake is a thin wrapper over this.
cmake_minimum_required(VERSION 3.16)
project(predictive-ime VERSION 0.1 LANGUAGES C CXX)

include(GNUInstallDirs)

option(BUILD_ENGINE      "Build the fcitx5 'predict' engine addon"          ON)
option(BUILD_DAEMON      "Build the n-gram prediction daemon (predictord)"  ON)
option(BUILD_UI          "Build the Qt Quick candidate panel (qmlpanel)"    ON)
option(BUILD_PREFERENCES "Build the Qt preferences app (ime-preferences)"   ON)
option(INSTALL_SYSTEMD_USER_UNIT "Install a systemd user service for the daemon" ON)

# qmlpanel and the caret-positioned candidate bar.
#   ON  : build against a fcitx5 patched with getInputMethodV2Raw
#         (see ui/waylandim-public.patch; this is what the Nix flake uses).
#   OFF : (target, after P0 spike) build against STOCK distro fcitx5 via a
#         vendored waylandim_public.h + getInputMethodV2 + rawPointer.
# Default is ON until the stock-fcitx5 path is implemented & CI-verified, at
# which point this flips to OFF so `cmake` works out-of-the-box everywhere.
# The portable CORE (engine+daemon+preferences) needs neither: set BUILD_UI=OFF
# to build it against stock fcitx5 today.
option(QMLPANEL_REQUIRE_PATCHED_FCITX5
       "qmlpanel uses the patched getInputMethodV2Raw API (else stock-compatible path)" ON)

if(BUILD_ENGINE)
  add_subdirectory(engine)
endif()
if(BUILD_DAEMON)
  add_subdirectory(daemon)
endif()
if(BUILD_UI)
  add_subdirectory(ui)
endif()
if(BUILD_PREFERENCES)
  add_subdirectory(ui/preferences)
endif()

if(INSTALL_SYSTEMD_USER_UNIT AND BUILD_DAEMON)
  set(PREDICTORD_BIN "${CMAKE_INSTALL_FULL_BINDIR}/predictord")
  set(MODEL_DIR      "${CMAKE_INSTALL_FULL_DATADIR}/ime-predictord")
  configure_file(packaging/ime-predictord.service.in
                 "${CMAKE_CURRENT_BINARY_DIR}/ime-predictord.service" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ime-predictord.service"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
endif()

message(STATUS "predictive-ime: engine=${BUILD_ENGINE} daemon=${BUILD_DAEMON} "
               "ui=${BUILD_UI} preferences=${BUILD_PREFERENCES} "
               "patched-fcitx5=${QMLPANEL_REQUIRE_PATCHED_FCITX5}")
