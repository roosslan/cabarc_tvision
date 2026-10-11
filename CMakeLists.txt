cmake_minimum_required(VERSION 3.16)

project(Cabine LANGUAGES C CXX)

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
# Чтение ZIP и RAR. Библиотеки подключены git-субмодулями в third_party
# и собираются из исходников статически.
# ---------------------------------------------------------------------------
set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
foreach (lib zlib minizip-ng unrar)
    if (NOT EXISTS "${THIRD_PARTY}/${lib}/README" AND NOT EXISTS "${THIRD_PARTY}/${lib}/README.md"
        AND NOT EXISTS "${THIRD_PARTY}/${lib}/readme.txt")
        message(FATAL_ERROR "Не найден ${THIRD_PARTY}/${lib}. Необходимо выполнить "
                            "git submodule update --init.")
    endif()
endforeach()

# zlib — сжатие Deflate для minizip-ng.
file(GLOB ZLIB_SOURCES "${THIRD_PARTY}/zlib/*.c")
add_library(cabine_zlib STATIC ${ZLIB_SOURCES})
target_include_directories(cabine_zlib PUBLIC "${THIRD_PARTY}/zlib")
target_compile_definitions(cabine_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
if (MSVC)
    target_compile_options(cabine_zlib PRIVATE /W0)
endif()

# minizip-ng — ZIP: Deflate, ZIP64, пароли PKWARE и WinZip AES (Windows CNG).
set(MZ "${THIRD_PARTY}/minizip-ng")
# Настройки платформы для MSVC (в оригинале их определяет CMakeLists.txt minizip-ng).
set(HAVE_DIRENT_H 0)
set(HAVE_SYS_DIRENT_H 0)
set(HAVE_INTTYPES_H 1)
set(HAVE_STDINT_H 1)
set(HAVE_PDIR 0)
set(HAVE_FSEEKO 0)
set(HAVE_SYMLINK 0)
set(HAVE_READLINK 0)
configure_file("${MZ}/mz_config.h.cmakein" "${CMAKE_CURRENT_BINARY_DIR}/minizip/mz_config.h" @ONLY)
add_library(cabine_minizip STATIC
    ${MZ}/mz_crypt.c ${MZ}/mz_os.c ${MZ}/mz_strm.c ${MZ}/mz_strm_buf.c ${MZ}/mz_strm_mem.c
    ${MZ}/mz_strm_split.c ${MZ}/mz_zip.c ${MZ}/mz_zip_rw.c ${MZ}/mz_os_win32.c
    ${MZ}/mz_strm_os_win32.c ${MZ}/mz_strm_zlib.c ${MZ}/mz_strm_pkcrypt.c ${MZ}/mz_strm_wzaes.c
    ${MZ}/mz_crypt_winvista.c ${MZ}/mz_crypt_winxp.c
)
target_include_directories(cabine_minizip PUBLIC "${MZ}" "${CMAKE_CURRENT_BINARY_DIR}/minizip")
target_compile_definitions(cabine_minizip PRIVATE
    HAVE_ZLIB ZLIB_COMPAT HAVE_PKCRYPT HAVE_WZAES _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
target_link_libraries(cabine_minizip PUBLIC cabine_zlib bcrypt ncrypt crypt32)
if (MSVC)
    target_compile_options(cabine_minizip PRIVATE /W0)
endif()

# UnRAR — распаковка RAR4 и RAR5 (интерфейс unrar.dll, собранный статически).
set(UNRAR_FILES archive arcread blake2s cmddata consio crc crypt dll encname errhnd extinfo
    extract filcreat file filefn filestr find getbits global hash headers isnt largepage match
    motw options pathfn qopen rar rarvm rawread rdwrfn rijndael rs rs16 scantree secpassword
    sha1 sha256 smallfn strfn strlist system threadpool timefn ui unicode unpack volume)
set(UNRAR_SOURCES)
foreach (f ${UNRAR_FILES})
    list(APPEND UNRAR_SOURCES "${THIRD_PARTY}/unrar/${f}.cpp")
endforeach()
add_library(cabine_unrar STATIC ${UNRAR_SOURCES})
target_include_directories(cabine_unrar PUBLIC "${THIRD_PARTY}/unrar")
target_compile_definitions(cabine_unrar PRIVATE RARDLL UNRAR SILENT _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(cabine_unrar PRIVATE /W0 /EHsc)
endif()

# ---------------------------------------------------------------------------
# Приложение
# ---------------------------------------------------------------------------
add_executable(cabine
    src/main.cpp
    src/app.cpp
    src/archive.cpp
    src/cabfile.cpp
    src/dialogs.cpp
    src/format.cpp
    src/fsutil.cpp
    src/hotkeys.cpp
    src/panel.cpp
    src/sfxstub.cpp
    src/shellreg.cpp
    src/viewer.cpp
    src/cabine.rc
    src/sfxstub.rc
)
target_compile_features(cabine PRIVATE cxx_std_17)
target_compile_definitions(cabine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(cabine PRIVATE tvision cabine_minizip cabine_unrar cabinet shell32 ole32 user32 advapi32)
if (MSVC)
    target_compile_options(cabine PRIVATE /W3)
endif()

# ---------------------------------------------------------------------------
# Программа-распаковщик самораспаковывающихся архивов. Встраивается
# в cabine.exe ресурсом; при создании архива к ней дописывается CAB.
# ---------------------------------------------------------------------------
add_executable(cabine-sfx
    src/sfx/sfxmain.cpp
    src/cabfile.cpp
    src/dialogs.cpp
    src/format.cpp
    src/fsutil.cpp
    src/hotkeys.cpp
    src/sfx/sfx.rc
)
target_compile_features(cabine-sfx PRIVATE cxx_std_17)
target_compile_definitions(cabine-sfx PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(cabine-sfx PRIVATE tvision cabinet shell32 ole32 user32 advapi32)
if (MSVC)
    target_compile_options(cabine-sfx PRIVATE /W3)
endif()

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sfxstub/$<CONFIG>/sfxstub_path.h"
     CONTENT "#define CABINE_SFX_STUB \"$<TARGET_FILE:cabine-sfx>\"
")
target_include_directories(cabine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sfxstub/$<CONFIG>")
add_dependencies(cabine cabine-sfx)

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

# ---------------------------------------------------------------------------
# Тесты ядра (без интерфейса): ctest -C Release
# ---------------------------------------------------------------------------
option(CABINE_TESTS "Собирать тесты" ON)
if (CABINE_TESTS)
    enable_testing()
    add_executable(cabine-tests
        tests/enginetest.cpp
        src/archive.cpp
        src/cabfile.cpp
        src/format.cpp
        src/fsutil.cpp
    )
    target_compile_features(cabine-tests PRIVATE cxx_std_17)
    target_compile_definitions(cabine-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        "CABINE_SFX_STUB=\"$<TARGET_FILE:cabine-sfx>\""
        "CABINE_TEST_DATA=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data\"")
    target_link_libraries(cabine-tests PRIVATE cabine_minizip cabine_unrar cabinet shell32 ole32 user32 advapi32)
    if (MSVC)
        target_compile_options(cabine-tests PRIVATE /W3 /utf-8)
    endif()
    add_dependencies(cabine-tests cabine-sfx)
    add_test(NAME engine COMMAND cabine-tests)
endif()
