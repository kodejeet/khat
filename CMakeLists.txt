cmake_minimum_required(VERSION 3.21)

project(GmailNotifier VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Sql Widgets DBus)

qt_standard_project_setup()

qt_add_executable(gmail-notifier
    src/main.cpp
)

target_compile_definitions(gmail-notifier PRIVATE
    GMAIL_NOTIFIER_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(gmail-notifier PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    Qt6::DBus
)

install(TARGETS gmail-notifier
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
