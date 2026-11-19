#pragma once

#include <string>
#include <iostream>

using std::string;

class snapshot_setup {
    public:
        static string host_name;
        static string backup_name;
        static string backup_dir;
        static string snapshot_dir;
        static string remote_snapshot_dir;
        static unsigned keep_remote_snapshots_num;
        static unsigned keep_snapshots_num;
        static bool dry_run;
        static string pre_command;
        static string post_command;
        static bool transfer;
        static bool create;
        static bool do_sync;
        // fail instead of falling back to local operation when the remote
        // snapshot dir is missing (set by -g)
        static bool require_remote;
        // per-section only (not inherited by following sections)
        static string watch_group;
        static string watch_devices;
};

// copy of all snapshot_setup values, to evaluate a config file without
// disturbing the current settings
struct snapshot_setup_state {
    string host_name, backup_name, backup_dir, snapshot_dir, remote_snapshot_dir;
    unsigned keep_remote_snapshots_num, keep_snapshots_num;
    bool dry_run;
    string pre_command, post_command;
    bool transfer, create, do_sync, require_remote;
    string watch_group, watch_devices;
};
snapshot_setup_state snapshot_setup_save();
void snapshot_setup_restore( const snapshot_setup_state& s );


int snap_and_transfer();
int setup_variables_saved( string name );
int snap_finalize_sync();

static const char _colors_black[] = "\u001b[30m";
static const char _colors_red[] = "\u001b[31m";
static const char _colors_green[] = "\u001b[32m";
static const char _colors_yellow[] = "\u001b[33m";
static const char _colors_blue[] = "\u001b[34m";
static const char _colors_magenta[] = "\u001b[35m";
static const char _colors_cyan[] = "\u001b[36m";
static const char _colors_white[] = "\u001b[37m";
static const char _colors_bright_black[] = "\u001b[30;1m";
static const char _colors_bright_red[] = "\u001b[31;1m";
static const char _colors_bright_green[] = "\u001b[32;1m";
static const char _colors_bright_yellow[] = "\u001b[33;1m";
static const char _colors_bright_blue[] = "\u001b[34;1m";
static const char _colors_bright_magenta[] = "\u001b[35;1m";
static const char _colors_bright_cyan[] = "\u001b[36;1m";
static const char _colors_bright_white[] = "\u001b[37;1m";
static const char _colors_reset[] = "\u001b[0m";
#define COLOR( str, name ) _colors_##name << str << _colors_reset

#define WARN( str )  std::cerr << COLOR( "warning: ", bright_yellow  ) << str << std::endl
#define ERR( str )   std::cerr << COLOR( "error: ",   bright_red     ) << str << std::endl
#define INFO( str )  std::cerr << COLOR( "info: ",    bright_blue    ) << str << std::endl
#define SHELL( str ) std::cerr << COLOR( "shell: ",   bright_green   ) << str << std::endl
#define CFG( str )   std::cerr << COLOR( "cfg: ",     bright_magenta ) << str << std::endl

