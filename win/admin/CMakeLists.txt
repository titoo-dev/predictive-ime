# Panneau d'administration (predict-admin.exe) — Win32 natif, sans dépendance
# tierce, lié en /MT comme le text service : il doit se lancer sur une machine
# qui n'a pas le redistribuable VC++.
cmake_minimum_required(VERSION 3.16)

add_executable(predict-admin WIN32
  predict-admin.cpp
  predict-admin.rc)
target_compile_features(predict-admin PRIVATE cxx_std_17)
target_include_directories(predict-admin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(predict-admin PRIVATE
  nlohmann_json::nlohmann_json
  ws2_32 comctl32 shell32 advapi32 user32 gdi32 ole32 version)
if(MSVC)
  target_compile_options(predict-admin PRIVATE /utf-8)
  target_compile_definitions(predict-admin PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_property(TARGET predict-admin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Le manifeste est fourni par le .rc (contrôles communs v6, DPI, UTF-8) :
  # on empêche le linker d'en générer un second qui entrerait en conflit.
  target_link_options(predict-admin PRIVATE /MANIFEST:NO)
endif()
# Le .rc référence le manifeste et l'icône par chemin relatif.
set_source_files_properties(predict-admin.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/predict-admin.manifest;${CMAKE_CURRENT_SOURCE_DIR}/../tsf/predict.ico")

install(TARGETS predict-admin RUNTIME DESTINATION bin)
