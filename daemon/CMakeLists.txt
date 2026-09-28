cmake_minimum_required(VERSION 3.16)
project(ime-predictord VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)

# /utf-8 IMPÉRATIF sous MSVC : les sources sont en UTF-8 (accents français) et
# le modèle parcourt l'UTF-8 octet par octet ; sans ce drapeau MSVC les relit
# dans la codepage système et les suggestions accentuées sortent en mojibake.
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

find_package(nlohmann_json REQUIRED)
# libcurl : reformulation via API externe (Groq) — cf reformulate_http.cpp.
find_package(CURL REQUIRED)

add_executable(predictord predictord.cpp reformulate_http.cpp)
target_link_libraries(predictord nlohmann_json::nlohmann_json CURL::libcurl)
install(TARGETS predictord DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Windows : AF_UNIX natif (Win10 1803+) via ws2_32.
if(WIN32)
  target_link_libraries(predictord ws2_32)
  # Sous-système WINDOWS, PAS console : une application console meurt en
  # STATUS_CONTROL_C_EXIT (0xC000013A) quand la console qui l'a lancée se ferme
  # — le daemon s'éteignait en cours de session. /ENTRY:mainCRTStartup garde le
  # `int main(argc, argv)` standard (pas de WinMain à écrire), et
  # detachFromConsole() se rattache au terminal quand il y en a un, sinon écrit
  # dans %LOCALAPPDATA%\ime-predictord\predictord.log.
  set_target_properties(predictord PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(predictord PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

# Outil de bench (le "spike" latence), optionnel.
add_executable(bench_ngram bench_ngram.cpp)
install(TARGETS bench_ngram DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Prédicteur neuronal (libllama). OFF par défaut → le daemon n-gram build sans
# dépendance lourde. ON : ajoute neural_predict (CLI d'isolation) ; à terme,
# predictord lui-même linkera libllama. Le runtime a besoin de GGML_BACKEND_PATH
# pointant sur le dossier des backends ggml-cpu-*.so (cf. wrapper du flake).
option(WITH_NEURAL "Build the neural (libllama) predictor" OFF)
if(WITH_NEURAL)
  find_package(llama REQUIRED)
  find_package(ggml REQUIRED)
  # CLI d'isolation/diagnostic.
  add_executable(neural_predict neural_predict.cpp neural.cpp)
  target_link_libraries(neural_predict llama ggml)
  install(TARGETS neural_predict DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # Intégration daemon : predictord linke le prédicteur neuronal (WITH_NEURAL
  # active le bloc #ifdef dans predictord.cpp ; neural OFF par défaut côté config).
  target_sources(predictord PRIVATE neural.cpp)
  target_link_libraries(predictord llama ggml)
  target_compile_definitions(predictord PRIVATE WITH_NEURAL=1)
endif()

install(FILES sample-corpus.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ime-predictord")
