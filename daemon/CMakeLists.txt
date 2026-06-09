cmake_minimum_required(VERSION 3.16)
project(ime-predictord VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

find_package(nlohmann_json REQUIRED)

add_executable(predictord predictord.cpp)
target_link_libraries(predictord nlohmann_json::nlohmann_json)
install(TARGETS predictord DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Outil de bench (le "spike" latence), optionnel.
add_executable(bench_ngram bench_ngram.cpp)
install(TARGETS bench_ngram DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES sample-corpus.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ime-predictord")
