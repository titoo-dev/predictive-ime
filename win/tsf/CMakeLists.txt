# Text service TSF — DLL COM in-process chargée dans CHAQUE application.
#
# Contraintes assumées ici :
#   - aucune dépendance tierce (le Windows SDK fournit msctf/ctffunc) ;
#   - /MT : la DLL atterrit dans des process qui n'ont pas forcément le
#     redistribuable VC++ — une liaison dynamique au CRT ferait échouer le
#     chargement SANS message (le service serait juste absent) ;
#   - /utf-8 : les sources portent des accents et le cœur parcourt l'UTF-8
#     octet par octet.
cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(predict-tsf SHARED
  dllmain.cpp
  Module.cpp
  TextService.cpp
  KeyHandling.cpp
  CandidateWindow.cpp
  Theme.cpp
  LangBarButton.cpp
  StatusIcon.cpp
  KeyboardLayout.cpp
  ../../core/engine_core.cpp
  predict-tsf.def
  predict-tsf.rc
)

target_include_directories(predict-tsf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(predict-tsf PRIVATE
  nlohmann_json::nlohmann_json
  ws2_32 ole32 oleaut32 user32 gdi32 advapi32 shell32 imm32
  d2d1 dwrite dwmapi shcore shlwapi)

if(MSVC)
  target_compile_options(predict-tsf PRIVATE /utf-8)
  # WIN32_LEAN_AND_MEAN au niveau du PROJET : sans lui, le premier windows.h
  # rencontré tire l'ancien winsock.h et tout winsock2.h se redéfinit ensuite
  # (des dizaines de C2011). Le mettre ici rend l'ordre d'inclusion indifférent.
  target_compile_definitions(predict-tsf PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_property(TARGET predict-tsf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Rendu de la barre (Direct2D/DirectWrite/DWM) chargé à la DEMANDE : la DLL
  # entre dans chaque process qui reçoit de la frappe, et la plupart
  # n'afficheront jamais la barre — inutile de leur faire charger ces DLL.
  target_link_libraries(predict-tsf PRIVATE delayimp)
  target_link_options(predict-tsf PRIVATE
    /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:dwmapi.dll
    /DELAYLOAD:api-ms-win-shcore-scaling-l1-1-1.dll /DELAYLOAD:shlwapi.dll)
endif()

install(TARGETS predict-tsf RUNTIME DESTINATION bin)
