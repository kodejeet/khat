cmake_minimum_required(VERSION 3.21)

project(GmailNotifier VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Sql Widgets DBus)

qt_standard_project_setup()

add_library(gmailnotifier_core STATIC
    src/config/AppConfig.cpp
    src/config/AppConfig.h
    src/config/AppPaths.cpp
    src/config/AppPaths.h
    src/config/ConfigStore.cpp
    src/config/ConfigStore.h
    src/config/JsonFile.cpp
    src/config/JsonFile.h
    src/config/OAuthClientConfig.cpp
    src/config/OAuthClientConfig.h
    src/config/OAuthClientStore.cpp
    src/config/OAuthClientStore.h
    src/gmail/GmailClient.cpp
    src/gmail/GmailClient.h
    src/gmail/GmailError.h
    src/gmail/GmailQuery.cpp
    src/gmail/GmailQuery.h
    src/model/MessageSummary.h
    src/state/StateDatabase.cpp
    src/state/StateDatabase.h
    src/sync/SyncService.cpp
    src/sync/SyncService.h
)

target_include_directories(gmailnotifier_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gmailnotifier_core PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

qt_add_executable(gmail-notifier
    src/main.cpp
)

target_compile_definitions(gmail-notifier PRIVATE
    GMAIL_NOTIFIER_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(gmail-notifier PRIVATE
    gmailnotifier_core
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
