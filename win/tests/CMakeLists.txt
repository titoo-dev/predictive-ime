# Tests de non-régression du text service (lancés par ctest, CI Windows).
add_executable(test_win_tsf
  test_win_tsf.cpp
  ../tsf/CandidateWindow.cpp
  ../tsf/Theme.cpp
  ../tsf/KeyboardLayout.cpp)
target_compile_features(test_win_tsf PRIVATE cxx_std_17)
target_include_directories(test_win_tsf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(test_win_tsf PRIVATE
  ws2_32 ole32 oleaut32 uuid user32 gdi32 advapi32
  d2d1 dwrite dwmapi shcore shlwapi)
if(MSVC)
  target_compile_options(test_win_tsf PRIVATE /utf-8)
  target_compile_definitions(test_win_tsf PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

add_test(NAME win-tsf COMMAND test_win_tsf)

# L'IME installé sur la machine qui lance les tests : sauté (77) là où Predict
# n'est pas installé — en CI notamment.
add_test(NAME win-layout-installed COMMAND test_win_tsf --installed)
set_tests_properties(win-layout-installed PROPERTIES SKIP_RETURN_CODE 77)

# Règles de disposition du setup (scripts/PredictLayout.ps1).
find_program(PWSH_EXE NAMES pwsh powershell)
if(PWSH_EXE)
  add_test(NAME win-setup-rules
    COMMAND "${PWSH_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/tests/Test-PredictLayout.ps1")
endif()
