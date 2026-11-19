/*
 * btrfs-snap-cpp --daemon: root system service.
 *
 * watches block devices (udev), config and fstab (inotify) and mounts
 * (libmount monitor); keeps one state machine per watch group:
 *
 *   absent -> present/mounted -> (question "connect")
 *       mount-backup / backup -> running -> (question "finish": unmount/keep)
 *       mount               -> mounted  -> (question "finish": unmount/keep)
 *       nothing
 *
 * questions are kept here and broadcast on the bus; any client (agent or
 * CLI) may answer, the first answer wins. backups run as transient system
 * units "btrfs-snap-cpp-backup-<group>.service" executing
 * "<this binary> -c <config> -g <group>".
 *
 * D-Bus: org.btrfssnapcpp.Daemon1 on the system bus; mutating methods are
 * authorized with polkit (org.btrfssnapcpp.manage), root callers always pass.
 *
 * author: alcubierre-drive
 * license: gpl-v3
 */

#include "config.hpp"
#include "ipc.hpp"
#include "modes.hpp"
#include "watchgroups.hpp"

#include <systemd/sd-bus.h>
#include <systemd/sd-device.h>
#include <systemd/sd-event.h>
#include <libmount/libmount.h>

#include <climits>
#include <csignal>
#include <cstdarg>
#include <functional>
#include <map>
#include <tuple>
#include <pwd.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace {

using namespace bsc;

const uint64_t DEBOUNCE_USEC = 500000ULL;
const uint64_t SLOW_CALL_USEC = 300 * USEC_PER_SEC;

bool verbose = false;
uint64_t question_timeout_usec = 0; // -T; 0: questions never expire
string config_file;
string fstab_file = "/etc/fstab";
string self_exe;

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

sd_event* event = nullptr;
sd_bus* bus = nullptr;
sd_event_source* debounce_src = nullptr;
bool reload_pending = false;

/* ---------------------------------------------------------------- devices */

struct dev_info {
    string devname;
    string fstype;
    vector<string> keys;
};
std::map<string, dev_info> devices; // by syspath

string lower( string s ) {
    for (auto& c: s) c = (char)tolower( (unsigned char)c );
    return s;
}

// undo udev's \xNN encoding (ID_FS_LABEL_ENC, ID_PART_ENTRY_NAME)
string udev_decode( const char* s ) {
    string out;
    for (size_t i=0; s[i]; ++i) {
        if (s[i] == '\\' && s[i+1] == 'x' && isxdigit( (unsigned char)s[i+2] ) &&
                isxdigit( (unsigned char)s[i+3] )) {
            out += (char)strtol( string( s+i+2, 2 ).c_str(), nullptr, 16 );
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

const char* prop( sd_device* d, const char* key ) {
    const char* v = nullptr;
    if (sd_device_get_property_value( d, key, &v ) < 0) return nullptr;
    return (v && v[0]) ? v : nullptr;
}

void update_device( sd_device* d, bool removed ) {
    const char* syspath = nullptr;
    if (sd_device_get_syspath( d, &syspath ) < 0) return;
    const char* ready = prop( d, "SYSTEMD_READY" );
    if (removed || (ready && !strcmp( ready, "0" ))) {
        if (devices.erase( syspath ))
            DBG( "device gone: %s", syspath );
        return;
    }
    dev_info info;
    const char* v;
    if (sd_device_get_devname( d, &v ) >= 0) {
        info.devname = v;
        info.keys.push_back( string( "dev:" ) + v );
    }
    for (const char* l = sd_device_get_devlink_first( d ); l; l = sd_device_get_devlink_next( d ))
        info.keys.push_back( string( "dev:" ) + l );
    if ((v = prop( d, "ID_FS_UUID" ))) info.keys.push_back( "uuid:" + lower( v ) );
    if ((v = prop( d, "ID_PART_ENTRY_UUID" ))) info.keys.push_back( "partuuid:" + lower( v ) );
    if ((v = prop( d, "ID_FS_LABEL_ENC" ))) info.keys.push_back( "label:" + udev_decode( v ) );
    if ((v = prop( d, "ID_PART_ENTRY_NAME" ))) info.keys.push_back( "partlabel:" + udev_decode( v ) );
    if ((v = prop( d, "ID_FS_TYPE" ))) info.fstype = v;
    if (verbose) {
        string k;
        for (auto& s: info.keys)
            if (s.rfind( "dev:/dev/disk/", 0 ) != 0) k += " " + s;
        DBG( "device: %s%s", info.devname.c_str(), k.c_str() );
    }
    devices[syspath] = std::move( info );
}

vector<const dev_info*> lookup( const string& key ) {
    vector<const dev_info*> out;
    for (auto& [path, info]: devices)
        for (auto& k: info.keys)
            if (k == key) { out.push_back( &info ); break; }
    return out;
}

/* ---------------------------------------------------------- groups & questions */

struct choice { string key, label; };

struct question {
    uint32_t id = 0;
    string group, kind, text, log;
    vector<choice> choices;
    string def;                       // non-intrusive default ("nothing" / "keep")
    sd_event_source* timer = nullptr; // expiry (-T), owned by the map entry
};
std::map<uint32_t, question> questions;
uint32_t next_question_id = 1;

enum class gstate { absent, present, mounted, running };

const char* state_name( gstate s ) {
    switch (s) {
        case gstate::absent: return "absent";
        case gstate::present: return "present";
        case gstate::mounted: return "mounted";
        case gstate::running: return "running";
    }
    return "?";
}

struct group_state {
    watch_group cfg;
    bool stale = false;     // removed from config while running
    gstate st = gstate::absent;
    bool asked = false;     // connect question asked since the devices appeared
    uint32_t question = 0;  // pending question id
    // backup run
    string unit;
    sd_bus_slot* match = nullptr;
    bool seen_active = false;
    string result;
    int32_t exec_code = 0, exec_status = 0;
    time_t started = 0;
};
std::map<string, group_state> groups; // by name; node addresses stay valid

void schedule_reconcile();

group_state* find_group( const string& name ) {
    auto it = groups.find( name );
    return it == groups.end() || it->second.stale ? nullptr : &it->second;
}

void set_state( group_state& g, gstate s ) {
    if (g.st == s) return;
    LOG( "group '%s': %s -> %s", g.cfg.name.c_str(), state_name( g.st ), state_name( s ) );
    g.st = s;
    sd_bus_emit_signal( bus, BSC_OBJ_PATH, BSC_IFACE, "GroupChanged", "ss", g.cfg.name.c_str(), state_name( s ) );
}

void emit_error( const string& group, const string& msg ) {
    LOG( "group '%s': error: %s", group.c_str(), msg.c_str() );
    sd_bus_emit_signal( bus, BSC_OBJ_PATH, BSC_IFACE, "Error", "ss", group.c_str(), msg.c_str() );
}

bool all_present( const group_state& g, vector<string>* devnodes ) {
    bool present = !g.cfg.devices.empty();
    for (auto& d: g.cfg.devices) {
        auto found = lookup( d.key );
        if (found.empty()) { present = false; continue; }
        bool all_btrfs = true;
        for (auto* f: found) {
            if (devnodes) devnodes->push_back( f->devname );
            all_btrfs &= f->fstype == "btrfs";
        }
        if (found.size() > 1 && !all_btrfs && devnodes)
            LOG( "group '%s': %s matches %zu devices", g.cfg.name.c_str(), d.spec.c_str(), found.size() );
    }
    return present;
}

bool all_mounted( const group_state& g ) {
    for (auto& mp: g.cfg.mountpoints)
        if (!watch_is_mounted( mp )) return false;
    return true;
}

bool any_mounted( const group_state& g ) {
    for (auto& mp: g.cfg.mountpoints)
        if (watch_is_mounted( mp )) return true;
    return false;
}

string mount_lines( const group_state& g ) {
    string s;
    for (auto& mp: g.cfg.mountpoints)
        s += "\n" + mp + (watch_is_mounted( mp ) ? " (mounted)" : " (not mounted)");
    return s;
}

int append_question( sd_bus_message* m, const question& q ) {
    int r = sd_bus_message_append( m, "uss", q.id, q.group.c_str(), q.kind.c_str() );
    if (r >= 0) r = sd_bus_message_open_container( m, 'a', "(ss)" );
    for (auto& c: q.choices)
        if (r >= 0) r = sd_bus_message_append( m, "(ss)", c.key.c_str(), c.label.c_str() );
    if (r >= 0) r = sd_bus_message_close_container( m );
    if (r >= 0) r = sd_bus_message_append( m, "sss", q.def.c_str(), q.text.c_str(), q.log.c_str() );
    return r;
}

string question_kind( const group_state& g ) {
    auto it = questions.find( g.question );
    return it == questions.end() ? "" : it->second.kind;
}

void resolve( uint32_t id, const string& answer, const string& by ) {
    auto it = questions.find( id );
    if (it == questions.end()) return;
    sd_event_source_disable_unref( it->second.timer );
    questions.erase( it );
    for (auto& [name, g]: groups)
        if (g.question == id) g.question = 0;
    DBG( "question #%u resolved: '%s' by %s", id, answer.c_str(), by.c_str() );
    sd_bus_emit_signal( bus, BSC_OBJ_PATH, BSC_IFACE, "QuestionResolved", "uss", id, answer.c_str(), by.c_str() );
}

void withdraw( group_state& g ) {
    if (g.question) resolve( g.question, "", "withdrawn" );
}

// an unanswered question (expired or not) never triggers an action: expiry
// just resolves it with its default, and the defaults are the no-op choices
int on_question_timeout( sd_event_source*, uint64_t, void* userdata ) {
    uint32_t id = (uint32_t)(uintptr_t)userdata;
    auto it = questions.find( id );
    if (it == questions.end()) return 0;
    string def = it->second.def;
    LOG( "group '%s': question #%u unanswered, using default '%s'", it->second.group.c_str(), id, def.c_str() );
    resolve( id, def, "timeout" );
    return 0;
}

// broadcast q; agents show it unless they already do
void send_question( const question& q ) {
    sd_bus_message* m = nullptr;
    int r = sd_bus_message_new_signal( bus, &m, BSC_OBJ_PATH, BSC_IFACE, "Question" );
    if (r >= 0) r = append_question( m, q );
    if (r >= 0) r = sd_bus_send( bus, m, nullptr );
    if (r < 0) LOG( "cannot send question: %s", strerror( -r ) );
    sd_bus_message_unref( m );
}

// choices[0] is the default; it must be a choice that does nothing
void ask( group_state& g, const string& kind, const string& text, vector<choice> choices,
        const string& log = "" ) {
    withdraw( g );
    question q;
    q.id = next_question_id++;
    q.group = g.cfg.name;
    q.kind = kind;
    q.text = text;
    q.log = log;
    q.choices = std::move( choices );
    q.def = q.choices.empty() ? "" : q.choices[0].key;
    questions[q.id] = q;
    g.question = q.id;
    if (question_timeout_usec &&
            sd_event_add_time_relative( event, &questions[q.id].timer, CLOCK_MONOTONIC,
                question_timeout_usec, USEC_PER_SEC, on_question_timeout, (void*)(uintptr_t)q.id ) < 0)
        LOG( "cannot arm question timeout" );
    LOG( "group '%s': question #%u (%s)", g.cfg.name.c_str(), q.id, kind.c_str() );
    send_question( q );
}

void ask_connect( group_state& g ) {
    string text = "Backup disks of '" + g.cfg.name + "' connected.\nSections: " +
        join( g.cfg.sections, ", " ) + mount_lines( g );
    if (g.cfg.mountpoints.empty() || all_mounted( g ))
        ask( g, "connect", text, { { "nothing", "Nothing" }, { "backup", "Backup" } } );
    else
        ask( g, "connect", text, { { "nothing", "Nothing" }, { "mount", "Mount only" },
                { "mount-backup", "Mount + backup" } } );
}

void ask_unmount( group_state& g, const string& text, const string& log = "" ) {
    if (!any_mounted( g )) return;
    ask( g, "finish", text + "\nUnmount " + join( g.cfg.mountpoints, " " ) + "?",
            { { "keep", "Keep mounted" }, { "unmount", "Unmount" } }, log );
}

/* ---------------------------------------------------------- mount / umount */

bool do_mount( group_state& g, string& err ) {
    struct libmnt_table* tb = mnt_new_table();
    if (!tb || mnt_table_parse_fstab( tb, fstab_file.c_str() )) {
        err = "cannot parse " + fstab_file;
        mnt_unref_table( tb );
        return false;
    }
    vector<string> done;
    for (auto& mp: g.cfg.mountpoints) {
        if (watch_is_mounted( mp )) continue;
        struct libmnt_context* cxt = mnt_new_context();
        mnt_context_set_fstab( cxt, tb );
        mnt_context_set_target( cxt, mp.c_str() );
        if (!mnt_table_find_target( tb, mp.c_str(), MNT_ITER_FORWARD )) {
            // not in fstab: mount the group's only device there
            const dev_info* dev = nullptr;
            if (g.cfg.devices.size() == 1) {
                auto found = lookup( g.cfg.devices[0].key );
                if (!found.empty()) dev = found[0];
            }
            if (!dev) {
                err = mp + " is not in fstab and the group has no single device to mount there";
                mnt_free_context( cxt );
                break;
            }
            mnt_context_set_source( cxt, dev->devname.c_str() );
        }
        LOG( "group '%s': mounting %s", g.cfg.name.c_str(), mp.c_str() );
        int rc = mnt_context_mount( cxt );
        int ex = 0;
        char buf[BUFSIZ] = { 0 };
        if (rc || mnt_context_get_status( cxt ) != 1) {
            ex = mnt_context_get_excode( cxt, rc, buf, sizeof(buf) );
            if (ex == 0 && rc == 0) ex = -1;
        }
        mnt_free_context( cxt );
        if (ex) {
            err = "mount " + mp + ": " + (buf[0] ? buf : "failed");
            break;
        }
        done.push_back( mp );
    }
    mnt_unref_table( tb );
    if (!err.empty()) {
        // don't leave half of the group mounted
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            struct libmnt_context* cxt = mnt_new_context();
            mnt_context_set_target( cxt, it->c_str() );
            mnt_context_umount( cxt );
            mnt_free_context( cxt );
        }
        return false;
    }
    return true;
}

bool do_umount( group_state& g, string& err ) {
    for (auto it = g.cfg.mountpoints.rbegin(); it != g.cfg.mountpoints.rend(); ++it) {
        if (!watch_is_mounted( *it )) continue;
        LOG( "group '%s': unmounting %s", g.cfg.name.c_str(), it->c_str() );
        struct libmnt_context* cxt = mnt_new_context();
        mnt_context_set_target( cxt, it->c_str() );
        int rc = mnt_context_umount( cxt );
        char buf[BUFSIZ] = { 0 };
        int ex = rc ? mnt_context_get_excode( cxt, rc, buf, sizeof(buf) ) : 0;
        mnt_free_context( cxt );
        if (ex) {
            err = "umount " + *it + ": " + (buf[0] ? buf : "failed");
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------- backup run */

void finish( group_state& g, const string& error ) {
    if (g.st != gstate::running) return;
    g.match = sd_bus_slot_unref( g.match );

    if (error.empty() && g.result.empty()) {
        char* path = nullptr;
        if (sd_bus_path_encode( "/org/freedesktop/systemd1/unit", g.unit.c_str(), &path ) >= 0) {
            char* res = nullptr;
            if (sd_bus_get_property_string( bus, "org.freedesktop.systemd1", path,
                        "org.freedesktop.systemd1.Service", "Result", nullptr, &res ) >= 0 && res)
                g.result = res;
            free( res );
            free( path );
        }
    }
    sd_bus_call_method_async( bus, nullptr, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "UnrefUnit", nullptr, nullptr, "s", g.unit.c_str() );

    bool ok = error.empty() && (g.result == "success" || (g.result.empty() && g.exec_status == 0));
    string summary;
    if (!error.empty()) {
        summary = "Backup '" + g.cfg.name + "' could not start: " + error;
    } else {
        summary = "Backup '" + g.cfg.name + (ok ? "' finished" : "' failed") + " after " +
            duration( (long)(time( nullptr ) - g.started) ) + ".";
        if (!ok) {
            summary += " Result: " + (g.result.empty() ? string( "unknown" ) : g.result);
            if (g.exec_code == 1) summary += ", exit status " + std::to_string( g.exec_status );
            else if (g.exec_code > 1) summary += ", signal " + std::to_string( g.exec_status );
            summary += ".";
        }
    }
    string log = string( "journalctl " ) + (session_mode() ? "--user " : "") + "-u " +
        shell_quote( g.unit ) + " --since @" + std::to_string( (long long)g.started - 1 );
    LOG( "%s (%s)", summary.c_str(), log.c_str() );
    sd_bus_emit_signal( bus, BSC_OBJ_PATH, BSC_IFACE, "BackupFinished", "sbss", g.cfg.name.c_str(),
            (int)ok, summary.c_str(), log.c_str() );

    if (g.stale) {
        groups.erase( string( g.cfg.name ) ); // invalidates g
        schedule_reconcile();
        return;
    }
    set_state( g, any_mounted( g ) ? gstate::mounted : gstate::present );
    ask_unmount( g, summary, log );
    schedule_reconcile(); // devices may have gone meanwhile
}

int on_unit_props( sd_bus_message* m, void* userdata, sd_bus_error* ) {
    group_state& g = *(group_state*)userdata;
    const char* iface = nullptr;
    if (sd_bus_message_read( m, "s", &iface ) < 0) return 0;
    string active_state;
    if (sd_bus_message_enter_container( m, 'a', "{sv}" ) < 0) return 0;
    while (sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
        const char* key = nullptr;
        const char* s = nullptr;
        if (sd_bus_message_read( m, "s", &key ) < 0) return 0;
        if (!strcmp( key, "ActiveState" ) && sd_bus_message_read( m, "v", "s", &s ) >= 0) active_state = s;
        else if (!strcmp( key, "Result" ) && sd_bus_message_read( m, "v", "s", &s ) >= 0) g.result = s;
        else if (!strcmp( key, "ExecMainCode" )) sd_bus_message_read( m, "v", "i", &g.exec_code );
        else if (!strcmp( key, "ExecMainStatus" )) sd_bus_message_read( m, "v", "i", &g.exec_status );
        else sd_bus_message_skip( m, "v" );
        sd_bus_message_exit_container( m );
    }
    if (active_state.empty()) return 0;
    DBG( "%s: %s %s", g.unit.c_str(), iface, active_state.c_str() );
    if (active_state == "activating" || active_state == "active" || active_state == "deactivating")
        g.seen_active = true;
    else if ((active_state == "inactive" || active_state == "failed") && g.seen_active)
        finish( g, "" );
    return 0;
}

int on_start_reply( sd_bus_message* m, void* userdata, sd_bus_error* ) {
    group_state& g = *(group_state*)userdata;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    if (e) finish( g, bus_error_text( e, 0 ) );
    return 0;
}

bool start_backup( group_state& g, string& err ) {
    if (g.cfg.sections.empty()) {
        err = "group has no sections with 'watch = " + g.cfg.name + "'";
        return false;
    }
    g.unit = backup_unit_name( g.cfg.name );
    g.seen_active = false;
    g.result.clear();
    g.exec_code = g.exec_status = 0;
    g.started = time( nullptr );

    char* path = nullptr;
    int r = sd_bus_path_encode( "/org/freedesktop/systemd1/unit", g.unit.c_str(), &path );
    if (r >= 0)
        r = sd_bus_match_signal( bus, &g.match, "org.freedesktop.systemd1", path,
                "org.freedesktop.DBus.Properties", "PropertiesChanged", on_unit_props, &g );
    free( path );
    if (r < 0) {
        err = string( "cannot watch unit: " ) + strerror( -r );
        return false;
    }

    vector<const char*> argv = { self_exe.c_str(), "-c", config_file.c_str(), "-g", g.cfg.name.c_str(), nullptr };
    string desc = "btrfs-snap-cpp backup of watch group " + g.cfg.name;
    sd_bus_message* m = nullptr;
    r = sd_bus_message_new_method_call( bus, &m, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "StartTransientUnit" );
    if (r >= 0) r = sd_bus_message_append( m, "ss", g.unit.c_str(), "fail" );
    if (r >= 0) r = sd_bus_message_open_container( m, 'a', "(sv)" );
    if (r >= 0) r = sd_bus_message_append( m, "(sv)(sv)(sv)(sv)",
            "Description", "s", desc.c_str(),
            "Type", "s", "exec",
            "CollectMode", "s", "inactive-or-failed",
            "AddRef", "b", 1 );
    if (r >= 0) r = sd_bus_message_open_container( m, 'r', "sv" );
    if (r >= 0) r = sd_bus_message_append( m, "s", "ExecStart" );
    if (r >= 0) r = sd_bus_message_open_container( m, 'v', "a(sasb)" );
    if (r >= 0) r = sd_bus_message_open_container( m, 'a', "(sasb)" );
    if (r >= 0) r = sd_bus_message_open_container( m, 'r', "sasb" );
    if (r >= 0) r = sd_bus_message_append( m, "s", self_exe.c_str() );
    if (r >= 0) r = sd_bus_message_append_strv( m, (char**)argv.data() );
    if (r >= 0) r = sd_bus_message_append( m, "b", 0 );
    for (int i=0; i<4 && r >= 0; ++i) r = sd_bus_message_close_container( m );
    if (r >= 0) r = sd_bus_message_close_container( m ); // a(sv)
    if (r >= 0) r = sd_bus_message_append( m, "a(sa(sv))", 0 );
    if (r >= 0) r = sd_bus_call_async( bus, nullptr, m, on_start_reply, &g, SLOW_CALL_USEC );
    sd_bus_message_unref( m );
    if (r < 0) {
        g.match = sd_bus_slot_unref( g.match );
        err = string( "cannot call systemd: " ) + strerror( -r );
        return false;
    }
    LOG( "group '%s': starting %s", g.cfg.name.c_str(), g.unit.c_str() );
    set_state( g, gstate::running );
    return true;
}

/* -------------------------------------------------------------- reconcile */

void reload() {
    vector<watch_group> cfg;
    if (load_watch_groups( config_file, fstab_file, cfg )) {
        LOG( "config reload failed, keeping previous configuration" );
        return;
    }
    for (auto it = groups.begin(); it != groups.end(); ) {
        if (find_watch_group( cfg, it->first )) { ++it; continue; }
        withdraw( it->second );
        if (it->second.st == gstate::running) { it->second.stale = true; ++it; continue; }
        it = groups.erase( it );
    }
    for (auto& c: cfg) {
        group_state& g = groups[c.name];
        g.cfg = c;
        g.stale = false;
        string devs;
        for (auto& d: c.devices)
            devs += " " + d.spec + " [" + d.source + "]";
        LOG( "group '%s': sections [%s], devices:%s, mount: %s", c.name.c_str(),
                join( c.sections, ", " ).c_str(), devs.c_str(), join( c.mountpoints, " " ).c_str() );
    }
}

void reconcile() {
    if (reload_pending) {
        reload_pending = false;
        reload();
    }
    for (auto& [name, g]: groups) {
        if (g.stale || g.st == gstate::running) continue;
        vector<string> devnodes;
        if (!all_present( g, &devnodes )) {
            withdraw( g );
            g.asked = false;
            set_state( g, gstate::absent );
            continue;
        }
        bool mounted = !g.cfg.mountpoints.empty() && all_mounted( g );
        if (g.st == gstate::absent) {
            DBG( "group '%s': devices present: %s", name.c_str(), join( devnodes, " " ).c_str() );
            set_state( g, mounted ? gstate::mounted : gstate::present );
        } else if (g.st == gstate::mounted && !any_mounted( g )) {
            // unmounted behind our back: an unmount question is moot
            if (question_kind( g ) == "finish") withdraw( g );
            set_state( g, gstate::present );
        } else if (g.st == gstate::present && mounted) {
            set_state( g, gstate::mounted );
        }
        if (!g.asked) {
            g.asked = true;
            ask_connect( g );
        }
    }
}

int on_debounce( sd_event_source*, uint64_t, void* ) {
    reconcile();
    return 0;
}

void schedule_reconcile() {
    int r;
    if (!debounce_src) {
        r = sd_event_add_time_relative( event, &debounce_src, CLOCK_MONOTONIC, DEBOUNCE_USEC, 0,
                on_debounce, nullptr );
    } else {
        r = sd_event_source_set_time_relative( debounce_src, DEBOUNCE_USEC );
        if (r >= 0) r = sd_event_source_set_enabled( debounce_src, SD_EVENT_ONESHOT );
    }
    if (r < 0) LOG( "cannot arm timer: %s", strerror( -r ) );
}

/* ------------------------------------------------------------ D-Bus API */

using action_fn = std::function<int( sd_bus_message*, sd_bus_error* )>;

struct auth_request {
    sd_bus_message* m;
    action_fn fn;
};

int on_polkit_reply( sd_bus_message* reply, void* userdata, sd_bus_error* ) {
    auth_request* req = (auth_request*)userdata;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int authorized = 0, challenge = 0;
    if (const sd_bus_error* pe = sd_bus_message_get_error( reply )) {
        sd_bus_reply_method_errorf( req->m, SD_BUS_ERROR_ACCESS_DENIED, "polkit: %s", bus_error_text( pe, 0 ) );
    } else if (sd_bus_message_enter_container( reply, 'r', "bba{ss}" ) < 0 ||
            sd_bus_message_read( reply, "bb", &authorized, &challenge ) < 0 || !authorized) {
        sd_bus_reply_method_errorf( req->m, SD_BUS_ERROR_ACCESS_DENIED,
                "not authorized (polkit action " BSC_POLKIT_ACTION ")" );
    } else {
        int r = req->fn( req->m, &e );
        if (r < 0) {
            if (sd_bus_error_is_set( &e )) sd_bus_reply_method_error( req->m, &e );
            else sd_bus_reply_method_errno( req->m, r, nullptr );
        }
    }
    sd_bus_error_free( &e );
    sd_bus_message_unref( req->m );
    delete req;
    return 0;
}

uid_t sender_uid( sd_bus_message* m ) {
    sd_bus_creds* c = nullptr;
    uid_t uid = (uid_t)-1;
    // unknown uid -> (uid_t)-1, which never skips the polkit check
    if (sd_bus_query_sender_creds( m, SD_BUS_CREDS_EUID | SD_BUS_CREDS_UID, &c ) >= 0)
        if (sd_bus_creds_get_euid( c, &uid ) < 0)
            sd_bus_creds_get_uid( c, &uid );
    sd_bus_creds_unref( c );
    return uid;
}

string sender_name( sd_bus_message* m ) {
    uid_t uid = sender_uid( m );
    if (uid == (uid_t)-1) return "unknown";
    if (struct passwd* pw = getpwuid( uid )) return pw->pw_name;
    return "uid " + std::to_string( uid );
}

// run fn now (root caller, session mode) or after polkit said yes
int authorize( sd_bus_message* m, sd_bus_error* ret_error, action_fn fn ) {
    if (session_mode() || sender_uid( m ) == 0)
        return fn( m, ret_error );
    sd_bus_message* call = nullptr;
    int r = sd_bus_message_new_method_call( bus, &call, "org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority", "org.freedesktop.PolicyKit1.Authority",
            "CheckAuthorization" );
    if (r >= 0) r = sd_bus_message_append( call, "(sa{sv})s", "system-bus-name", 1, "name", "s",
            sd_bus_message_get_sender( m ), BSC_POLKIT_ACTION );
    if (r >= 0) r = sd_bus_message_append( call, "a{ss}us", 0,
            sd_bus_message_get_allow_interactive_authorization( m ) ? 1u : 0u, "" );
    auth_request* req = new auth_request{ sd_bus_message_ref( m ), std::move( fn ) };
    if (r >= 0) r = sd_bus_call_async( bus, nullptr, call, on_polkit_reply, req, SLOW_CALL_USEC );
    sd_bus_message_unref( call );
    if (r < 0) {
        sd_bus_message_unref( req->m );
        delete req;
        return sd_bus_error_set_errnof( ret_error, -r, "cannot ask polkit: %s", strerror( -r ) );
    }
    return 1; // reply comes from on_polkit_reply
}

int append_strv( sd_bus_message* m, const vector<string>& v ) {
    vector<const char*> p;
    for (auto& s: v) p.push_back( s.c_str() );
    p.push_back( nullptr );
    return sd_bus_message_append_strv( m, (char**)p.data() );
}

int m_list_groups( sd_bus_message* m, void*, sd_bus_error* ) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return( m, &reply );
    if (r >= 0) r = sd_bus_message_open_container( reply, 'a', "(ssuasasas)" );
    for (auto& [name, g]: groups) {
        if (g.stale) continue;
        vector<string> devs, mps;
        for (auto& d: g.cfg.devices)
            devs.push_back( d.spec + " [" + d.source + "] " + (lookup( d.key ).empty() ? "missing" : "present") );
        for (auto& mp: g.cfg.mountpoints)
            mps.push_back( mp + (watch_is_mounted( mp ) ? " mounted" : " not mounted") );
        if (r >= 0) r = sd_bus_message_open_container( reply, 'r', "ssuasasas" );
        if (r >= 0) r = sd_bus_message_append( reply, "ssu", name.c_str(), state_name( g.st ), g.question );
        if (r >= 0) r = append_strv( reply, g.cfg.sections );
        if (r >= 0) r = append_strv( reply, devs );
        if (r >= 0) r = append_strv( reply, mps );
        if (r >= 0) r = sd_bus_message_close_container( reply );
    }
    if (r >= 0) r = sd_bus_message_close_container( reply );
    if (r >= 0) r = sd_bus_send( nullptr, reply, nullptr );
    sd_bus_message_unref( reply );
    return r;
}

int m_get_questions( sd_bus_message* m, void*, sd_bus_error* ) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return( m, &reply );
    if (r >= 0) r = sd_bus_message_open_container( reply, 'a', "(ussa(ss)sss)" );
    for (auto& [id, q]: questions) {
        if (r >= 0) r = sd_bus_message_open_container( reply, 'r', "ussa(ss)sss" );
        if (r >= 0) r = append_question( reply, q );
        if (r >= 0) r = sd_bus_message_close_container( reply );
    }
    if (r >= 0) r = sd_bus_message_close_container( reply );
    if (r >= 0) r = sd_bus_send( nullptr, reply, nullptr );
    sd_bus_message_unref( reply );
    return r;
}

#define ERR_FAILED "org.btrfssnapcpp.Error.Failed"

// reply error + Error signal (so agents can show it)
int fail( sd_bus_error* e, const string& group, const string& msg ) {
    emit_error( group, msg );
    return sd_bus_error_setf( e, ERR_FAILED, "%s", msg.c_str() );
}

group_state* group_arg( sd_bus_message* m, sd_bus_error* e, const char** name ) {
    int r = sd_bus_message_read( m, "s", name );
    if (r < 0) { sd_bus_error_set_errno( e, r ); return nullptr; }
    group_state* g = find_group( *name );
    if (!g) sd_bus_error_setf( e, SD_BUS_ERROR_INVALID_ARGS, "no watch group '%s'", *name );
    return g;
}

int act_mount( group_state& g, sd_bus_error* e ) {
    if (g.st == gstate::running) return fail( e, g.cfg.name, "backup running" );
    if (!all_present( g, nullptr )) return fail( e, g.cfg.name, "devices not connected" );
    if (g.cfg.mountpoints.empty()) return fail( e, g.cfg.name, "group has no mountpoints" );
    string err;
    if (!do_mount( g, err )) return fail( e, g.cfg.name, err );
    if (question_kind( g ) == "connect") withdraw( g );
    set_state( g, gstate::mounted );
    return 0;
}

int act_umount( group_state& g, sd_bus_error* e ) {
    if (g.st == gstate::running) return fail( e, g.cfg.name, "backup running" );
    if (question_kind( g ) == "finish") withdraw( g );
    string err;
    if (!do_umount( g, err )) {
        ask_unmount( g, "Unmounting failed: " + err );
        return fail( e, g.cfg.name, err );
    }
    set_state( g, all_present( g, nullptr ) ? gstate::present : gstate::absent );
    return 0;
}

int act_backup( group_state& g, sd_bus_error* e ) {
    if (g.st == gstate::running) return fail( e, g.cfg.name, "backup already running" );
    if (!all_present( g, nullptr )) return fail( e, g.cfg.name, "devices not connected" );
    string err;
    if (!g.cfg.mountpoints.empty() && !do_mount( g, err )) return fail( e, g.cfg.name, err );
    if (!start_backup( g, err )) return fail( e, g.cfg.name, err );
    withdraw( g );
    return 0;
}

// Ask: the group's pending question once more, else the question for its
// state (present: connect, mounted: unmount). Like every question, it does
// nothing unless answered. false + why if there is nothing to ask.
bool ask_again( group_state& g, string& what ) {
    auto it = questions.find( g.question );
    if (it != questions.end()) {
        send_question( it->second );
        what = "question #" + std::to_string( g.question ) + " (" + it->second.kind + ") shown again";
        return true;
    }
    if (g.st == gstate::running) { what = "backup running"; return false; }
    if (!all_present( g, nullptr )) { what = "devices not connected"; return false; }
    if (g.st == gstate::mounted && any_mounted( g ))
        ask_unmount( g, "Mounted " + join( g.cfg.mountpoints, " " ) + "." );
    else
        ask_connect( g );
    if (!g.question) { what = "nothing to ask"; return false; }
    what = "question #" + std::to_string( g.question ) + " (" + question_kind( g ) + ") asked";
    return true;
}

// Ask(s group) -> a(sbs) (group, asked, what); "" = every group. Needs no
// authorization: a question only offers the actions, answering is checked.
int m_ask( sd_bus_message* m, void*, sd_bus_error* e ) {
    const char* name;
    int r = sd_bus_message_read( m, "s", &name );
    if (r < 0) return r;
    LOG( "ask %s requested by %s", name[0] ? name : "(all groups)", sender_name( m ).c_str() );
    vector<std::tuple<string, bool, string>> out;
    if (name[0]) {
        group_state* g = find_group( name );
        if (!g) return sd_bus_error_setf( e, SD_BUS_ERROR_INVALID_ARGS, "no watch group '%s'", name );
        string what;
        if (!ask_again( *g, what )) return sd_bus_error_setf( e, ERR_FAILED, "%s: %s", name, what.c_str() );
        out.emplace_back( name, true, what );
    } else {
        for (auto& [n, g]: groups) {
            if (g.stale) continue;
            string what;
            bool ok = ask_again( g, what );
            out.emplace_back( n, ok, what );
        }
    }
    sd_bus_message* reply = nullptr;
    r = sd_bus_message_new_method_return( m, &reply );
    if (r >= 0) r = sd_bus_message_open_container( reply, 'a', "(sbs)" );
    for (auto& [n, ok, what]: out)
        if (r >= 0) r = sd_bus_message_append( reply, "(sbs)", n.c_str(), (int)ok, what.c_str() );
    if (r >= 0) r = sd_bus_message_close_container( reply );
    if (r >= 0) r = sd_bus_send( nullptr, reply, nullptr );
    sd_bus_message_unref( reply );
    return r;
}

int reply_ok( sd_bus_message* m, int r ) {
    return r < 0 ? r : sd_bus_reply_method_return( m, "" );
}

int m_answer( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* e ) -> int {
        uint32_t id;
        const char* ch;
        int r = sd_bus_message_read( m, "us", &id, &ch );
        if (r < 0) return r;
        auto it = questions.find( id );
        if (it == questions.end())
            return sd_bus_error_setf( e, SD_BUS_ERROR_INVALID_ARGS, "no pending question #%u", id );
        question q = it->second;
        bool valid = false;
        for (auto& c: q.choices) valid |= c.key == ch;
        if (!valid)
            return sd_bus_error_setf( e, SD_BUS_ERROR_INVALID_ARGS, "'%s' is not a choice of question #%u", ch, id );
        string answer = ch;
        resolve( id, answer, sender_name( m ) );
        group_state* g = find_group( q.group );
        if (!g) return sd_bus_error_setf( e, ERR_FAILED, "group '%s' is gone", q.group.c_str() );
        LOG( "group '%s': answer '%s'", q.group.c_str(), answer.c_str() );
        if (answer == "mount") {
            r = act_mount( *g, e );
            if (r >= 0)
                ask_unmount( *g, "Mounted " + join( g->cfg.mountpoints, " " ) + "." );
        } else if (answer == "mount-backup" || answer == "backup") {
            r = act_backup( *g, e );
        } else if (answer == "unmount") {
            r = act_umount( *g, e );
        } else {
            r = 0; // nothing / keep
        }
        // a failed connect action: ask again instead of leaving the group
        // without any pending question
        if (r < 0 && q.kind == "connect" && g->st != gstate::running && all_present( *g, nullptr ))
            ask_connect( *g );
        return reply_ok( m, r );
    } );
}

int m_mount( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* e ) -> int {
        const char* name;
        group_state* g = group_arg( m, e, &name );
        if (!g) return -EINVAL;
        return reply_ok( m, act_mount( *g, e ) );
    } );
}

int m_unmount( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* e ) -> int {
        const char* name;
        group_state* g = group_arg( m, e, &name );
        if (!g) return -EINVAL;
        return reply_ok( m, act_umount( *g, e ) );
    } );
}

int m_backup( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* e ) -> int {
        const char* name;
        group_state* g = group_arg( m, e, &name );
        if (!g) return -EINVAL;
        return reply_ok( m, act_backup( *g, e ) );
    } );
}

int m_stop( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* e ) -> int {
        const char* name;
        group_state* g = group_arg( m, e, &name );
        if (!g) return -EINVAL;
        if (g->st != gstate::running)
            return sd_bus_error_setf( e, ERR_FAILED, "no backup running for '%s'", name );
        LOG( "group '%s': stopping %s", name, g->unit.c_str() );
        sd_bus_call_method_async( bus, nullptr, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                "org.freedesktop.systemd1.Manager", "StopUnit", nullptr, nullptr, "ss", g->unit.c_str(), "replace" );
        return reply_ok( m, 0 );
    } );
}

int m_reload( sd_bus_message* m, void*, sd_bus_error* ret_error ) {
    return authorize( m, ret_error, []( sd_bus_message* m, sd_bus_error* ) -> int {
        reload();
        reconcile();
        return reply_ok( m, 0 );
    } );
}

const sd_bus_vtable daemon_vtable[] = {
    SD_BUS_VTABLE_START( 0 ),
    SD_BUS_METHOD( "ListGroups", "", "a(ssuasasas)", m_list_groups, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "GetQuestions", "", "a(ussa(ss)sss)", m_get_questions, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Answer", "us", "", m_answer, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Mount", "s", "", m_mount, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Unmount", "s", "", m_unmount, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Backup", "s", "", m_backup, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Stop", "s", "", m_stop, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Reload", "", "", m_reload, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_METHOD( "Ask", "s", "a(sbs)", m_ask, SD_BUS_VTABLE_UNPRIVILEGED ),
    SD_BUS_SIGNAL( "Question", "ussa(ss)sss", 0 ),
    SD_BUS_SIGNAL( "QuestionResolved", "uss", 0 ),
    SD_BUS_SIGNAL( "GroupChanged", "ss", 0 ),
    SD_BUS_SIGNAL( "BackupFinished", "sbss", 0 ),
    SD_BUS_SIGNAL( "Error", "ss", 0 ),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------- sources */

int on_uevent( sd_device_monitor*, sd_device* d, void* ) {
    sd_device_action_t action;
    bool removed = sd_device_get_action( d, &action ) >= 0 && action == SD_DEVICE_REMOVE;
    update_device( d, removed );
    schedule_reconcile();
    return 0;
}

int on_inotify( sd_event_source*, const struct inotify_event* ev, void* ) {
    if (!ev->len) return 0;
    for (const string* f: { &config_file, &fstab_file }) {
        size_t slash = f->rfind( '/' );
        string base = slash == string::npos ? *f : f->substr( slash+1 );
        if (base == ev->name) {
            DBG( "%s changed", f->c_str() );
            reload_pending = true;
            schedule_reconcile();
        }
    }
    return 0;
}

int watch_dir_of( const string& file ) {
    size_t slash = file.rfind( '/' );
    string dir = slash == string::npos ? "." : (slash == 0 ? "/" : file.substr( 0, slash ));
    return sd_event_add_inotify( event, nullptr, dir.c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM, on_inotify, nullptr );
}

int on_mount_change( sd_event_source*, int, uint32_t, void* userdata ) {
    struct libmnt_monitor* mn = (struct libmnt_monitor*)userdata;
    const char* file;
    int type;
    while (mnt_monitor_next_change( mn, &file, &type ) == 0) {}
    schedule_reconcile();
    return 0;
}

void print_help( const char* prog ) {
    printf( "usage: %s --daemon [options]\n"
            "watch backup disks and ask what to do when a watch group is connected\n"
            "options: -h              show this help\n"
            "         -c <file>       config file (default %s)\n"
            "         -f <file>       fstab (default /etc/fstab)\n"
            "         -T <seconds>    answer unanswered questions with their default\n"
            "                         ('nothing' / 'keep mounted') after <seconds>;\n"
            "                         default: never expire. unanswered questions\n"
            "                         never mount, back up or unmount anything\n"
            "         -v              verbose\n"
            "BTRFS_SNAP_CPP_BUS=session: use the session bus, no polkit (testing)\n",
            prog, DEFAULT_CONFIG_FILE );
}

#define CHECK( expr, what ) do { int _r = (expr); if (_r < 0) { \
        LOG( "%s: %s", what, strerror( -_r ) ); return EXIT_FAILURE; } } while (0)

} // namespace

int daemon_main( int argc, char** argv ) {
    config_file = default_config_file();
    int opt;
    optind = 1;
    while ((opt = getopt( argc, argv, "hvc:f:T:" )) != -1) {
        switch (opt) {
            case 'T': question_timeout_usec = strtoull( optarg, nullptr, 10 ) * USEC_PER_SEC; break;
            case 'h': print_help( "btrfs-snap-cpp" ); return EXIT_SUCCESS;
            case 'v': verbose = true; break;
            case 'c': config_file = optarg; break;
            case 'f': fstab_file = optarg; break;
            default: print_help( "btrfs-snap-cpp" ); return EXIT_FAILURE;
        }
    }
    setvbuf( stderr, nullptr, _IOLBF, 0 );
    {
        char buf[PATH_MAX];
        ssize_t n = readlink( "/proc/self/exe", buf, sizeof(buf)-1 );
        if (n <= 0) { LOG( "cannot resolve own executable" ); return EXIT_FAILURE; }
        self_exe.assign( buf, n );
        char* abs = realpath( config_file.c_str(), nullptr );
        if (abs) { config_file = abs; free( abs ); }
    }
    if (!session_mode() && geteuid() != 0)
        LOG( "warning: not running as root; mounting and backups will fail" );

    sigset_t ss;
    sigemptyset( &ss );
    sigaddset( &ss, SIGTERM );
    sigaddset( &ss, SIGINT );
    sigprocmask( SIG_BLOCK, &ss, nullptr );

    CHECK( sd_event_default( &event ), "sd_event_default" );
    CHECK( sd_event_add_signal( event, nullptr, SIGTERM, nullptr, nullptr ), "signal" );
    CHECK( sd_event_add_signal( event, nullptr, SIGINT, nullptr, nullptr ), "signal" );

    CHECK( open_daemon_bus( &bus ), "cannot connect to bus" );
    CHECK( sd_bus_add_object_vtable( bus, nullptr, BSC_OBJ_PATH, BSC_IFACE, daemon_vtable, nullptr ), "vtable" );
    CHECK( sd_bus_request_name( bus, BSC_BUS_NAME, 0 ), "cannot own " BSC_BUS_NAME
            " (already running? missing D-Bus policy?)" );
    CHECK( sd_bus_attach_event( bus, event, 0 ), "attach bus" );
    // systemd only emits unit change signals while somebody is subscribed
    sd_bus_call_method_async( bus, nullptr, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "Subscribe", nullptr, nullptr, "" );

    // monitor first, then enumerate, so nothing falls in between
    sd_device_monitor* mon = nullptr;
    CHECK( sd_device_monitor_new( &mon ), "udev monitor" );
    CHECK( sd_device_monitor_filter_add_match_subsystem_devtype( mon, "block", nullptr ), "udev filter" );
    CHECK( sd_device_monitor_attach_event( mon, event ), "udev attach" );
    CHECK( sd_device_monitor_start( mon, on_uevent, nullptr ), "udev start" );
    sd_device_enumerator* en = nullptr;
    CHECK( sd_device_enumerator_new( &en ), "enumerator" );
    CHECK( sd_device_enumerator_add_match_subsystem( en, "block", 1 ), "enumerator" );
    if (session_mode()) // test setups (containers) may have no udev at all
        sd_device_enumerator_allow_uninitialized( en );
    for (sd_device* d = sd_device_enumerator_get_device_first( en ); d;
            d = sd_device_enumerator_get_device_next( en ))
        update_device( d, false );
    sd_device_enumerator_unref( en );

    CHECK( watch_dir_of( config_file ), "inotify on config dir" );
    if (watch_dir_of( fstab_file ) < 0)
        LOG( "cannot watch fstab directory" );

    struct libmnt_monitor* mn = mnt_new_monitor();
    if (mn && mnt_monitor_enable_kernel( mn, 1 ) == 0) {
        int fd = mnt_monitor_get_fd( mn );
        if (fd < 0 || sd_event_add_io( event, nullptr, fd, EPOLLIN, on_mount_change, mn ) < 0)
            LOG( "cannot watch mount table" );
    }

    reload();
    reconcile();
    LOG( "running on the %s bus as %s", session_mode() ? "session" : "system", BSC_BUS_NAME );
    int r = sd_event_loop( event );

    for (auto& [name, g]: groups) withdraw( g );
    for (auto& [id, q]: questions) sd_event_source_disable_unref( q.timer );
    questions.clear();
    sd_bus_flush( bus );
    sd_device_monitor_unref( mon );
    mnt_unref_monitor( mn );
    sd_bus_flush_close_unref( bus );
    sd_event_unref( event );
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
