cmake_minimum_required(VERSION 3.24)
project(mxl-multiviewer VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MV_BUILD_TESTS "Build unit tests" ON)
option(MV_WITH_NMOS "Link Sony nmos-cpp" OFF)
option(MV_WITH_BLEND2D "Rasterise overlays with Blend2D" ON)

if(MV_WITH_BLEND2D)
    include(FetchContent)
    FetchContent_Declare(blend2d_src
        URL https://blend2d.com/download/blend2d-0.21.2.tar.gz
        URL_HASH SHA256=234ba57a64153c96885a6271f15fdcf6c594ac8cb03adaad19abb8d3b2b533fa
    )
    FetchContent_GetProperties(blend2d_src)
    if(NOT blend2d_src_POPULATED)
        FetchContent_Populate(blend2d_src)
    endif()
    set(BLEND2D_STATIC ON CACHE BOOL "" FORCE)
    set(BLEND2D_TEST OFF CACHE BOOL "" FORCE)
    set(BLEND2D_DEMOS OFF CACHE BOOL "" FORCE)
    set(BLEND2D_NO_INSTALL ON CACHE BOOL "" FORCE)
    set(BLEND2D_NO_JIT_LOGGING ON CACHE BOOL "" FORCE)
    add_subdirectory(${blend2d_src_SOURCE_DIR} ${CMAKE_BINARY_DIR}/blend2d EXCLUDE_FROM_ALL)
endif()
set(NMOS_CPP_DIR "" CACHE PATH "Path to sony/nmos-cpp Development/")

find_package(Threads REQUIRED)

add_library(mv-core STATIC
    src/util/sha1.cpp
    src/util/uuid.cpp
    src/util/logging.cpp
    src/media/timebase.cpp
    src/media/v210.cpp
    src/media/scale.cpp
    src/media/ppm.cpp
    src/media/alarm.cpp
    src/media/audioring.cpp
    src/media/overlay.cpp
    src/config/config.cpp
    src/config/store.cpp
    src/domain/scan.cpp
    src/layout/model.cpp
    src/layout/migrate.cpp
    src/layout/geometry.cpp
    src/nmos/ids.cpp
    src/control/tsl.cpp
    src/ops/metrics.cpp
    src/ops/httpserver.cpp
    src/ops/api.cpp
    src/media/flowtext.cpp
    src/media/jpeg.cpp
    src/layout/book.cpp
    src/util/httpclient.cpp
    src/app/runtime.cpp
)
target_include_directories(mv-core PUBLIC src third_party)
target_link_libraries(mv-core PUBLIC Threads::Threads)
target_compile_options(mv-core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(MV_WITH_BLEND2D)
    set(DEJAVU_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/media/dejavu_sans.hpp)
    add_custom_command(
        OUTPUT ${DEJAVU_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/third_party/dejavu/DejaVuSans.ttf
                -DOUTPUT=${DEJAVU_HEADER}
                -DNAMESPACE=mv::font
                -DVAR=dejavuSans
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBytes.cmake
        DEPENDS third_party/dejavu/DejaVuSans.ttf cmake/EmbedBytes.cmake
        COMMENT "Embedding DejaVu Sans"
        VERBATIM
    )
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/media/overlay.cpp PROPERTIES OBJECT_DEPENDS "${DEJAVU_HEADER}")
    target_include_directories(mv-core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mv-core PUBLIC blend2d::blend2d)
    target_compile_definitions(mv-core PUBLIC MV_WITH_BLEND2D=1)
endif()

if(MV_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_config.cpp
        tests/unit/test_layout.cpp
        tests/unit/test_media.cpp
        tests/unit/test_control.cpp
    )
    target_include_directories(unit-tests PRIVATE src third_party)
    target_link_libraries(unit-tests PRIVATE mv-core)
    target_compile_options(unit-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

find_package(mxl CONFIG QUIET)
include(CheckLanguage)
check_language(CUDA)
set(MV_CUDA_LANGUAGE OFF)
if(CMAKE_CUDA_COMPILER)
    set(CMAKE_CUDA_ARCHITECTURES 75 86 89)
    enable_language(CUDA)
    set(MV_CUDA_LANGUAGE ON)
endif()

if(mxl_FOUND)
    set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_generated.hpp)
    find_program(NPM_EXECUTABLE npm)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html AND NOT NPM_EXECUTABLE)
        set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding prebuilt web UI"
        )
        set(MV_UI_SOURCE ${WEBUI_GENERATED})
    elseif(NPM_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json)
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
        file(GLOB_RECURSE MV_WEB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/src/*)
        add_custom_command(
            OUTPUT ${WEBUI_HTML}
            COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
            COMMAND ${NPM_EXECUTABLE} run build
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            DEPENDS web/package.json web/index.html web/vite.config.js ${MV_WEB_SOURCES}
            COMMENT "Building Vue web UI"
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED} -DVAR=indexHtml
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding web UI"
        )
        set(MV_UI_SOURCE ${WEBUI_GENERATED})
    else()
        set(MV_UI_SOURCE "")
    endif()
    add_executable(mxl-multiviewer src/main.cpp src/mxlio/engine.cpp src/nmos/node.cpp ${MV_UI_SOURCE})
    target_include_directories(mxl-multiviewer PRIVATE src third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
    if(MV_UI_SOURCE)
        target_compile_definitions(mv-core PUBLIC MV_HAS_UI=1)
        target_include_directories(mv-core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/ops/api.cpp PROPERTIES OBJECT_DEPENDS "${WEBUI_GENERATED}")
    endif()
    target_link_libraries(mxl-multiviewer PRIVATE mv-core mxl::mxl)
    target_compile_options(mxl-multiviewer PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(MV_CUDA_LANGUAGE)
        target_sources(mxl-multiviewer PRIVATE src/media/cuda_compose.cu)
        set_target_properties(mxl-multiviewer PROPERTIES CUDA_RUNTIME_LIBRARY Static CUDA_ARCHITECTURES "75;86;89")
        target_compile_definitions(mxl-multiviewer PRIVATE MV_WITH_CUDA=1)
    else()
        target_sources(mxl-multiviewer PRIVATE src/media/cuda_stub.cpp)
    endif()
    if(MV_WITH_NMOS)
        if(NOT NMOS_CPP_DIR)
            message(FATAL_ERROR "MV_WITH_NMOS=ON requires -DNMOS_CPP_DIR")
        endif()
        set(NMOS_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JSON_SCHEMA_VALIDATOR ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_SUPPLIED_JWT_CPP ON CACHE BOOL "" FORCE)
        set(NMOS_CPP_USE_AVAHI OFF CACHE BOOL "" FORCE)
        set(CMAKE_CXX_STANDARD 17)
        add_subdirectory(${NMOS_CPP_DIR} ${CMAKE_BINARY_DIR}/nmos-cpp EXCLUDE_FROM_ALL)
        set(CMAKE_CXX_STANDARD 20)
        target_compile_definitions(mxl-multiviewer PRIVATE MV_WITH_NMOS=1)
        target_link_libraries(mxl-multiviewer PRIVATE nmos-cpp::nmos-cpp)
        set_source_files_properties(src/nmos/node.cpp PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            COMPILE_OPTIONS "-w")
    endif()
    add_executable(mxl-mv-writer tests/tools/pattern_writer.cpp)
    target_link_libraries(mxl-mv-writer PRIVATE mxl::mxl)
    target_compile_features(mxl-mv-writer PRIVATE cxx_std_20)
    add_executable(mxl-mv-sample tests/tools/sample_flow.cpp)
    target_link_libraries(mxl-mv-sample PRIVATE mxl::mxl)
    target_compile_features(mxl-mv-sample PRIVATE cxx_std_20)
endif()
