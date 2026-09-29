# Sonde en ligne de commande utilisant le VRAI client C++ (core/daemon_client.h).
# Elle existe pour que ce chemin soit TESTÉ sous Windows et pas seulement
# supposé : les scripts PowerShell passent par .NET et ne l'exercent jamais.
add_executable(predict-probe predict-probe.cpp)
target_include_directories(predict-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(predict-probe PRIVATE nlohmann_json::nlohmann_json ws2_32)
if(MSVC)
  target_compile_options(predict-probe PRIVATE /utf-8)
  target_compile_definitions(predict-probe PRIVATE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
install(TARGETS predict-probe RUNTIME DESTINATION bin)

# Aperçu visuel de la barre de candidats (captures PNG de chaque disposition),
# sans installer le text service. Outil de développement : pas installé.
add_executable(candidate-preview
  candidate-preview.cpp
  ../tsf/CandidateWindow.cpp
  ../tsf/Module.cpp
  ../tsf/Theme.cpp
  ../tsf/StatusIcon.cpp)
target_compile_features(candidate-preview PRIVATE cxx_std_17)
target_include_directories(candidate-preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(candidate-preview PRIVATE
  ws2_32 ole32 user32 gdi32 advapi32 windowscodecs d2d1 dwrite dwmapi shcore)
if(MSVC)
  target_compile_options(candidate-preview PRIVATE /utf-8)
  target_compile_definitions(candidate-preview PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
