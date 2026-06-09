cmake_minimum_required(VERSION 3.16)
project(fcitx5-predict VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(GNUInstallDirs)

find_package(Fcitx5Core REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(predict MODULE predict.cpp)
target_link_libraries(predict Fcitx5::Core nlohmann_json::nlohmann_json)
set_target_properties(predict PROPERTIES PREFIX "lib")

install(TARGETS predict DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5")
install(FILES predict-addon.conf
        RENAME predict.conf
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5/addon")
install(FILES predict.conf
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod")
