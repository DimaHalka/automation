cmake_minimum_required(VERSION 3.22)
project(AutomationDashboard VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Default Qt6 path if not explicitly provided
if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "C:/Users/DGalka/Qt6/lib/cmake/Qt6")
    list(APPEND CMAKE_PREFIX_PATH "C:/Users/DGalka/Qt6")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)

# Output Release binary directly into cpp/release/
set(RELEASE_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RELEASE_OUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RELEASE_OUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RELEASE_OUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${RELEASE_OUT_DIR}")

set(SOURCES
    src/main.cpp
    src/Config.h
    src/Config.cpp
    src/WorkspaceManager.h
    src/WorkspaceManager.cpp
    src/AdoClient.h
    src/AdoClient.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    resources/app.rc
)

add_executable(Dashboard WIN32 ${SOURCES})

target_include_directories(Dashboard PRIVATE src)

if(MSVC)
    target_compile_options(Dashboard PRIVATE /utf-8 /W3)
endif()

target_link_libraries(Dashboard PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    dwmapi
    user32
)

# Find windeployqt for automatic deployment of Qt DLLs and plugins into cpp/release/
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "C:/Users/DGalka/Qt6/bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET Dashboard POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-translations
            --no-opengl-sw
            --no-system-d3d-compiler
            --no-compiler-runtime
            "$<TARGET_FILE:Dashboard>"
        COMMENT "Running windeployqt on Dashboard.exe..."
    )
endif()
