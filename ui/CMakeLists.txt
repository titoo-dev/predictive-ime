cmake_minimum_required(VERSION 3.16)
project(fcitx5-qmlpanel VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(GNUInstallDirs)

find_package(Fcitx5Core REQUIRED)
# Wayland (display) + WaylandIM (getInputMethodV2Raw, via fcitx5 patché)
find_package(Fcitx5Module REQUIRED COMPONENTS Wayland WaylandIM)
find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

# Bindings C du protocole input-method-unstable-v2 (vendorisé : absent de
# wayland-protocols standard). private-code → symboles non exportés (pas de
# collision avec libwaylandim qui embarque les siens).
set(PROTO ${CMAKE_CURRENT_SOURCE_DIR}/input-method-unstable-v2.xml)
set(PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/input-method-unstable-v2-client-protocol.h)
set(PROTO_SRC ${CMAKE_CURRENT_BINARY_DIR}/input-method-unstable-v2-protocol.c)
add_custom_command(OUTPUT ${PROTO_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${PROTO} ${PROTO_HDR}
    DEPENDS ${PROTO} VERBATIM)
add_custom_command(OUTPUT ${PROTO_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${PROTO} ${PROTO_SRC}
    DEPENDS ${PROTO} VERBATIM)

add_library(qmlpanel MODULE qmlui.cpp panelview.cpp ${PROTO_SRC} ${PROTO_HDR})
set_target_properties(qmlpanel PROPERTIES PREFIX "lib")
target_include_directories(qmlpanel PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# Qt définit des macros emit/foreach qui entrent en collision avec des méthodes
# fcitx du même nom → on désactive ces mots-clés (on ne les utilise pas).
target_compile_definitions(qmlpanel PRIVATE QT_NO_KEYWORDS QT_NO_FOREACH)
target_link_libraries(qmlpanel
    Fcitx5::Core
    Fcitx5::Module::Wayland
    Fcitx5::Module::WaylandIM
    PkgConfig::WaylandClient
    Qt6::Gui Qt6::Qml Qt6::Quick)

# Chemins Qt bakés (fcitx n'est pas une appli Qt wrappée → qputenv au chargement)
if(QMLPANEL_QT_PLUGIN_PATH)
    target_compile_definitions(qmlpanel PRIVATE
        QMLPANEL_QT_PLUGIN_PATH="${QMLPANEL_QT_PLUGIN_PATH}")
endif()
if(QMLPANEL_QML_IMPORT_PATH)
    target_compile_definitions(qmlpanel PRIVATE
        QMLPANEL_QML_IMPORT_PATH="${QMLPANEL_QML_IMPORT_PATH}")
endif()

# Rendu hors ligne des états de la barre en PNG (test QML + relecture du
# design) : ne dépend que de Qt, pas de fcitx ni de wayland.
if(BUILD_TESTING)
    add_executable(panel_preview panel_preview.cpp panelview.cpp)
    target_compile_definitions(panel_preview PRIVATE QT_NO_KEYWORDS QT_NO_FOREACH)
    target_link_libraries(panel_preview Qt6::Gui Qt6::Qml Qt6::Quick)
    if(QMLPANEL_QT_PLUGIN_PATH)
        target_compile_definitions(panel_preview PRIVATE
            QMLPANEL_QT_PLUGIN_PATH="${QMLPANEL_QT_PLUGIN_PATH}")
    endif()
    if(QMLPANEL_QML_IMPORT_PATH)
        target_compile_definitions(panel_preview PRIVATE
            QMLPANEL_QML_IMPORT_PATH="${QMLPANEL_QML_IMPORT_PATH}")
    endif()
endif()

install(TARGETS qmlpanel DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5")
install(FILES qmlpanel-addon.conf
        RENAME qmlpanel.conf
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5/addon")
