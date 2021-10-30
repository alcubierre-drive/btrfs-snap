/*
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include <iostream>
#include <vector>
#include <string>
#include <glob.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <ctime>
#include <unistd.h>
#include <algorithm>

#define WARN( str ) cerr << "warning: " << str << endl
#define ERR( str ) cerr << "error: " << str << endl
#define INFO( str ) cerr << "info: " << str << endl

using std::vector;
using std::string;
using std::cerr;
using std::endl;
using std::sort;
using std::cout;

string host_name = "cygnus";
string backup_name = "home";
string backup_dir = "/home/";
string snapshot_dir = "/.snapshots/";
string remote_snapshot_dir = "/mnt/backup-mypass/snapshots/";
unsigned keep_remote_snapshots_num = 10;
unsigned keep_snapshots_num = 10;
bool dry_run = false;

void setup_variables_home_partition();
void setup_variables_root_partition();

int has_dir( string dir ); // 0: exists, 1: is file, -1: cannot access
vector<string> glob_list( string expr );
int execute( string command );
string date_now();
bool has_root_priv();

void print_help( string progname );

int btrfs_sync();
int btrfs_create_snapshot( string backup_dir, string name );
int btrfs_transfer_snapshot_difference( string prev_name, string name, string out_dir );
int btrfs_transfer_full_snapshot( string name, string out_dir );
int btrfs_delete_snapshot( string name );

int main( int argc, char** argv ) {
    int opt;
    string setup_variables = "";
    while ((opt = getopt(argc, argv, ":hdR:S:r:s:b:B:p:H:")) != -1) {
        switch (opt) {
            case 'h':
                print_help( argv[0] );
                return EXIT_SUCCESS;
            case 'R':
                remote_snapshot_dir = string(optarg);
                break;
            case 'S':
                snapshot_dir = string(optarg);
                break;
            case 'r':
                keep_remote_snapshots_num = atoi(optarg);
                break;
            case 's':
                keep_snapshots_num = atoi(optarg);
                break;
            case 'b':
                backup_dir = string(optarg);
                break;
            case 'B':
                backup_name = string(optarg);
                break;
            case 'p':
                setup_variables = string(optarg);
                break;
            case 'H':
                host_name = string(optarg);
                break;
            case 'd':
                dry_run = true;
                break;
            case '?':
                WARN( "unknown option '-" << (char)optopt << "'. show help with -h" );
                break;
            default:
                break;
        }
    }

    if (setup_variables == "home") {
        setup_variables_home_partition();
    } else if (setup_variables == "root") {
        setup_variables_root_partition();
    } else if (setup_variables == "") {} else {
        ERR( "cannot set up variables for partition != 'home' or 'root'" );
        return EXIT_FAILURE;
    }

    string run_date = date_now();

    if (!has_root_priv()) {
        ERR( "cannot get root privileges." );
        return EXIT_FAILURE;
    }

    if (has_dir( snapshot_dir )) {
        ERR( "snapshot directory '" << snapshot_dir << "' does not exist." );
        return EXIT_FAILURE;
    }

    if (has_dir( backup_dir )) {
        ERR( "backup directory '" << backup_dir << "' does not exist." );
        return EXIT_FAILURE;
    }

    if (keep_snapshots_num < 1) {
        ERR( "snapshot num kept must be larger than one." );
        return EXIT_FAILURE;
    }

    if (keep_remote_snapshots_num < 1) {
        ERR( "remote snapshot num kept must be larger than one." );
        return EXIT_FAILURE;
    }

    string current_snap_name = host_name + "_" + backup_name + "_" + run_date;
    string current_snap_glob = host_name + "_" + backup_name + "_*/";

    btrfs_create_snapshot( backup_dir, snapshot_dir + current_snap_name );

    vector<string> local_snapshots = glob_list( snapshot_dir + current_snap_glob );
    sort( local_snapshots.begin(), local_snapshots.end() );

    if (has_dir( remote_snapshot_dir )) {
        INFO( "remote snapshot directory '" << remote_snapshot_dir << "' not present. local operation." );
        if (local_snapshots.size() > keep_snapshots_num) {
            unsigned del_num = 0;
            for (unsigned i=keep_snapshots_num; i<local_snapshots.size(); ++i) {
                btrfs_delete_snapshot( local_snapshots[del_num] );
                del_num++;
            }
        }
    } else {
        vector<string> remote_snapshots = glob_list( remote_snapshot_dir + current_snap_glob );
        sort( remote_snapshots.begin(), remote_snapshots.end() );
        reverse( remote_snapshots.begin(), remote_snapshots.end() );
        reverse( local_snapshots.begin(), local_snapshots.end() );
        if (remote_snapshots.size() == 0) {
            INFO( "no remote snapshots available, sending full snapshot to remote..." );
            btrfs_transfer_full_snapshot( snapshot_dir + current_snap_name, remote_snapshot_dir );
        } else {
            string match_base_name = "";
            for (string local: local_snapshots) {
                for (string remote: remote_snapshots) {
                    string local_base = local.substr( snapshot_dir.length(), string::npos );
                    string remote_base = remote.substr( remote_snapshot_dir.length(), string::npos );
                    if (local_base == remote_base) {
                        match_base_name = local_base;
                        goto breakloops;
                    }
                }
            }
breakloops:
            if (match_base_name == "") {
                INFO( "could not find matching snapshots, sending full snapshot to remote..." );
                btrfs_transfer_full_snapshot( snapshot_dir + current_snap_name, remote_snapshot_dir );
            } else {
                INFO( "sending partial backup..." );
                btrfs_transfer_snapshot_difference( snapshot_dir + match_base_name,
                        snapshot_dir + current_snap_name,
                        remote_snapshot_dir );
            }
        }

        INFO( "cleaning up..." );
        for (unsigned i=keep_remote_snapshots_num-1; i<remote_snapshots.size(); ++i) {
            btrfs_delete_snapshot( remote_snapshots[i] );
        }
        for (unsigned i=keep_snapshots_num; i<local_snapshots.size(); ++i) {
            btrfs_delete_snapshot( local_snapshots[i] );
        }
    }

    return EXIT_SUCCESS;
}

vector<string> glob_list( string expr ) {
    glob_t G;
    G.gl_offs = 10;
    glob( expr.c_str(), GLOB_MARK | GLOB_NOCHECK, NULL, &G );
    std::vector<std::string> result(G.gl_pathc);
    for (size_t i=0; i<G.gl_pathc; ++i) {
        result[i] = G.gl_pathv[i];
    }
    globfree( &G );
    return result;
}

int has_dir( string pathname ) {
    struct stat info;
    if( stat( pathname.c_str(), &info ) != 0 )
        return -1;
    else if( info.st_mode & S_IFDIR )
        return 0;
    else
        return 1;
}

int execute( string command ) {
    return system( command.c_str() );
}

int btrfs_create_snapshot( string backup_dir, string name ) {
    string command = "btrfs subvolume snapshot -r '";
    command += backup_dir + "' '" + name + "'";
    INFO( command );
    if (dry_run) return 0;
    return execute( command );
}

int btrfs_transfer_snapshot_difference( string prev_name, string name, string out_dir ) {
    string command = "btrfs send -p '";
    command += prev_name + "' '" + name + "' | btrfs receive '" + out_dir + "'";
    INFO( command );
    if (dry_run) return 0;
    return execute( command );
}

int btrfs_transfer_full_snapshot( string name, string out_dir ) {
    string command = "btrfs send '";
    command += name + "' | btrfs receive '" + out_dir + "'";
    INFO( command );
    if (dry_run) return 0;
    return execute( command );
}

int btrfs_delete_snapshot( string name ) {
    string command = "btrfs subvolume delete '";
    command += name + "'";
    INFO( command );
    if (dry_run) return 0;
    return execute( command );
}

int btrfs_sync() {
    string command = "sync";
    INFO( command );
    if (dry_run) return 0;
    return execute( command );
}

string date_now() {
    time_t now = time(NULL);
    struct tm tstruct;
    char buf[256];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tstruct);
    return buf;
}

bool has_root_priv() {
    if (dry_run) return true;
    return !geteuid();
}

void print_help( string progname ) {
    cout << "usage: " << progname << " <options> to back up btrfs file systems" << endl
         << "options: -h              show this help" << endl
         << "         -R <dir>        set remote snapshot dir" << endl
         << "         -S <dir>        set local snapshot dir" << endl
         << "         -r <num>        set remote snapshot num to keep" << endl
         << "         -s <num>        set local snapshot num to keep " << endl
         << "         -b <dir>        set backup dir to mirror" << endl
         << "         -B <name>       set short name of backup" << endl
         << "         -p home|root    set root/home partition mode" << endl
         << "         -H <name>       set hostname (name prefix)" << endl
         << "         -d              dry run (only print commands)" << endl;
}

void setup_variables_home_partition() {
    backup_name = "home";
    backup_dir = "/home/";
}
void setup_variables_root_partition() {
    backup_name = "root";
    backup_dir = "/";
}

