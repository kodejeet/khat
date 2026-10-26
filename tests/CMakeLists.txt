function(add_gmailnotifier_test name source)
    qt_add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE gmailnotifier_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endfunction()

add_gmailnotifier_test(test-config test_config.cpp)
add_gmailnotifier_test(test-state test_state.cpp)
add_gmailnotifier_test(test-gmail test_gmail.cpp)
add_gmailnotifier_test(test-sync test_sync.cpp)
add_gmailnotifier_test(test-backoff test_backoff.cpp)
add_gmailnotifier_test(test-scheduler test_scheduler.cpp)
add_gmailnotifier_test(test-connectivity test_connectivity.cpp)
add_gmailnotifier_test(test-notifications test_notifications.cpp)
add_gmailnotifier_test(test-settings-window test_settings_window.cpp)

find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    qt_add_executable(test-freedesktop-notification test_freedesktop_notification.cpp)
    target_link_libraries(test-freedesktop-notification PRIVATE gmailnotifier_core Qt6::Test)
    add_test(NAME test-freedesktop-notification
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test-freedesktop-notification>)
    set_tests_properties(test-freedesktop-notification PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GMAIL_NOTIFIER_TEST_PRIVATE_BUS=1"
    )
else()
    message(STATUS "dbus-run-session not found; skipping test-freedesktop-notification")
endif()
