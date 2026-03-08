cmake_minimum_required(VERSION 3.20)

project(ppp_modern LANGUAGES C CXX)

option(PPP_BUILD_TOOLS "Build auxiliary PPP tooling" ON)
option(PPP_BUILD_TESTS "Build PPP unit tests" ON)
option(PPP_ENABLE_SQLITE "Enable SQLite-backed job repository support" ON)
option(PPP_ENABLE_SQLSERVER "Enable SQL Server-backed job repository support" OFF)
option(PPP_SQLITE_VENDOR "Vendor SQLite amalgamation (third_party/sqlite/sqlite3.c)" OFF)

add_library(ppp_core
    src/job.cpp
    src/job_processor.cpp
    src/job_repository.cpp
    src/job_serialization.cpp
    src/job_service.cpp
    src/processing_config.cpp
    src/processing_config_io.cpp
    src/scheduling_policy_io.cpp
    src/tiff.cpp
    src/geometry.cpp
    src/image.cpp
)

set(PPP_CORE_HAVE_SQLITE 0)
if(PPP_ENABLE_SQLITE)
    if(PPP_SQLITE_VENDOR)
        set(SQLITE_VENDOR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
        if(NOT EXISTS ${SQLITE_VENDOR_SRC})
            message(FATAL_ERROR "PPP_SQLITE_VENDOR=ON but ${SQLITE_VENDOR_SRC} not found. Place the amalgamation there.")
        endif()
        target_sources(ppp_core PRIVATE src/job_repository_sqlite.cpp ${SQLITE_VENDOR_SRC})
        target_include_directories(ppp_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
        if(MSVC)
            target_compile_definitions(ppp_core PRIVATE SQLITE_API=__declspec(dllexport))
        endif()
        set(PPP_CORE_HAVE_SQLITE 1)
    else()
        find_package(SQLite3 QUIET)
        if(SQLite3_FOUND)
            target_sources(ppp_core PRIVATE src/job_repository_sqlite.cpp)
            target_link_libraries(ppp_core PRIVATE SQLite::SQLite3)
            set(PPP_CORE_HAVE_SQLITE 1)
        else()
            message(WARNING "SQLite3 not found; SQLite repository support disabled")
        endif()
    endif()
endif()

set(PPP_CORE_HAVE_SQLSERVER 0)
if(PPP_ENABLE_SQLSERVER)
    find_package(ODBC QUIET)
    if(ODBC_FOUND)
        target_sources(ppp_core PRIVATE src/job_repository_sqlserver.cpp)
        target_link_libraries(ppp_core PRIVATE ODBC::ODBC)
        set(PPP_CORE_HAVE_SQLSERVER 1)
    else()
        message(WARNING "ODBC not found; SQL Server repository support disabled")
    endif()
endif()

target_include_directories(ppp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(ppp_core PUBLIC cxx_std_20)

target_compile_definitions(ppp_core
    PRIVATE
        PPP_CORE_VERSION="0.1.0"
    PUBLIC
        PPP_CORE_HAVE_SQLITE=${PPP_CORE_HAVE_SQLITE}
        PPP_CORE_HAVE_SQLSERVER=${PPP_CORE_HAVE_SQLSERVER}
)

if(PPP_BUILD_TOOLS)
    add_executable(ppp_jobctl tools/ppp_jobctl.cpp)
    target_link_libraries(ppp_jobctl PRIVATE ppp_core)
    target_compile_features(ppp_jobctl PRIVATE cxx_std_20)
    if(WIN32)
        add_executable(ppp_jobviewer WIN32 tools/ppp_jobviewer.cpp)
        target_link_libraries(ppp_jobviewer PRIVATE ppp_core)
        target_compile_features(ppp_jobviewer PRIVATE cxx_std_20)
        target_compile_definitions(ppp_jobviewer PRIVATE UNICODE _UNICODE)
    endif()
endif()

if(PPP_BUILD_TESTS)
    enable_testing()
    add_executable(ppp_core_tests
        tests/job_service_tests.cpp
    )
    target_link_libraries(ppp_core_tests PRIVATE ppp_core)
    target_compile_features(ppp_core_tests PRIVATE cxx_std_20)
    add_test(NAME ppp_core_tests COMMAND ppp_core_tests)
endif()

include(GNUInstallDirs)

install(TARGETS ppp_core
    EXPORT ppp_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(PPP_BUILD_TOOLS)
    install(TARGETS ppp_jobctl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ppp_coreTargets
    NAMESPACE ppp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ppp_core
)

add_library(ppp::core ALIAS ppp_core)
