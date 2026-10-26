function(add_khat_test name source)
    qt_add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE khat_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endfunction()

add_khat_test(test-config test_config.cpp)
add_khat_test(test-state test_state.cpp)
add_khat_test(test-gmail test_gmail.cpp)
add_khat_test(test-sync test_sync.cpp)
add_khat_test(test-backoff test_backoff.cpp)
add_khat_test(test-scheduler test_scheduler.cpp)
add_khat_test(test-connectivity test_connectivity.cpp)
add_khat_test(test-notifications test_notifications.cpp)
add_khat_test(test-oauth test_oauth.cpp)
add_khat_test(test-oauth-manager test_oauth_manager.cpp)
add_khat_test(test-settings-window test_settings_window.cpp)
add_khat_test(test-app-controller test_app_controller.cpp)
add_khat_test(test-single-instance test_single_instance.cpp)
add_khat_test(test-unix-signal test_unix_signal.cpp)
add_khat_test(test-icon-assets test_icon_assets.cpp)
target_compile_definitions(test-icon-assets PRIVATE
    KHAT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    qt_add_executable(test-freedesktop-notification test_freedesktop_notification.cpp)
    target_link_libraries(test-freedesktop-notification PRIVATE khat_core Qt6::Test)
    add_test(NAME test-freedesktop-notification
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test-freedesktop-notification>)
    set_tests_properties(test-freedesktop-notification PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KHAT_TEST_PRIVATE_BUS=1"
    )

    qt_add_executable(test-power-monitor test_power_monitor.cpp)
    target_link_libraries(test-power-monitor PRIVATE khat_core Qt6::Test)
    add_test(NAME test-power-monitor
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test-power-monitor>)
    set_tests_properties(test-power-monitor PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KHAT_TEST_PRIVATE_BUS=1"
    )

    qt_add_executable(test-secret-service test_secret_service.cpp)
    target_link_libraries(test-secret-service PRIVATE khat_core Qt6::Test)
    add_test(NAME test-secret-service
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test-secret-service>)
    set_tests_properties(test-secret-service PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KHAT_TEST_PRIVATE_BUS=1"
    )
else()
    message(STATUS "dbus-run-session not found; skipping D-Bus tests")
endif()
