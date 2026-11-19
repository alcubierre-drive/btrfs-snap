#pragma once

#ifndef DEFAULT_CONFIG_FILE
#define DEFAULT_CONFIG_FILE "/etc/btrfs-snap-cpp.conf"
#endif

#include <string>
#include <vector>
#include <utility>

using std::string;
using std::vector;
using std::pair;

int parse_config( string fname, vector<vector<pair<string,string>>>& config,
        vector<string>& sections );
// DEFAULT_CONFIG_FILE, or the pre-rename /etc/btrfs-snap.conf if only that exists
string default_config_file();
void set_params( const vector<pair<string,string>>& params );
