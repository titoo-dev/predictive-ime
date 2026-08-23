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
option(BUILD_PREFERENCES "Build the Qt preferences app (ime-preferences)"   ON)
option(INSTALL_SYSTEMD_USER_UNIT "Install a systemd user service for the daemon" ON)

# The optional Qt Quick candidate panel (qmlpanel) lives in its own project —
# it needs a patched fcitx5 (waylandim public API), which the core does not:
# https://github.com/titoo-dev/fcitx5-qmlpanel. Everything here builds against
# any stock fcitx5.

if(BUILD_ENGINE)
  add_subdirectory(engine)
endif()
if(BUILD_DAEMON)
  add_subdirectory(daemon)
endif()
if(BUILD_PREFERENCES)
  add_subdirectory(preferences)
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
               "preferences=${BUILD_PREFERENCES}")
