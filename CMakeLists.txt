cmake_minimum_required(VERSION 3.21)

project(khat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Sql Widgets DBus)

qt_standard_project_setup()

add_library(khat_core STATIC
    src/app/AppController.cpp
    src/app/AppController.h
    src/app/PowerMonitor.cpp
    src/app/PowerMonitor.h
    src/app/SingleInstance.cpp
    src/app/SingleInstance.h
    src/app/UnixSignalHandler.cpp
    src/app/UnixSignalHandler.h
    src/auth/AccessTokenProvider.h
    src/auth/OAuthLoopbackServer.cpp
    src/auth/OAuthLoopbackServer.h
    src/auth/OAuthManager.cpp
    src/auth/OAuthManager.h
    src/auth/OAuthPkce.cpp
    src/auth/OAuthPkce.h
    src/auth/OAuthToken.cpp
    src/auth/OAuthToken.h
    src/auth/OAuthTokenClient.cpp
    src/auth/OAuthTokenClient.h
    src/auth/OAuthTokenStore.h
    src/auth/SecretServiceTokenStore.cpp
    src/auth/SecretServiceTokenStore.h
    src/auth/SecretServiceTypes.cpp
    src/auth/SecretServiceTypes.h
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
    src/connectivity/ConnectivityMonitor.cpp
    src/connectivity/ConnectivityMonitor.h
    src/gmail/GmailClient.cpp
    src/gmail/GmailClient.h
    src/gmail/GmailError.h
    src/gmail/GmailQuery.cpp
    src/gmail/GmailQuery.h
    src/model/MessageSummary.h
    src/notify/FreedesktopNotificationBackend.cpp
    src/notify/FreedesktopNotificationBackend.h
    src/notify/NotificationBackend.h
    src/notify/NotificationContent.cpp
    src/notify/NotificationContent.h
    src/notify/NotificationDispatcher.cpp
    src/notify/NotificationDispatcher.h
    src/notify/NotificationService.cpp
    src/notify/NotificationService.h
    src/state/StateDatabase.cpp
    src/state/StateDatabase.h
    src/sync/BackoffPolicy.cpp
    src/sync/BackoffPolicy.h
    src/sync/SyncScheduler.cpp
    src/sync/SyncScheduler.h
    src/sync/SyncService.cpp
    src/sync/SyncService.h
    src/ui/SettingsWindow.cpp
    src/ui/SettingsWindow.h
)

target_include_directories(khat_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(khat_core PUBLIC
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
)

qt_add_executable(khat
    src/main.cpp
)

qt_add_resources(khat "application-icons"
    PREFIX "/icons"
    BASE "assets/icons"
    FILES
        assets/icons/khat-16.png
        assets/icons/khat-24.png
        assets/icons/khat-32.png
        assets/icons/khat-48.png
        assets/icons/khat-64.png
        assets/icons/khat-128.png
        assets/icons/khat-256.png
        assets/icons/khat-512.png
)

target_compile_definitions(khat PRIVATE
    KHAT_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(khat PRIVATE
    khat_core
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Sql
    Qt6::Widgets
    Qt6::DBus
)

install(TARGETS khat
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(KHAT_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/data)
configure_file(
    data/khat.desktop.in
    ${KHAT_DATA_DIR}/khat.desktop
    @ONLY
)
configure_file(
    data/khat.service.in
    ${KHAT_DATA_DIR}/khat.service
    @ONLY
)

install(FILES ${KHAT_DATA_DIR}/khat.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES ${KHAT_DATA_DIR}/khat.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user
)

set(KHAT_ICON_SIZES 16 22 24 32 48 64 128 256 512)
foreach(icon_size IN LISTS KHAT_ICON_SIZES)
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/khat-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
        RENAME khat.png
    )
endforeach()

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
