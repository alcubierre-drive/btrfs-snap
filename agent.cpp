/*
 * btrfs-snap-cpp agent: user session service. Shows the daemon's questions
 * as desktop notifications (org.freedesktop.Notifications) and sends the
 * chosen action back with Answer(). Closing a notification does not answer;
 * the question stays pending (see "btrfs-snap-cpp status").
 *
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include "ipc.hpp"
#include "modes.hpp"

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <csignal>
#include <cstdarg>
#include <map>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace {

using namespace bsc;

bool verbose = false;
string terminal; // command prefix for "Show log", e.g. "kitty" or "foot"

void log_msg( const char* fmt, ... ) __attribute__((format(printf, 1, 2)));
void log_msg( const char* fmt, ... ) {
    va_list ap;
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fputc( '\n', stderr );
}
#define LOG( ... ) log_msg( __VA_ARGS__ )
#define DBG( ... ) do { if (verbose) log_msg( __VA_ARGS__ ); } while (0)

sd_bus* daemon_bus = nullptr; // where the daemon lives (system bus)
sd_bus* user_bus = nullptr;   // notifications

struct question {
    uint32_t id = 0;
    string group, kind, def, text, log;
    vector<std::pair<string, string>> choices;
};

struct shown {
    uint32_t question = 0; // 0: plain info notification
    string group;
    string log;
    string def;            // answer for the notification's "default" action
};
std::map<uint32_t, shown> notifs;             // notification id -> what it shows
std::map<uint32_t, uint32_t> question_notif;  // question id -> notification id
std::map<string, uint32_t> finished_notif;    // group -> BackupFinished notification

string markup_escape( const string& s ) {
    string out;
    for (char c: s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

uint32_t notify( uint32_t replaces, const string& summary, const string& body,
        const vector<string>& actions, uint8_t urgency, int32_t timeout ) {
    sd_bus_message* m = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    uint32_t id = 0;
    int r = sd_bus_message_new_method_call( user_bus, &m, "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify" );
    if (r >= 0) r = sd_bus_message_append( m, "susss", "btrfs-snap-cpp", replaces, "drive-harddisk",
            summary.c_str(), markup_escape( body ).c_str() );
    if (r >= 0) r = sd_bus_message_open_container( m, 'a', "s" );
    for (auto& a: actions)
        if (r >= 0) r = sd_bus_message_append_basic( m, 's', a.c_str() );
    if (r >= 0) r = sd_bus_message_close_container( m );
    if (r >= 0) r = sd_bus_message_append( m, "a{sv}i", 1, "urgency", "y", urgency, timeout );
    if (r >= 0) r = sd_bus_call( user_bus, m, 10 * USEC_PER_SEC, &err, &reply );
    if (r >= 0) r = sd_bus_message_read( reply, "u", &id );
    if (r < 0) {
        LOG( "notification failed: %s", bus_error_text( &err, r ) );
        id = 0;
    }
    sd_bus_error_free( &err );
    sd_bus_message_unref( m );
    sd_bus_message_unref( reply );
    return id;
}

void notify_close( uint32_t id ) {
    if (!id) return;
    sd_bus_call_method_async( user_bus, nullptr, "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
            "CloseNotification", nullptr, nullptr, "u", id );
}

void forget_notif( uint32_t id ) {
    auto it = notifs.find( id );
    if (it == notifs.end()) return;
    if (it->second.question) question_notif.erase( it->second.question );
    for (auto f = finished_notif.begin(); f != finished_notif.end(); )
        f = f->second == id ? finished_notif.erase( f ) : std::next( f );
    notifs.erase( it );
}

void spawn_shell( const string& cmd ) {
    posix_spawnattr_t attr;
    posix_spawnattr_init( &attr );
    sigset_t none, def;
    sigemptyset( &none );
    sigemptyset( &def );
    sigaddset( &def, SIGCHLD );
    posix_spawnattr_setsigmask( &attr, &none );
    posix_spawnattr_setsigdefault( &attr, &def );
    posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID );
    const char* argv[] = { "/bin/sh", "-c", cmd.c_str(), nullptr };
    pid_t pid;
    int r = posix_spawn( &pid, "/bin/sh", nullptr, &attr, (char* const*)argv, environ );
    if (r) LOG( "cannot spawn '%s': %s", cmd.c_str(), strerror( r ) );
    posix_spawnattr_destroy( &attr );
}

int read_question( sd_bus_message* m, question& q ) {
    const char *group, *kind, *def, *text, *log;
    int r = sd_bus_message_read( m, "uss", &q.id, &group, &kind );
    if (r < 0) return r;
    q.group = group;
    q.kind = kind;
    if ((r = sd_bus_message_enter_container( m, 'a', "(ss)" )) < 0) return r;
    const char *key, *label;
    while ((r = sd_bus_message_read( m, "(ss)", &key, &label )) > 0)
        q.choices.emplace_back( key, label );
    if (r < 0) return r;
    if ((r = sd_bus_message_exit_container( m )) < 0) return r;
    if ((r = sd_bus_message_read( m, "sss", &def, &text, &log )) < 0) return r;
    q.def = def;
    q.text = text;
    q.log = log;
    return 1;
}

void show_question( const question& q ) {
    if (question_notif.count( q.id )) return;
    vector<string> actions;
    // clicking the notification itself (the "default" action, or whatever a
    // notification daemon picks when there is none) must be harmless: bind it
    // to the question's default, which never mounts/backs up/unmounts
    if (!q.def.empty())
        for (auto& c: q.choices)
            if (c.first == q.def) {
                actions.push_back( "default" );
                actions.push_back( c.second );
            }
    for (auto& c: q.choices) {
        actions.push_back( "c:" + c.first );
        actions.push_back( c.second );
    }
    if (!q.log.empty() && !terminal.empty()) {
        actions.push_back( "log" );
        actions.push_back( "Show log" );
    }
    uint32_t replaces = 0;
    if (q.kind == "finish") {
        auto it = finished_notif.find( q.group );
        if (it != finished_notif.end()) {
            replaces = it->second;
            forget_notif( replaces );
        }
    }
    string summary = q.kind == "connect" ? "Backup disks '" + q.group + "' connected"
                                         : "Backup disks '" + q.group + "'";
    uint32_t id = notify( replaces, summary, q.text, actions, 1, 0 );
    if (!id) return;
    notifs[id] = shown{ q.id, q.group, q.log, q.def };
    question_notif[q.id] = id;
    DBG( "question #%u -> notification %u", q.id, id );
}

void clear_questions() {
    for (auto it = question_notif.begin(); it != question_notif.end(); ) {
        uint32_t n = it->second;
        ++it;
        notify_close( n );
        forget_notif( n );
    }
}

void load_questions() {
    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method( daemon_bus, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE, "GetQuestions",
            &err, &reply, "" );
    if (r < 0) {
        LOG( "daemon not reachable (%s); waiting for it", bus_error_text( &err, r ) );
    } else if (sd_bus_message_enter_container( reply, 'a', "(ussa(ss)sss)" ) >= 0) {
        while (sd_bus_message_enter_container( reply, 'r', "ussa(ss)sss" ) > 0) {
            question q;
            if (read_question( reply, q ) < 0) break;
            sd_bus_message_exit_container( reply );
            show_question( q );
        }
    }
    sd_bus_error_free( &err );
    sd_bus_message_unref( reply );
}

/* --------------------------------------------------------- daemon signals */

int on_question( sd_bus_message* m, void*, sd_bus_error* ) {
    question q;
    if (read_question( m, q ) > 0) show_question( q );
    return 0;
}

int on_resolved( sd_bus_message* m, void*, sd_bus_error* ) {
    uint32_t id;
    const char *answer, *by;
    if (sd_bus_message_read( m, "uss", &id, &answer, &by ) < 0) return 0;
    auto it = question_notif.find( id );
    if (it == question_notif.end()) return 0;
    uint32_t n = it->second;
    notify_close( n );
    forget_notif( n );
    return 0;
}

int on_finished( sd_bus_message* m, void*, sd_bus_error* ) {
    const char *group, *summary, *log;
    int ok;
    if (sd_bus_message_read( m, "sbss", &group, &ok, &summary, &log ) < 0) return 0;
    vector<string> actions;
    if (log[0] && !terminal.empty()) actions = { "log", "Show log" };
    uint32_t id = notify( 0, ok ? "Backup finished" : "Backup failed", summary, actions, ok ? 1 : 2, ok ? -1 : 0 );
    if (!id) return 0;
    notifs[id] = shown{ 0, group, log, "" };
    finished_notif[group] = id;
    return 0;
}

int on_error( sd_bus_message* m, void*, sd_bus_error* ) {
    const char *group, *msg;
    if (sd_bus_message_read( m, "ss", &group, &msg ) < 0) return 0;
    notify( 0, string( "Backup disks '" ) + group + "': error", msg, {}, 2, -1 );
    return 0;
}

int on_owner_changed( sd_bus_message* m, void*, sd_bus_error* ) {
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read( m, "sss", &name, &old_owner, &new_owner ) < 0) return 0;
    if (strcmp( name, BSC_BUS_NAME )) return 0;
    // daemon restarted or gone: its questions are gone with it
    clear_questions();
    if (new_owner[0]) {
        LOG( "daemon (re)started" );
        load_questions();
    }
    return 0;
}

/* --------------------------------------------------- notification signals */

int on_answer_reply( sd_bus_message* m, void*, sd_bus_error* ) {
    // the daemon also broadcasts Error for failed actions; only report the
    // failures that happen before any action (authorization, stale question)
    if (const sd_bus_error* e = sd_bus_message_get_error( m ))
        if (strcmp( e->name, "org.btrfssnapcpp.Error.Failed" ))
            notify( 0, "Backup disks: answer failed", bus_error_text( e, 0 ), {}, 2, -1 );
    return 0;
}

int on_action( sd_bus_message* m, void*, sd_bus_error* ) {
    uint32_t id;
    const char* key;
    if (sd_bus_message_read( m, "us", &id, &key ) < 0) return 0;
    auto it = notifs.find( id );
    if (it == notifs.end()) return 0;
    if (!strcmp( key, "log" )) {
        spawn_shell( terminal + " " + it->second.log );
    } else if ((!strncmp( key, "c:", 2 ) || !strcmp( key, "default" )) && it->second.question) {
        string answer = strcmp( key, "default" ) ? key+2 : it->second.def;
        if (answer.empty()) return 0;
        DBG( "answering #%u with %s", it->second.question, answer.c_str() );
        sd_bus_message* call = nullptr;
        int r = sd_bus_message_new_method_call( daemon_bus, &call, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE, "Answer" );
        if (r >= 0) r = sd_bus_message_append( call, "us", it->second.question, answer.c_str() );
        if (r >= 0) r = sd_bus_message_set_allow_interactive_authorization( call, 1 );
        if (r >= 0) r = sd_bus_call_async( daemon_bus, nullptr, call, on_answer_reply, nullptr, 300 * USEC_PER_SEC );
        if (r < 0) LOG( "cannot answer: %s", strerror( -r ) );
        sd_bus_message_unref( call );
    }
    return 0;
}

int on_closed( sd_bus_message* m, void*, sd_bus_error* ) {
    uint32_t id, reason;
    if (sd_bus_message_read( m, "uu", &id, &reason ) < 0) return 0;
    forget_notif( id );
    return 0;
}

void print_help( const char* prog ) {
    printf( "usage: %s agent [options]\n"
            "show btrfs-snap-cpp daemon questions as desktop notifications\n"
            "options: -h              show this help\n"
            "         -t <cmd>        terminal command prefix for 'Show log' (default $TERMINAL)\n"
            "         -v              verbose\n", prog );
}

#define CHECK( expr, what ) do { int _r = (expr); if (_r < 0) { \
        LOG( "%s: %s", what, strerror( -_r ) ); return EXIT_FAILURE; } } while (0)

} // namespace

int agent_main( int argc, char** argv ) {
    if (const char* t = getenv( "TERMINAL" )) terminal = t;
    int opt;
    optind = 1;
    while ((opt = getopt( argc, argv, "hvt:" )) != -1) {
        switch (opt) {
            case 'h': print_help( "btrfs-snap-cpp" ); return EXIT_SUCCESS;
            case 'v': verbose = true; break;
            case 't': terminal = optarg; break;
            default: print_help( "btrfs-snap-cpp" ); return EXIT_FAILURE;
        }
    }
    setvbuf( stderr, nullptr, _IOLBF, 0 );

    sigset_t ss;
    sigemptyset( &ss );
    sigaddset( &ss, SIGTERM );
    sigaddset( &ss, SIGINT );
    sigprocmask( SIG_BLOCK, &ss, nullptr );
    signal( SIGCHLD, SIG_IGN ); // auto-reap spawned terminals

    sd_event* event = nullptr;
    CHECK( sd_event_default( &event ), "sd_event_default" );
    CHECK( sd_event_add_signal( event, nullptr, SIGTERM, nullptr, nullptr ), "signal" );
    CHECK( sd_event_add_signal( event, nullptr, SIGINT, nullptr, nullptr ), "signal" );

    CHECK( open_daemon_bus( &daemon_bus ), "cannot connect to daemon bus" );
    CHECK( sd_bus_attach_event( daemon_bus, event, 0 ), "attach daemon bus" );
    CHECK( sd_bus_open_user( &user_bus ), "cannot connect to session bus" );
    CHECK( sd_bus_attach_event( user_bus, event, 0 ), "attach session bus" );

    struct { const char* member; sd_bus_message_handler_t fn; } sigs[] = {
        { "Question", on_question }, { "QuestionResolved", on_resolved },
        { "BackupFinished", on_finished }, { "Error", on_error },
    };
    for (auto& s: sigs)
        CHECK( sd_bus_match_signal( daemon_bus, nullptr, BSC_BUS_NAME, BSC_OBJ_PATH, BSC_IFACE,
                    s.member, s.fn, nullptr ), "match daemon signal" );
    CHECK( sd_bus_match_signal( daemon_bus, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameOwnerChanged", on_owner_changed, nullptr ), "match owner" );
    CHECK( sd_bus_match_signal( user_bus, nullptr, nullptr, "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", "ActionInvoked", on_action, nullptr ), "match ActionInvoked" );
    CHECK( sd_bus_match_signal( user_bus, nullptr, nullptr, "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", "NotificationClosed", on_closed, nullptr ),
            "match NotificationClosed" );

    load_questions();
    int r = sd_event_loop( event );

    clear_questions();
    sd_bus_flush_close_unref( user_bus );
    sd_bus_flush_close_unref( daemon_bus );
    sd_event_unref( event );
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
