/*
 * author: alcubierre-drive
 * license: gpl-v3
 */

#ifndef DEFAULT_CONFIG_FILE
#define DEFAULT_CONFIG_FILE "/etc/btrfs-snap.conf"
#endif

#include "snap.hpp"
#include "config.hpp"
#include "nokill.hpp"
#include <unistd.h>

void print_help( string progname ) {
    using std::cout;
    using std::endl;
    cout << "usage: " << progname << " <options> to back up btrfs file systems" << endl
         << "options: -h              show this help" << endl
         << "         -R <dir>        set remote snapshot dir" << endl
         << "         -S <dir>        set local snapshot dir" << endl
         << "         -r <num>        set remote snapshot num to keep" << endl
         << "         -s <num>        set local snapshot num to keep " << endl
         << "         -b <dir>        set backup dir to mirror" << endl
         << "         -B <name>       set short name of backup" << endl
         << "         -p <name>       set pre-saved mode <name>" << endl
         << "         -H <name>       set hostname (name prefix)" << endl
         << "         -T              skip transfer" << endl
         << "         -P <cmd>        execute <cmd> before snapshotting" << endl
         << "         -x <cmd>        execute <cmd> after snapshooting" << endl
         << "         -C              do not create a snapshot" << endl
         << "         -d              dry run (only print commands)" << endl
         << "         -c <file>       run from config file" << endl;
}

int main( int argc, char** argv ) {
    int opt;
    string setup_variables = "";
    string config_file = DEFAULT_CONFIG_FILE;
    while ((opt = getopt(argc, argv, ":hdR:S:r:s:b:B:p:H:TP:Cc:x:")) != -1) {
        switch (opt) {
            case 'h':
                print_help( argv[0] );
                return EXIT_SUCCESS;
            case 'R':
                snapshot_setup::remote_snapshot_dir = string(optarg);
                break;
            case 'S':
                snapshot_setup::snapshot_dir = string(optarg);
                break;
            case 'r':
                snapshot_setup::keep_remote_snapshots_num = atoi(optarg);
                break;
            case 's':
                snapshot_setup::keep_snapshots_num = atoi(optarg);
                break;
            case 'b':
                snapshot_setup::backup_dir = string(optarg);
                break;
            case 'B':
                snapshot_setup::backup_name = string(optarg);
                break;
            case 'p':
                setup_variables = string(optarg);
                break;
            case 'H':
                snapshot_setup::host_name = string(optarg);
                break;
            case 'd':
                snapshot_setup::dry_run = true;
                break;
            case 'T':
                snapshot_setup::transfer = false;
                break;
            case 'P':
                snapshot_setup::pre_command = string(optarg);
                break;
            case 'x':
                snapshot_setup::post_command = string(optarg);
                break;
            case 'C':
                snapshot_setup::create = false;
                break;
            case 'c':
                config_file = string(optarg);
                break;
            case '?':
                WARN( "unknown option '-" << (char)optopt << "'. show help with -h" );
                break;
            default:
                break;
        }
    }
    nokill_init();

    if (config_file != "") {
        vector<vector<pair<string,string>>> config;
        vector<string> sections;
        if (parse_config( config_file, config, sections ))
            return EXIT_FAILURE;
        if (sections.size() > 0) {
            CFG( "config file mode." );
            for (unsigned i=0; i<sections.size(); ++i) {
                CFG( "[" << sections[i] << "]" );
                set_params( config[i] );
                if (snap_and_transfer())
                    return EXIT_FAILURE;
            }
            if (!snapshot_setup::do_sync)
                if (snap_finalize_sync())
                    return EXIT_FAILURE;
            return EXIT_SUCCESS;
        }
    }

    if (setup_variables != "")
        if (setup_variables_saved( setup_variables ))
            return EXIT_FAILURE;

    if (snap_and_transfer())
        return EXIT_FAILURE;

    if (!snapshot_setup::do_sync)
        if (snap_finalize_sync())
            return EXIT_FAILURE;

    nokill_clear();
    return EXIT_SUCCESS;
}
