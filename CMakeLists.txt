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

    # Protobuf/nanopb generation and src/studio/watchdog_request_exec.c (the
    # transport-independent GetStatus/ListIncidents/DeleteIncidents executor
    # against the local store) are needed by BOTH the local Studio RPC
    # handler (CONFIG_ZMK_WATCHDOG_STUDIO_RPC, central/non-split only) and
    # the split relay peripheral responder (CONFIG_ZMK_WATCHDOG_SPLIT_RELAY,
    # which a split peripheral -- with no Studio of its own -- also needs),
    # so this is gated on either being enabled, not just the RPC one.
    if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC OR CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
        target_sources(app PRIVATE src/studio/watchdog_request_exec.c)

        if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC)
            target_sources(app PRIVATE src/studio/watchdog_handler.c)

            if(CONFIG_ZMK_WATCHDOG_STUDIO_RPC_TEST)
                target_sources(app PRIVATE src/test/watchdog_rpc_test.c)
            endif()
        endif()

        if(CONFIG_ZMK_WATCHDOG_SPLIT_RELAY)
            target_sources(app PRIVATE src/split/watchdog_relay.c)
        endif()

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
