cmake_minimum_required(VERSION 3.21)

project(ResearchSSH-Next
    VERSION 1.0.0
    DESCRIPTION "Windows-first, Android-ready SSH research client (Rust core + C++/Qt Quick UI)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for single-config generators (keeps the CRT consistent with
# the Rust core, which links the dynamic release CRT on MSVC).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- Qt -------------------------------------------------------------------
# Requires Qt 6.5+. Point CMake at your Qt install with either:
#   -DCMAKE_PREFIX_PATH=C:/Qt/6.7.2/msvc2019_64
# or by setting the CMAKE_PREFIX_PATH environment variable.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 QuickDialogs2)

qt_standard_project_setup(REQUIRES 6.5)

# Auto-generate qmldir files for QML subdirectories (e.g. app/qml/components),
# so the module's types resolve cleanly across directories.
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# --- Rust core ------------------------------------------------------------
include(cmake/RustCore.cmake)

# --- Application ----------------------------------------------------------
set_source_files_properties(app/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(researchssh_next
    app/cpp/main.cpp
    app/cpp/AppController.cpp
    app/cpp/AppController.h
    app/cpp/RustCoreBridge.cpp
    app/cpp/RustCoreBridge.h
    app/cpp/ServerListModel.cpp
    app/cpp/ServerListModel.h
    app/cpp/TerminalViewModel.cpp
    app/cpp/TerminalViewModel.h
    app/cpp/CodeHighlighter.cpp
    app/cpp/CodeHighlighter.h
    app/cpp/RemoteFileTreeModel.cpp
    app/cpp/RemoteFileTreeModel.h
    app/cpp/EditorViewModel.cpp
    app/cpp/EditorViewModel.h
    app/cpp/CredentialStore.cpp
    app/cpp/CredentialStore.h)

if(WIN32)
    target_sources(researchssh_next PRIVATE app/windows/app.rc)
    set_target_properties(researchssh_next PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

qt_add_qml_module(researchssh_next
    URI ResearchSSH
    VERSION 1.0
    QML_FILES
        app/qml/Theme.qml
        app/qml/Main.qml
        app/qml/components/PanelHeader.qml
        app/qml/components/StatusDot.qml
        app/qml/components/StyledButton.qml
        app/qml/components/ServerPane.qml
        app/qml/components/WorkspacePane.qml
        app/qml/components/TerminalPane.qml
        app/qml/components/CodeEditorPane.qml
        app/qml/components/StatusPane.qml
        app/qml/components/PackagePane.qml
        app/qml/components/RemoteFilesPane.qml
        app/qml/components/ConnectDialog.qml
        app/qml/components/HostKeyDialog.qml)

qt_add_resources(researchssh_next app_assets
    PREFIX "/"
    FILES
        app/assets/researchssh-next.png)

target_compile_definitions(researchssh_next PRIVATE
    RESEARCHSSH_APP_VERSION="${PROJECT_VERSION}"
    RESEARCHSSH_UPDATE_REPO="wweiyi2004/ResearchSSH-Next")

target_include_directories(researchssh_next PRIVATE app/cpp)

target_link_libraries(researchssh_next PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    researchssh_core)

if(WIN32)
    target_link_libraries(researchssh_next PRIVATE Advapi32)
endif()

# --- Tests ----------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
