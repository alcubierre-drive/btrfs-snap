#pragma once

#include <string>

// btrfs-snap-cpp --daemon: root system service (daemon.cpp)
int daemon_main( int argc, char** argv );
// btrfs-snap-cpp agent: user session notification agent (agent.cpp)
int agent_main( int argc, char** argv );
// btrfs-snap-cpp status|backup|mount|umount|stop|answer|reload (client.cpp)
bool is_client_command( const std::string& word );
int client_main( int argc, char** argv );
