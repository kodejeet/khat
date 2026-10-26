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
