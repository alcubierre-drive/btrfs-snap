/*
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include "snap.hpp"

#include <vector>
#include <glob.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctime>
#include <linux/limits.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>

string snapshot_setup::host_name = "cygnus";
string snapshot_setup::backup_name = "home";
string snapshot_setup::backup_dir = "/home/";
string snapshot_setup::snapshot_dir = "/.snapshots/";
string snapshot_setup::remote_snapshot_dir = "/mnt/backup-mypass/snapshots/";
unsigned snapshot_setup::keep_remote_snapshots_num = 10;
unsigned snapshot_setup::keep_snapshots_num = 10;
bool snapshot_setup::dry_run = false;
string snapshot_setup::pre_command = "";
string snapshot_setup::post_command = "";
bool snapshot_setup::transfer = true;
bool snapshot_setup::create = true;

using std::vector;
using std::string;
using std::cerr;
using std::endl;
using std::sort;
using std::cout;

static int has_dir( string dir ); // 0: exists, 1: is file, -1: cannot access
static vector<string> glob_list( string expr );
static int execute( string command );
static string date_now();
static bool has_root_priv();

static int execute_pre_command( string command );
static int execute_post_command( string command, string snapshot_name );
static int btrfs_sync();
static int btrfs_create_snapshot( string backup_dir, string name );
static int btrfs_transfer_snapshot_difference( string prev_name, string name, string out_dir );
static int btrfs_transfer_full_snapshot( string name, string out_dir );
static int btrfs_delete_snapshot( string name );

int snap_and_transfer() {

    string run_date = date_now();

    if (!has_root_priv()) {
        ERR( "cannot get root privileges." );
        return EXIT_FAILURE;
    }

    if (has_dir( snapshot_setup::snapshot_dir )) {
        ERR( "snapshot directory '" << snapshot_setup::snapshot_dir << "' does not exist." );
        return EXIT_FAILURE;
    }

    if (has_dir( snapshot_setup::backup_dir )) {
        ERR( "backup directory '" << snapshot_setup::backup_dir << "' does not exist." );
        return EXIT_FAILURE;
    }

    if (snapshot_setup::keep_snapshots_num < 1) {
        ERR( "snapshot num kept must be larger than one." );
        return EXIT_FAILURE;
    }

    if (snapshot_setup::keep_remote_snapshots_num < 1) {
        ERR( "remote snapshot num kept must be larger than one." );
        return EXIT_FAILURE;
    }

    if (snapshot_setup::pre_command != "")
        if (execute_pre_command( snapshot_setup::pre_command ))
            return EXIT_FAILURE;

    string current_snap_name = snapshot_setup::host_name + "_" + snapshot_setup::backup_name + "_" + run_date;
    string current_snap_glob = snapshot_setup::host_name + "_" + snapshot_setup::backup_name + "_*/";

    if (snapshot_setup::create)
        if (btrfs_create_snapshot( snapshot_setup::backup_dir, snapshot_setup::snapshot_dir + current_snap_name ))
            return EXIT_FAILURE;

    vector<string> local_snapshots = glob_list( snapshot_setup::snapshot_dir + current_snap_glob );
    sort( local_snapshots.begin(), local_snapshots.end() );

    if (!snapshot_setup::create)
        current_snap_name = local_snapshots[local_snapshots.size()-1].substr(snapshot_setup::snapshot_dir.length(),
                string::npos);

    if (has_dir( snapshot_setup::remote_snapshot_dir ) || !snapshot_setup::transfer ) {
        if (!snapshot_setup::transfer)
            INFO( "transfer disabled. local operation." );
        else if (has_dir( snapshot_setup::remote_snapshot_dir ))
            WARN( "remote snapshot directory '" << snapshot_setup::remote_snapshot_dir <<
                    "' not present. local operation." );
        if (local_snapshots.size() > snapshot_setup::keep_snapshots_num) {
            unsigned del_num = 0;
            for (unsigned i=snapshot_setup::keep_snapshots_num; i<local_snapshots.size(); ++i) {
                if (btrfs_delete_snapshot( local_snapshots[del_num] ))
                    return EXIT_FAILURE;
                del_num++;
            }
        }
    } else {
        vector<string> remote_snapshots = glob_list( snapshot_setup::remote_snapshot_dir + current_snap_glob );
        sort( remote_snapshots.begin(), remote_snapshots.end() );
        reverse( remote_snapshots.begin(), remote_snapshots.end() );
        reverse( local_snapshots.begin(), local_snapshots.end() );
        if (remote_snapshots.size() == 0) {
            INFO( "no remote snapshots available, sending full snapshot to remote..." );
            if (btrfs_transfer_full_snapshot( snapshot_setup::snapshot_dir + current_snap_name,
                        snapshot_setup::remote_snapshot_dir ))
                return EXIT_FAILURE;
        } else {
            string match_base_name = "";
            for (string local: local_snapshots) {
                for (string remote: remote_snapshots) {
                    string local_base = local.substr( snapshot_setup::snapshot_dir.length(),
                            string::npos );
                    string remote_base = remote.substr( snapshot_setup::remote_snapshot_dir.length(),
                            string::npos );
                    if (local_base == remote_base) {
                        match_base_name = local_base;
                        goto breakloops;
                    }
                }
            }
breakloops:
            if (match_base_name == "") {
                INFO( "could not find matching snapshots, sending full snapshot to remote..." );
                if (btrfs_transfer_full_snapshot( snapshot_setup::snapshot_dir + current_snap_name,
                            snapshot_setup::remote_snapshot_dir ))
                    return EXIT_FAILURE;
            } else {
                if (match_base_name == current_snap_name)
                    INFO( "most recent snapshot already present on remote" );
                else {
                    INFO( "sending partial backup..." );
                    if (btrfs_transfer_snapshot_difference( snapshot_setup::snapshot_dir + match_base_name,
                            snapshot_setup::snapshot_dir + current_snap_name,
                            snapshot_setup::remote_snapshot_dir ))
                        return EXIT_FAILURE;
                }
            }
        }

        INFO( "cleaning up..." );
        for (unsigned i=snapshot_setup::keep_remote_snapshots_num-1; i<remote_snapshots.size(); ++i) {
            if (btrfs_delete_snapshot( remote_snapshots[i] ))
                return EXIT_FAILURE;
        }
        for (unsigned i=snapshot_setup::keep_snapshots_num; i<local_snapshots.size(); ++i) {
            if (btrfs_delete_snapshot( local_snapshots[i] ))
                return EXIT_FAILURE;
        }
    }

    if (btrfs_sync())
        return EXIT_FAILURE;

    if (snapshot_setup::post_command != "")
        if (execute_post_command( snapshot_setup::post_command, current_snap_name ))
            return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

vector<string> glob_list( string expr ) {
    glob_t G;
    G.gl_offs = 10;
    glob( expr.c_str(), GLOB_MARK | GLOB_NOCHECK, NULL, &G );
    vector<string> result(G.gl_pathc);
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

string tempfile() {
    char tmpnam[PATH_MAX];
    std::tmpnam( tmpnam );
    return tmpnam;
}

int execute( string command ) {
    string fname = tempfile();
    command += " 2>&1 > '" + fname + "'";
    int result = system( command.c_str() );
    std::ifstream ftmp( fname );
    for (string line; std::getline(ftmp, line); ) {
        SHELL( line );
    }
    ftmp.close();
    remove( fname.c_str() );
    return result;
}

int execute_pre_command( string command ) {
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

static string ReplaceString(string subject, const string& search, const string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    }
    return subject;
}

int execute_post_command( string command_, string snapshot_name ) {
    string command = ReplaceString( command_, "%SNAPSHOT%", snapshot_name );
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

int btrfs_create_snapshot( string backup_dir, string name ) {
    string command = "btrfs subvolume snapshot -r '";
    command += backup_dir + "' '" + name + "'";
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

int btrfs_transfer_snapshot_difference( string prev_name, string name, string out_dir ) {
    string command = "btrfs send -p '";
    command += prev_name + "' '" + name + "' 2>/dev/null | btrfs receive '" + out_dir + "'";
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

int btrfs_transfer_full_snapshot( string name, string out_dir ) {
    string command = "btrfs send '";
    command += name + "' 2>/dev/null | btrfs receive '" + out_dir + "'";
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

int btrfs_delete_snapshot( string name ) {
    string command = "btrfs subvolume delete '";
    command += name + "'";
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
    return execute( command );
}

int btrfs_sync() {
    string command = "sync";
    INFO( command );
    if (snapshot_setup::dry_run) return 0;
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
    if (snapshot_setup::dry_run) return true;
    return !geteuid();
}

int setup_variables_saved( string name ) {
    if (name == "home") {
        snapshot_setup::backup_name = "home";
        snapshot_setup::backup_dir = "/home/";
    } else if (name == "root") {
        snapshot_setup::backup_name = "root";
        snapshot_setup::backup_dir = "/";
    } else if (name == "sirwer_home") {
        snapshot_setup::backup_name = "home";
        snapshot_setup::backup_dir = "/mnt/btrfs_discs/hdd/subvol_home/";
        snapshot_setup::host_name = "sirwer";
        snapshot_setup::transfer = false;
        snapshot_setup::snapshot_dir = "/.snapshots_data/";
    } else if (name == "sirwer_root") {
        snapshot_setup::backup_name = "root";
        snapshot_setup::backup_dir = "/";
        snapshot_setup::host_name = "sirwer";
        snapshot_setup::snapshot_dir = "/.snapshots/";
        snapshot_setup::remote_snapshot_dir = "/mnt/btrfs_discs/hdd/backup_ssd/";
        snapshot_setup::keep_snapshots_num = 1;
        snapshot_setup::pre_command = "tar -P -f /boot.tar.gz -z -c /mnt/btrfs_discs/ssd/boot";
    } else if (name == "home_hdd") {
        snapshot_setup::backup_name = "home";
        snapshot_setup::backup_dir = "/home/";
        snapshot_setup::remote_snapshot_dir = "/mnt/backup-hdd/snapshots/";
        snapshot_setup::keep_remote_snapshots_num = 3;
    } else if (name == "root_hdd") {
        snapshot_setup::backup_name = "root";
        snapshot_setup::backup_dir = "/";
        snapshot_setup::remote_snapshot_dir = "/mnt/backup-hdd/snapshots/";
        snapshot_setup::keep_remote_snapshots_num = 3;
    } else {
        ERR( "pre-saved mode can be 'home', 'root', 'sirwer_home', 'sirwer_root', 'root_hdd', 'home_hdd'" );
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


