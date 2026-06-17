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

# NOTE on qmlpanel (BUILD_UI): the caret-positioned candidate bar needs the raw
# zwp_input_method_v2 object, which a STOCK fcitx5 does NOT expose to external
# addons. Verified by probing fcitx5-devel on 5.1.7/5.1.13/5.1.17/5.1.19: they
# ship neither the waylandim public header, the ZwpInputMethodV2 wrapper, nor
# rawPointer. So qmlpanel REQUIRES a fcitx5 carrying the waylandim public API —
# patched (ui/waylandim-public.patch, as the flake does) or, once merged,
# upstream. The portable CORE (engine + daemon + preferences) needs none of this
# and builds against any stock fcitx5: pass -DBUILD_UI=OFF.
# See docs/specs/2026-06-17-cross-distro-oss-design.md.

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
  # systemd user units always live in <prefix>/lib/systemd/user — NOT in the
  # arch libdir (lib64): systemd's search path never includes lib64.
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ime-predictord.service"
          DESTINATION "lib/systemd/user")
endif()

message(STATUS "predictive-ime: engine=${BUILD_ENGINE} daemon=${BUILD_DAEMON} "
               "ui=${BUILD_UI} preferences=${BUILD_PREFERENCES} "
               "(ui requires a patched/upstream fcitx5 — see comment above)")
