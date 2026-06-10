cmake_minimum_required(VERSION 3.20)
project(ime-preferences CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick)

add_executable(ime-preferences prefs.cpp)
target_link_libraries(ime-preferences PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick)

install(TARGETS ime-preferences DESTINATION bin)
install(FILES ime-preferences.desktop DESTINATION share/applications)
