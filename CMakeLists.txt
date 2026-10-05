cmake_minimum_required(VERSION 3.16)

project(Cabine LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Cabine работает только под Windows: используется Cabinet API (FCI/FDI).")
endif()

# Статическая RTL: исполняемый файл не зависит от VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Turbo Vision подключён git-субмодулем tvision (magiblot/tvision).
set(TV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tvision" CACHE PATH "Путь к исходникам Turbo Vision")
if (NOT EXISTS "${TV_ROOT}/include/tvision/tv.h")
    message(FATAL_ERROR "Не найден Turbo Vision в ${TV_ROOT}. Необходимо выполнить "
                        "git submodule update --init либо указать -DTV_ROOT=<путь>.")
endif()

# ---------------------------------------------------------------------------
# Turbo Vision собирается прямо из исходников, без его CMakeLists.txt:
# файл строк интерфейса tvtext2.cpp заменён русифицированной копией.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TV_SOURCES "${TV_ROOT}/source/*/*.cpp")
list(REMOVE_ITEM TV_SOURCES
    "${TV_ROOT}/source/tvision/geninc.cpp"
    "${TV_ROOT}/source/tvision/tvtext2.cpp"
)

add_library(tvision STATIC ${TV_SOURCES} src/tvtext_ru.cpp)
target_compile_features(tvision PUBLIC cxx_std_14)
target_include_directories(tvision PUBLIC "${TV_ROOT}/include")
target_include_directories(tvision PRIVATE
    "${TV_ROOT}/include/tvision"
    "${TV_ROOT}/include/tvision/compat"
)
target_compile_definitions(tvision
    PUBLIC _CRT_SECURE_NO_WARNINGS
    PRIVATE TVISION_NO_STL
)
if (MSVC)
    target_compile_options(tvision PUBLIC
        /wd4068 /wd4146 /wd4166 /wd4244 /wd4250 /wd4267 /wd4996
        /permissive- /Zc:__cplusplus /Zc:externConstexpr /Zc:inline
        /utf-8
    )
endif()
target_precompile_headers(tvision PRIVATE "${TV_ROOT}/include/pch.h")
set_source_files_properties(src/tvtext_ru.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# ---------------------------------------------------------------------------
# Приложение
# ---------------------------------------------------------------------------
add_executable(cabine
    src/main.cpp
    src/app.cpp
    src/cabfile.cpp
    src/dialogs.cpp
    src/format.cpp
    src/fsutil.cpp
    src/hotkeys.cpp
    src/panel.cpp
    src/shellreg.cpp
    src/viewer.cpp
    src/cabine.rc
)
target_compile_features(cabine PRIVATE cxx_std_17)
target_compile_definitions(cabine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(cabine PRIVATE tvision cabinet shell32 ole32 user32 advapi32)
if (MSVC)
    target_compile_options(cabine PRIVATE /W3)
endif()

# ---------------------------------------------------------------------------
# Посредник для контекстного меню Проводника: без окна консоли собирает пути
# от всех запущенных экземпляров и один раз запускает cabine.exe.
# ---------------------------------------------------------------------------
add_executable(cabine-shell WIN32
    src/shell/shellmain.cpp
    src/fsutil.cpp
    src/cabine.rc
)
target_compile_features(cabine-shell PRIVATE cxx_std_17)
target_compile_definitions(cabine-shell PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(cabine-shell PRIVATE shell32 ole32 user32)
if (MSVC)
    target_compile_options(cabine-shell PRIVATE /W3 /utf-8)
endif()
