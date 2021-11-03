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
        static bool transfer;
        static bool create;
};

int snap_and_transfer();
int setup_variables_saved( string name );

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

#define WARN( str )  std::cerr << COLOR( "warning: ", bright_yellow ) << str << std::endl
#define ERR( str )   std::cerr << COLOR( "error: ",   bright_red    ) << str << std::endl
#define INFO( str )  std::cerr << COLOR( "info: ",    bright_blue   ) << str << std::endl
#define SHELL( str ) std::cerr << COLOR( "shell: ",   bright_green  ) << str << std::endl

