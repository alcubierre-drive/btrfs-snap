/*
 * btrfs-snap-cpp status|backup|mount|umount|stop|answer|reload|ask: talk to
 * the daemon. Works without root; mutating commands are authorized by polkit.
 *
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include "ipc.hpp"
#include "modes.hpp"

#include <systemd/sd-bus.h>

#include <csignal>
#include <cstdarg>
#include <getopt.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using namespace bsc;

sd_bus* bus = nullptr;

int fail_call( const char* method, const sd_bus_error* e, int r ) {
    fprintf( stderr, "error: %s\n", bus_error_text( e, r ) );
    if (e && e->name && (!strcmp( e->name, SD_BUS_ERROR_SERVICE_UNKNOWN ) ||
                !strcmp( e->name, SD_BUS_ERROR_NAME_HAS_NO_OWNER )))
        fprintf( stderr, "is the daemon running? (systemctl status btrfs-snap-cpp)\n" );
    (void)method;
    return EXIT_FAILURE;
}

// call a daemon method (interactive polkit allowed, generous timeout)
int call( sd_bus_message** reply, const char* method, const char* types, ... ) {
    sd_bus_message* m = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_message_new_method_call( bus, &m, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE, method );
    if (r >= 0 && types && types[0]) {
        va_list ap;
        va_start( ap, types );
        r = sd_bus_message_appendv( m, types, ap );
        va_end( ap );
    }
    if (r >= 0) r = sd_bus_message_set_allow_interactive_authorization( m, 1 );
    if (r >= 0) r = sd_bus_call( bus, m, 300 * USEC_PER_SEC, &err, reply );
    sd_bus_message_unref( m );
    if (r < 0) {
        fail_call( method, &err, r );
        sd_bus_error_free( &err );
        return r;
    }
    sd_bus_error_free( &err );
    return 0;
}

vector<string> read_strv( sd_bus_message* m ) {
    vector<string> out;
    char** l = nullptr;
    if (sd_bus_message_read_strv( m, &l ) >= 0 && l) {
        for (char** p = l; *p; ++p) {
            out.push_back( *p );
            free( *p );
        }
        free( l );
    }
    return out;
}

int cmd_status() {
    sd_bus_message* reply = nullptr;
    if (call( &reply, "ListGroups", "" ) < 0) return EXIT_FAILURE;
    if (sd_bus_message_enter_container( reply, 'a', "(ssuasasas)" ) >= 0) {
        bool any = false;
        while (sd_bus_message_enter_container( reply, 'r', "ssuasasas" ) > 0) {
            const char *name, *state;
            uint32_t q;
            sd_bus_message_read( reply, "ssu", &name, &state, &q );
            auto sections = read_strv( reply );
            auto devs = read_strv( reply );
            auto mps = read_strv( reply );
            sd_bus_message_exit_container( reply );
            any = true;
            printf( "%s: %s", name, state );
            if (q) printf( "  (question #%u pending)", q );
            printf( "\n  sections: %s\n", join( sections, ", " ).c_str() );
            for (auto& d: devs) printf( "  device:   %s\n", d.c_str() );
            for (auto& mp: mps) printf( "  mount:    %s\n", mp.c_str() );
        }
        if (!any) printf( "no watch groups configured\n" );
    }
    sd_bus_message_unref( reply );

    if (call( &reply, "GetQuestions", "" ) < 0) return EXIT_FAILURE;
    bool header = false;
    if (sd_bus_message_enter_container( reply, 'a', "(ussa(ss)sss)" ) >= 0) {
        while (sd_bus_message_enter_container( reply, 'r', "ussa(ss)sss" ) > 0) {
            uint32_t id;
            const char *group, *kind, *def, *text, *log, *key, *label;
            sd_bus_message_read( reply, "uss", &id, &group, &kind );
            vector<string> keys;
            string labels;
            sd_bus_message_enter_container( reply, 'a', "(ss)" );
            while (sd_bus_message_read( reply, "(ss)", &key, &label ) > 0) {
                keys.push_back( key );
                labels += string( labels.empty() ? "" : ", " ) + key + " = " + label;
            }
            sd_bus_message_exit_container( reply );
            sd_bus_message_read( reply, "sss", &def, &text, &log );
            sd_bus_message_exit_container( reply );
            if (!header) { printf( "\npending questions:\n" ); header = true; }
            string t = text;
            for (size_t p = 0; (p = t.find( '\n', p )) != string::npos; p += 7) t.replace( p, 1, "\n      " );
            printf( "  #%u %s (%s):\n      %s\n      choices: %s\n      default: %s (if never answered: nothing happens)\n"
                    "      answer:  btrfs-snap-cpp answer %u %s\n",
                    id, group, kind, t.c_str(), labels.c_str(), def, id, join( keys, "|" ).c_str() );
            if (log[0]) printf( "      log:     %s\n", log );
        }
    }
    sd_bus_message_unref( reply );
    return EXIT_SUCCESS;
}

struct wait_state {
    string group;
    bool done = false;
    bool ok = false;
    string summary, log;
};

int on_finished( sd_bus_message* m, void* userdata, sd_bus_error* ) {
    wait_state& w = *(wait_state*)userdata;
    const char *group, *summary, *log;
    int ok;
    if (sd_bus_message_read( m, "sbss", &group, &ok, &summary, &log ) < 0 || w.group != group) return 0;
    w.summary = summary;
    w.log = log;
    w.done = true;
    w.ok = ok;
    return 0;
}

int on_changed( sd_bus_message* m, void* userdata, sd_bus_error* ) {
    wait_state& w = *(wait_state*)userdata;
    const char *group, *state;
    if (sd_bus_message_read( m, "ss", &group, &state ) >= 0 && w.group == group)
        printf( "%s: %s\n", group, state );
    return 0;
}

// journalctl -f on the backup unit, from just before the start
pid_t spawn_journal_follow( const string& group, time_t since ) {
    string unit = backup_unit_name( group );
    string since_arg = "--since=@" + std::to_string( (long long)since - 1 );
    vector<const char*> args = { "journalctl" };
    if (session_mode()) args.push_back( "--user" );
    for (const char* a: { "--follow", "--output=cat", "--unit" }) args.push_back( a );
    args.push_back( unit.c_str() );
    args.push_back( since_arg.c_str() );
    args.push_back( nullptr );
    pid_t pid = -1;
    int r = posix_spawnp( &pid, "journalctl", nullptr, nullptr, (char* const*)args.data(), environ );
    if (r) {
        fprintf( stderr, "warning: cannot run journalctl: %s\n", strerror( r ) );
        return -1;
    }
    return pid;
}

int cmd_backup( int argc, char** argv ) {
    bool wait = false, follow = false;
    static const struct option longopts[] = {
        { "wait", no_argument, nullptr, 'w' },
        { "follow", no_argument, nullptr, 'f' },
        { nullptr, 0, nullptr, 0 },
    };
    int opt;
    optind = 1;
    while ((opt = getopt_long( argc, argv, "wf", longopts, nullptr )) != -1) {
        if (opt == 'w') wait = true;
        else if (opt == 'f') follow = wait = true;
        else return EXIT_FAILURE;
    }
    if (optind != argc-1) {
        fprintf( stderr, "usage: btrfs-snap-cpp backup [-w|--wait] [-f|--follow] <group>\n"
                "  -w  wait for the backup to finish, print the result\n"
                "  -f  like -w, and show the backup's log live\n" );
        return EXIT_FAILURE;
    }
    time_t started = time( nullptr );
    wait_state w;
    w.group = argv[optind];
    if (wait) {
        sd_bus_match_signal( bus, nullptr, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE, "BackupFinished", on_finished, &w );
        sd_bus_match_signal( bus, nullptr, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE, "GroupChanged", on_changed, &w );
    }
    if (call( nullptr, "Backup", "s", w.group.c_str() ) < 0) return EXIT_FAILURE;
    if (!wait) {
        printf( "backup of '%s' started\n", w.group.c_str() );
        return EXIT_SUCCESS;
    }
    pid_t journal = follow ? spawn_journal_follow( w.group, started ) : -1;
    while (!w.done) {
        int r = sd_bus_process( bus, nullptr );
        if (r < 0) { fprintf( stderr, "error: %s\n", strerror( -r ) ); break; }
        if (r == 0) sd_bus_wait( bus, UINT64_MAX );
    }
    if (journal > 0) {
        usleep( 500000 ); // let journalctl print the last lines
        kill( journal, SIGTERM );
        waitpid( journal, nullptr, 0 );
    }
    if (w.done)
        printf( "%s\nlog: %s\n", w.summary.c_str(), w.log.c_str() );
    return w.done && w.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ask [<group>]: show the pending question again, or ask the one for the
// group's state; without a group, for every group
int cmd_ask( int argc, char** argv ) {
    if (argc > 2) {
        fprintf( stderr, "usage: btrfs-snap-cpp ask [<group>]\n" );
        return EXIT_FAILURE;
    }
    sd_bus_message* reply = nullptr;
    if (call( &reply, "Ask", "s", argc == 2 ? argv[1] : "" ) < 0) return EXIT_FAILURE;
    bool any = false, none = true;
    if (sd_bus_message_enter_container( reply, 'a', "(sbs)" ) >= 0) {
        const char *group, *what;
        int ok;
        while (sd_bus_message_read( reply, "(sbs)", &group, &ok, &what ) > 0) {
            printf( "%s: %s\n", group, what );
            any |= ok;
            none = false;
        }
    }
    sd_bus_message_unref( reply );
    if (none) printf( "no watch groups configured\n" );
    return any ? EXIT_SUCCESS : EXIT_FAILURE;
}

int group_cmd( int argc, char** argv, const char* method ) {
    if (argc != 2) {
        fprintf( stderr, "usage: btrfs-snap-cpp %s <group>\n", argv[0] );
        return EXIT_FAILURE;
    }
    return call( nullptr, method, "s", argv[1] ) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

const char* const commands[] = { "status", "backup", "mount", "umount", "stop", "answer", "reload", "ask" };

} // namespace

bool is_client_command( const std::string& word ) {
    for (const char* c: commands)
        if (word == c) return true;
    return false;
}

int client_main( int argc, char** argv ) {
    int r = open_daemon_bus( &bus );
    if (r < 0) {
        fprintf( stderr, "error: cannot connect to bus: %s\n", strerror( -r ) );
        return EXIT_FAILURE;
    }
    string cmd = argv[0];
    int ret;
    if (cmd == "status") {
        ret = cmd_status();
    } else if (cmd == "backup") {
        ret = cmd_backup( argc, argv );
    } else if (cmd == "mount") {
        ret = group_cmd( argc, argv, "Mount" );
    } else if (cmd == "umount") {
        ret = group_cmd( argc, argv, "Unmount" );
    } else if (cmd == "stop") {
        ret = group_cmd( argc, argv, "Stop" );
    } else if (cmd == "answer") {
        if (argc != 3) {
            fprintf( stderr, "usage: btrfs-snap-cpp answer <question id> <choice>\n" );
            ret = EXIT_FAILURE;
        } else {
            ret = call( nullptr, "Answer", "us", (uint32_t)strtoul( argv[1], nullptr, 10 ), argv[2] ) < 0
                ? EXIT_FAILURE : EXIT_SUCCESS;
        }
    } else if (cmd == "ask") {
        ret = cmd_ask( argc, argv );
    } else { // reload
        ret = call( nullptr, "Reload", "" ) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    sd_bus_flush_close_unref( bus );
    return ret;
}
