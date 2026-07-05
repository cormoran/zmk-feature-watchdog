zephyr_include_directories(include)
if(CONFIG_ZMK_WATCHDOG)
    target_sources(app PRIVATE src/watchdog_store.c)
    target_sources(app PRIVATE src/watchdog_pending.c)
    target_sources(app PRIVATE src/watchdog_reset_cause.c)

    if(CONFIG_ZMK_WATCHDOG_FREEZE_DETECT)
        target_sources(app PRIVATE src/watchdog_freeze.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_FATAL_DETECT)
        target_sources(app PRIVATE src/watchdog_fatal.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_TEST_INJECTION)
        target_sources(app PRIVATE src/watchdog_inject.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_TEST)
        target_sources(app PRIVATE src/test/watchdog_test.c)
    endif()

    if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC)
        file(GLOB_RECURSE C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/*.c)
        target_sources(app PRIVATE ${C_FILES})

        list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
        include(nanopb)
        set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
        set(NANOPB_GENERATE_CPP_STANDALONE OFF)

        # NOTE: adding to app target directly causes build issues, so we create a library instead
        zephyr_library()
        file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

        nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
        target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
        target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

        # app should depend on the generated proto files
        target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
        add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
    endif()
endif()
