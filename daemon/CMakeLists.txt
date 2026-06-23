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

# Prédicteur neuronal (libllama). OFF par défaut → le daemon n-gram build sans
# dépendance lourde. ON : ajoute neural_predict (CLI d'isolation) ; à terme,
# predictord lui-même linkera libllama. Le runtime a besoin de GGML_BACKEND_PATH
# pointant sur le dossier des backends ggml-cpu-*.so (cf. wrapper du flake).
option(WITH_NEURAL "Build the neural (libllama) predictor" OFF)
if(WITH_NEURAL)
  find_package(llama REQUIRED)
  find_package(ggml REQUIRED)
  add_executable(neural_predict neural_predict.cpp)
  target_link_libraries(neural_predict llama ggml)
  install(TARGETS neural_predict DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(FILES sample-corpus.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ime-predictord")
