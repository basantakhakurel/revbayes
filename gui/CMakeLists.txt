cmake_minimum_required(VERSION 3.21)
project(RevStudio VERSION 0.1.0 LANGUAGES CXX)

# RevStudio is a Qt 6 front end for RevBayes. It never links the interpreter: it talks to `rb --server` over a pipe
# (protocol: GUI_Implementation_Note.md, section 6.4). That is why it is an independent CMake project, built and
# released separately from the interpreter (which uses meson or the generated CMake files under src/).

set(CMAKE_CXX_STANDARD 17)                 # independent of rb's C++23
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source floor is Qt 6.4 (Ubuntu 24.04's distro Qt); release builds use one pinned Qt (see .github/workflows/gui-build.yml).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()                # Qt 6.3+: turns on AUTOMOC and AUTOUIC, but NOT AUTORCC
set(CMAKE_AUTORCC ON)                      # without this the .qrc is silently never compiled (found in phase 0)

option(REVSTUDIO_BUILD_TESTS "Build the QtTest suites and the mock backend" ON)
option(REVSTUDIO_SANITIZE "Enable ASan and UBSan (GCC and Clang)" OFF)

if(REVSTUDIO_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warnings for our own targets only (not for Qt headers).
add_library(revstudio_warnings INTERFACE)
if(MSVC)
  target_compile_options(revstudio_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(revstudio_warnings INTERFACE -Wall -Wextra)
endif()

configure_file(src/app/revstudio_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/revstudio_version.h @ONLY)

# Non-UI logic: linked by the application and by the tests. QtCore (+Gui for VariablesModel's QColor/QBrush
# BackgroundRole, which needs no display) only, so it is testable without one.
add_library(revstudio_core STATIC
  src/app/ConsoleHistory.cpp
  src/app/Settings.cpp
  src/app/VariablesModel.cpp
  src/backend/ProtocolCodec.cpp
  src/backend/BackendLocator.cpp
  src/backend/BackendProcess.cpp
  src/backend/Session.cpp)
target_include_directories(revstudio_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(revstudio_core PUBLIC Qt6::Core Qt6::Gui PRIVATE revstudio_warnings)

qt_add_executable(RevStudio
  src/main.cpp
  src/app/ConsoleWidget.cpp
  src/app/EditorTabs.cpp
  src/app/FileExplorerPanel.cpp
  src/app/HelpDock.cpp
  src/app/MainWindow.cpp
  src/app/PreferencesDialog.cpp
  src/app/RevHighlighter.cpp
  src/app/ScriptEditor.cpp
  src/app/SelfTest.cpp
  src/app/Theme.cpp
  src/app/VariablesPanel.cpp
  resources/revstudio.qrc)
if(WIN32)
  target_sources(RevStudio PRIVATE packaging/windows/revstudio.rc)
endif()
target_link_libraries(RevStudio PRIVATE revstudio_core Qt6::Widgets Qt6::Svg revstudio_warnings)
set_target_properties(RevStudio PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE OFF)

if(REVSTUDIO_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
  add_subdirectory(tests)                  # QtTest executables and the mock-rb backend
endif()

include(GNUInstallDirs)
install(TARGETS RevStudio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/linux/revstudio.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/linux/revstudio.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
