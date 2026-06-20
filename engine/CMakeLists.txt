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

# ----------------------------------------------------------------- tests -----
# Harnais headless : pilote le vrai libpredict.so via l'infra de test fcitx5.
include(CTest)
if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  find_package(Fcitx5ModuleTestFrontend REQUIRED)
  find_package(Fcitx5ModuleTestIM REQUIRED)

  # Prefix du paquet fcitx5 (déduit du dossier CMake de Fcitx5Core) : addons de
  # test (testfrontend/testim/testui) + leurs .conf.
  get_filename_component(FCITX5_PREFIX "${Fcitx5Core_DIR}/../../.." ABSOLUTE)
  set(FCITX5_ADDON_DIR "${FCITX5_PREFIX}/lib/fcitx5")
  set(FCITX5_DATA_DIR "${FCITX5_PREFIX}/share/fcitx5")

  # predict.conf doit être trouvable sous <data>/inputmethod et <data>/addon
  # par setupTestingEnvironment → arbo dédiée dans le build dir.
  set(TEST_DATA "${CMAKE_CURRENT_BINARY_DIR}/testdata")
  file(MAKE_DIRECTORY "${TEST_DATA}/inputmethod" "${TEST_DATA}/addon")
  configure_file(predict.conf "${TEST_DATA}/inputmethod/predict.conf" COPYONLY)
  configure_file(predict-addon.conf "${TEST_DATA}/addon/predict.conf" COPYONLY)

  add_executable(test_predict_engine test_predict_engine.cpp)
  target_link_libraries(test_predict_engine
    Fcitx5::Core Fcitx5::Module::TestFrontend
    nlohmann_json::nlohmann_json Threads::Threads)
  target_compile_definitions(test_predict_engine PRIVATE
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PREDICT_ADDON_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    TEST_DATA_DIR="${TEST_DATA}"
    FCITX_ADDON_DIR="${FCITX5_ADDON_DIR}"
    FCITX_DATA_DIR="${FCITX5_DATA_DIR}")
  add_test(NAME engine COMMAND test_predict_engine)
endif()
