cmake_minimum_required(VERSION 3.25)
# Keep compiler debug information in project object files; no shared PDB server.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
project(NativeDownloader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Qt-free task command/snapshot/persistence core used by the Win32 product.
add_library(litefetch-core STATIC
    src/core/task.cpp src/core/task.h
    src/core/task_identity.cpp src/core/task_identity.h
    src/core/task_store.cpp src/core/task_store.h
    src/core/text_utf8.cpp src/core/text_utf8.h
    src/core/http_engine.cpp src/core/http_engine.h
    src/core/segmented_transfer.cpp src/core/segmented_transfer.h
    src/core/ed2k_link.cpp src/core/ed2k_link.h
    src/core/ed2k_client.cpp src/core/ed2k_client.h
    src/core/ed2k_transfer.cpp src/core/ed2k_transfer.h
    src/core/ed2k_task.cpp src/core/ed2k_task.h
    src/core/ed2k_serve.cpp src/core/ed2k_serve.h
    src/core/kad_id.cpp src/core/kad_id.h
    src/core/kad_routing.cpp src/core/kad_routing.h
    src/core/kad_wire.cpp src/core/kad_wire.h
    src/core/kad_node.cpp src/core/kad_node.h
    src/core/rate_limit.cpp src/core/rate_limit.h
    src/core/settings.cpp src/core/settings.h
    src/core/low_usage.cpp src/core/low_usage.h
    src/core/traffic_meter.cpp src/core/traffic_meter.h
    src/core/schedule.cpp src/core/schedule.h
    src/core/native_frame.cpp src/core/native_frame.h
    src/core/native_host_loop.cpp src/core/native_host_loop.h
    src/core/pipe_channel.cpp src/core/pipe_channel.h
    src/core/credentials.cpp src/core/credentials.h
    src/win32/native_bridge.cpp src/win32/native_bridge.h
    src/win32/options_dialog.cpp src/win32/options_dialog.h
    src/win32/notification_policy.cpp src/win32/notification_policy.h
    src/win32/traffic_text.cpp src/win32/traffic_text.h
    src/core/task_service.cpp src/core/task_service.h)
target_include_directories(litefetch-core PUBLIC src/core)
target_link_libraries(litefetch-core PUBLIC CURL::libcurl PRIVATE unofficial::sqlite3::sqlite3 crypt32)

# BitTorrent lives in its own library so binaries that never touch torrents do
# not link libtorrent at all.
find_package(LibtorrentRasterbar CONFIG REQUIRED)
add_library(litefetch-bt STATIC src/core/bt_engine.cpp src/core/bt_engine.h)
target_include_directories(litefetch-bt PUBLIC src/core)
target_link_libraries(litefetch-bt PUBLIC litefetch-core LibtorrentRasterbar::torrent-rasterbar)
if(WIN32)
    target_link_libraries(litefetch-core PRIVATE bcrypt ws2_32)
endif()
add_executable(litefetch-core-cli src/core/cli/main.cpp)
target_link_libraries(litefetch-core-cli PRIVATE litefetch-core)

# Win32 UI model: formatting, list rows, action enablement and source parsing.
# Kept free of window code so it is testable without a desktop.
add_library(litefetch-ui-model STATIC
    src/win32/ui_text.cpp src/win32/ui_text.h
    src/win32/url_filename.cpp src/win32/url_filename.h
    src/win32/list_layout.cpp src/win32/list_layout.h
    src/win32/columns.cpp src/win32/columns.h
    src/win32/task_view.cpp src/win32/task_view.h
    src/win32/batch_input.cpp src/win32/batch_input.h
    src/win32/startup.cpp src/win32/startup.h
    src/win32/explorer_target.cpp src/win32/explorer_target.h
    src/win32/task_list_model.cpp src/win32/task_list_model.h)
target_include_directories(litefetch-ui-model PUBLIC src/win32)
target_link_libraries(litefetch-ui-model PUBLIC litefetch-core)
if(MSVC)
    target_compile_options(litefetch-ui-model PRIVATE /W4 /permissive- /utf-8)
endif()

# Worker-thread boundary between the window and the core. It is a separate
# library so the thread/cancel/shutdown behavior can be tested without a UI.
add_library(litefetch-win32-runtime STATIC src/win32/task_worker.cpp src/win32/task_worker.h)
target_include_directories(litefetch-win32-runtime PUBLIC src/win32)
target_link_libraries(litefetch-win32-runtime PUBLIC litefetch-core PRIVATE user32)
if(MSVC)
    target_compile_options(litefetch-win32-runtime PRIVATE /W4 /permissive- /utf-8)
endif()
if(MSVC)
    target_compile_options(litefetch-core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(litefetch-core-cli PRIVATE /W4 /permissive- /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(core-identity-tests tests/core/identity_test.cpp)
    target_link_libraries(core-identity-tests PRIVATE litefetch-core)
    add_test(NAME core-identity COMMAND core-identity-tests)
    set_tests_properties(core-identity PROPERTIES TIMEOUT 15)
    add_executable(core-store-tests tests/core/store_test.cpp)
    target_link_libraries(core-store-tests PRIVATE litefetch-core unofficial::sqlite3::sqlite3)
    add_test(NAME core-store COMMAND core-store-tests)
    set_tests_properties(core-store PROPERTIES TIMEOUT 20)
    add_executable(core-service-tests tests/core/service_test.cpp)
    target_link_libraries(core-service-tests PRIVATE litefetch-core ws2_32)
    add_test(NAME core-service COMMAND core-service-tests)
    set_tests_properties(core-service PROPERTIES TIMEOUT 240)
    add_executable(core-storage-failure-tests tests/core/storage_failure_test.cpp)
    target_link_libraries(core-storage-failure-tests PRIVATE litefetch-core unofficial::sqlite3::sqlite3)
    add_test(NAME core-storage-failure COMMAND core-storage-failure-tests)
    set_tests_properties(core-storage-failure PROPERTIES TIMEOUT 20)
    add_executable(core-unicode-path-tests tests/core/unicode_path_test.cpp)
    target_link_libraries(core-unicode-path-tests PRIVATE litefetch-core ws2_32)
    add_test(NAME core-unicode-path COMMAND core-unicode-path-tests)
    set_tests_properties(core-unicode-path PROPERTIES TIMEOUT 30)
    add_executable(ui-model-tests tests/win32/model_test.cpp)
    target_link_libraries(ui-model-tests PRIVATE litefetch-ui-model)
    add_test(NAME ui-model COMMAND ui-model-tests)
    set_tests_properties(ui-model PROPERTIES TIMEOUT 20)
    add_executable(ui-columns-tests tests/win32/columns_test.cpp)
    target_include_directories(ui-columns-tests PRIVATE tests/core)
    target_link_libraries(ui-columns-tests PRIVATE litefetch-ui-model)
    add_test(NAME ui-columns COMMAND ui-columns-tests)
    set_tests_properties(ui-columns PROPERTIES TIMEOUT 20)
    add_executable(ui-view-tests tests/win32/view_test.cpp)
    target_include_directories(ui-view-tests PRIVATE tests/core)
    target_link_libraries(ui-view-tests PRIVATE litefetch-ui-model)
    add_test(NAME ui-view COMMAND ui-view-tests)
    set_tests_properties(ui-view PROPERTIES TIMEOUT 20)
    add_executable(ui-batch-tests tests/win32/batch_test.cpp)
    target_include_directories(ui-batch-tests PRIVATE tests/core)
    target_link_libraries(ui-batch-tests PRIVATE litefetch-ui-model)
    add_test(NAME ui-batch COMMAND ui-batch-tests)
    set_tests_properties(ui-batch PROPERTIES TIMEOUT 20)
    # BitTorrent boundary test: builds a local swarm and downloads one file.
    add_executable(win32-native-bridge-tests tests/win32/native_bridge_test.cpp)
    target_include_directories(win32-native-bridge-tests PRIVATE tests/core src/win32)
    target_link_libraries(win32-native-bridge-tests PRIVATE litefetch-core ws2_32)
    add_test(NAME win32-native-bridge COMMAND win32-native-bridge-tests)
    set_tests_properties(win32-native-bridge PROPERTIES TIMEOUT 60)
    add_executable(core-credentials-tests tests/core/credentials_test.cpp)
    target_include_directories(core-credentials-tests PRIVATE tests/core)
    target_link_libraries(core-credentials-tests PRIVATE litefetch-core crypt32)
    add_test(NAME core-credentials COMMAND core-credentials-tests)
    set_tests_properties(core-credentials PROPERTIES TIMEOUT 60)

    add_executable(core-native-host-tests tests/core/native_host_test.cpp)
    target_include_directories(core-native-host-tests PRIVATE tests/core)
    target_link_libraries(core-native-host-tests PRIVATE litefetch-core)
    add_test(NAME core-native-host COMMAND core-native-host-tests)
    set_tests_properties(core-native-host PROPERTIES TIMEOUT 60)

    add_executable(core-native-frame-tests tests/core/native_frame_test.cpp)
    target_include_directories(core-native-frame-tests PRIVATE tests/core)
    target_link_libraries(core-native-frame-tests PRIVATE litefetch-core)
    add_test(NAME core-native-frame COMMAND core-native-frame-tests)
    set_tests_properties(core-native-frame PROPERTIES TIMEOUT 60)

    add_executable(core-schedule-tests tests/core/schedule_test.cpp)
    target_include_directories(core-schedule-tests PRIVATE tests/core)
    target_link_libraries(core-schedule-tests PRIVATE litefetch-core)
    add_test(NAME core-schedule COMMAND core-schedule-tests)
    set_tests_properties(core-schedule PROPERTIES TIMEOUT 60)

    add_executable(core-traffic-tests tests/core/traffic_test.cpp)
    target_include_directories(core-traffic-tests PRIVATE tests/core)
    target_link_libraries(core-traffic-tests PRIVATE litefetch-core)
    add_test(NAME core-traffic COMMAND core-traffic-tests)
    set_tests_properties(core-traffic PROPERTIES TIMEOUT 60)

    add_executable(core-low-usage-tests tests/core/low_usage_test.cpp)
    target_include_directories(core-low-usage-tests PRIVATE tests/core)
    target_link_libraries(core-low-usage-tests PRIVATE litefetch-core)
    add_test(NAME core-low-usage COMMAND core-low-usage-tests)
    set_tests_properties(core-low-usage PROPERTIES TIMEOUT 60)

    add_executable(core-rate-limit-tests tests/core/rate_limit_test.cpp)
    target_include_directories(core-rate-limit-tests PRIVATE tests/core)
    target_link_libraries(core-rate-limit-tests PRIVATE litefetch-core)
    add_test(NAME core-rate-limit COMMAND core-rate-limit-tests)
    set_tests_properties(core-rate-limit PROPERTIES TIMEOUT 60)

    add_executable(core-kad-tests tests/core/kad_test.cpp)
    target_include_directories(core-kad-tests PRIVATE tests/core)
    target_link_libraries(core-kad-tests PRIVATE litefetch-core ws2_32)
    add_test(NAME core-kad COMMAND core-kad-tests)
    set_tests_properties(core-kad PROPERTIES TIMEOUT 60)

    add_executable(litefetch-ed2khash src/tools/ed2khash.cpp)
    target_link_libraries(litefetch-ed2khash PRIVATE litefetch-core bcrypt)
    add_executable(litefetch-ed2kseed src/tools/ed2kseed.cpp)
    target_link_libraries(litefetch-ed2kseed PRIVATE litefetch-core bcrypt ws2_32)

    add_executable(litefetch-ed2kget src/tools/ed2kget.cpp)
    target_link_libraries(litefetch-ed2kget PRIVATE litefetch-core bcrypt ws2_32)

    add_executable(core-ed2k-tests tests/core/ed2k_test.cpp)
    target_include_directories(core-ed2k-tests PRIVATE tests/core)
    target_link_libraries(core-ed2k-tests PRIVATE litefetch-core ws2_32)
    add_test(NAME core-ed2k COMMAND core-ed2k-tests)
    set_tests_properties(core-ed2k PROPERTIES TIMEOUT 120)
    add_executable(core-bt-tests tests/core/bt_test.cpp)
    target_include_directories(core-bt-tests PRIVATE tests/core)
    target_link_libraries(core-bt-tests PRIVATE litefetch-bt ws2_32)
    add_test(NAME core-bt COMMAND core-bt-tests)
    set_tests_properties(core-bt PROPERTIES TIMEOUT 120)
    add_executable(ui-worker-tests tests/win32/task_worker_test.cpp)
    target_include_directories(ui-worker-tests PRIVATE tests/core)
    target_link_libraries(ui-worker-tests PRIVATE litefetch-win32-runtime ws2_32)
    add_test(NAME ui-worker COMMAND ui-worker-tests)
    set_tests_properties(ui-worker PROPERTIES TIMEOUT 90)
    set(core_test_temp "${CMAKE_SOURCE_DIR}/tmp/core-test-temp")
    set(core_test_targets core-identity core-store core-service core-storage-failure core-unicode-path
        ui-model ui-columns ui-view ui-batch ui-worker)
    set_tests_properties(${core_test_targets} PROPERTIES
        ENVIRONMENT "LITEFETCH_TEST_TEMP=${core_test_temp};TEMP=${core_test_temp};TMP=${core_test_temp}")
    if(MSVC)
        foreach(core_test core-identity-tests core-store-tests core-service-tests
                core-storage-failure-tests core-unicode-path-tests ui-model-tests
                ui-columns-tests ui-view-tests ui-batch-tests ui-worker-tests)
            target_compile_options(${core_test} PRIVATE /W4 /permissive- /utf-8)
        endforeach()
    endif()
endif()

# Win32 product window. It uses only Win32/Comctl32 resources (menu, toolbar,
# ListView, status bar) and the Qt-free core; system chrome is drawn by Windows.
#
# The window lives in a library so an in-process test can create the real
# window and drive it without launching a second process: cross-process message
# scripts race against dialogs and timers, while a same-thread harness calls the
# same handlers synchronously.
if(WIN32)
    enable_language(RC)
    add_library(litefetch-win32-ui STATIC
        src/win32/main_window.cpp src/win32/main_window.h
        src/win32/new_download_dialog.cpp src/win32/new_download_dialog.h
        src/win32/column_dialog.cpp src/win32/column_dialog.h
        src/win32/batch_dialog.cpp src/win32/batch_dialog.h)
    target_compile_definitions(litefetch-win32-ui PUBLIC UNICODE _UNICODE)
    target_link_libraries(litefetch-win32-ui PUBLIC litefetch-ui-model litefetch-win32-runtime
        PRIVATE litefetch-bt comctl32 shell32 ole32 shlwapi)
    if(MSVC)
        target_compile_options(litefetch-win32-ui PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_executable(litefetch-win32 WIN32
        src/win32/main.cpp
        src/win32/app.manifest
        src/win32/win32.rc)
    target_link_libraries(litefetch-win32 PRIVATE litefetch-win32-ui litefetch-bt
        comctl32 shell32 ole32 shlwapi)
    if(MSVC)
        target_compile_options(litefetch-win32 PRIVATE /W4 /permissive- /utf-8)
    endif()
    if(BUILD_TESTING)
        # In-process window harness: creates the real main window on the test
        # thread and sends the same WM_COMMANDs a user would, so checks do not
        # race a second process, its dialogs or its timers.
        add_executable(ui-window-tests tests/win32/window_test.cpp src/win32/win32.rc)
        target_include_directories(ui-window-tests PRIVATE tests/core)
        target_link_libraries(ui-window-tests PRIVATE litefetch-win32-ui uiautomationcore oleaut32 gdiplus)
        add_test(NAME ui-window COMMAND ui-window-tests)
        set_tests_properties(ui-window PROPERTIES TIMEOUT 90
            ENVIRONMENT "LITEFETCH_TEST_TEMP=${CMAKE_SOURCE_DIR}/tmp/core-test-temp;LITEFETCH_EVIDENCE_DIR=${CMAKE_SOURCE_DIR}/.pi/evidence/ui-window")
        if(MSVC)
            target_compile_options(ui-window-tests PRIVATE /W4 /permissive- /utf-8)
        endif()
    endif()
endif()