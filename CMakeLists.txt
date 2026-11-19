cmake_minimum_required(VERSION 3.10)

project(
    btrfs-snap-cpp
    VERSION 0.10
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBMOUNT REQUIRED IMPORTED_TARGET mount)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_executable(btrfs-snap-cpp
    main.cpp snap.cpp config.cpp nokill.cpp watchgroups.cpp
    daemon.cpp agent.cpp client.cpp)
target_link_libraries(btrfs-snap-cpp PRIVATE PkgConfig::LIBMOUNT PkgConfig::LIBSYSTEMD Threads::Threads)
# /etc/btrfs-snap-cpp.conf for prefix /usr (GNUInstallDirs maps /usr -> /etc)
if(NOT DEFINED DEFAULT_CONFIG_FILE)
    set(DEFAULT_CONFIG_FILE "${CMAKE_INSTALL_FULL_SYSCONFDIR}/btrfs-snap-cpp.conf")
endif()
target_compile_definitions(btrfs-snap-cpp PRIVATE DEFAULT_CONFIG_FILE="${DEFAULT_CONFIG_FILE}")
install(TARGETS btrfs-snap-cpp DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(systemd/btrfs-snap-cpp.service.in btrfs-snap-cpp.service @ONLY)
configure_file(systemd/btrfs-snap-cpp-agent.service.in btrfs-snap-cpp-agent.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/btrfs-snap-cpp.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/btrfs-snap-cpp-agent.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
install(FILES dbus/org.btrfssnapcpp.Daemon1.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES polkit/org.btrfssnapcpp.policy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
# sample config, only if there is no config yet (never overwrite the real one)
get_filename_component(CONFIG_DIR "${DEFAULT_CONFIG_FILE}" DIRECTORY)
get_filename_component(CONFIG_NAME "${DEFAULT_CONFIG_FILE}" NAME)
install(CODE "
    if(EXISTS \"\$ENV{DESTDIR}${DEFAULT_CONFIG_FILE}\")
        message(STATUS \"Keeping existing config: \$ENV{DESTDIR}${DEFAULT_CONFIG_FILE}\")
    else()
        file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/btrfs-snap-cpp.conf\"
            DESTINATION \"${CONFIG_DIR}\" RENAME \"${CONFIG_NAME}\")
    endif()
")
