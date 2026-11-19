/*
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include "snap.hpp"
#include "config.hpp"
#include "nokill.hpp"
#include "watchgroups.hpp"
#include "modes.hpp"
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
         << "         -c <file>       run from config file" << endl
         << "         -g <group>      only run config sections with 'watch = <group>'" << endl
         << "                         and fail if the remote snapshot dir is missing" << endl
         << endl
         << "daemon mode:" << endl
         << "  " << progname << " --daemon [-c <file>] [-f <fstab>] [-v]" << endl
         << "  " << progname << " agent [-t <terminal>] [-v]" << endl
         << "  " << progname << " status | backup [-w|-f] <group> | mount <group> | umount <group>" << endl
         << "  " << progname << " stop <group> | answer <id> <choice> | reload" << endl;
}

int main( int argc, char** argv ) {
    int opt;
    string setup_variables = "";
    if (argc > 1) {
        string mode = argv[1];
        if (mode == "--daemon" || mode == "daemon")
            return daemon_main( argc-1, argv+1 );
        if (mode == "agent")
            return agent_main( argc-1, argv+1 );
        if (is_client_command( mode ))
            return client_main( argc-1, argv+1 );
    }

    string config_file = default_config_file();
    string group = "";
    while ((opt = getopt(argc, argv, ":hdR:S:r:s:b:B:p:H:TP:Cc:x:g:")) != -1) {
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
            case 'g':
                group = string(optarg);
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
            if (group != "")
                snapshot_setup::require_remote = true;
            unsigned ran = 0;
            for (unsigned i=0; i<sections.size(); ++i) {
                string watch_name;
                if (watch_section_group( sections[i], watch_name ))
                    continue; // group configuration, not a backup job
                set_params( config[i] );
                if (group != "" && snapshot_setup::watch_group != group)
                    continue;
                CFG( "[" << sections[i] << "]" );
                ran++;
                if (snap_and_transfer())
                    return EXIT_FAILURE;
            }
            if (group != "" && !ran) {
                ERR( "no config section has 'watch = " << group << "'" );
                return EXIT_FAILURE;
            }
            if (!snapshot_setup::do_sync)
                if (snap_finalize_sync())
                    return EXIT_FAILURE;
            return EXIT_SUCCESS;
        }
    }
    if (group != "") {
        ERR( "-g needs a config file with sections" );
        return EXIT_FAILURE;
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
