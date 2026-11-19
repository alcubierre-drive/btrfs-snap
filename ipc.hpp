#pragma once
// shared between daemon, agent and client

#include <systemd/sd-bus.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define BSC_BUS_NAME "org.btrfssnapcpp.Daemon1"
#define BSC_OBJ_PATH "/org/btrfssnapcpp/Daemon1"
#define BSC_IFACE "org.btrfssnapcpp.Daemon1"
#define BSC_POLKIT_ACTION "org.btrfssnapcpp.manage"

namespace bsc {

using std::string;
using std::vector;

const uint64_t USEC_PER_SEC = 1000000ULL;

// BTRFS_SNAP_CPP_BUS=session: daemon/agent/client meet on the session bus and
// the daemon skips polkit (testing without root)
inline bool session_mode() {
    const char* e = getenv( "BTRFS_SNAP_CPP_BUS" );
    return e && !strcmp( e, "session" );
}

inline int open_daemon_bus( sd_bus** bus ) {
    return session_mode() ? sd_bus_open_user( bus ) : sd_bus_open_system( bus );
}

inline string join( const vector<string>& v, const string& sep ) {
    string out;
    for (size_t i=0; i<v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

inline string shell_quote( const string& s ) {
    string out = "'";
    for (char c: s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// systemd-escape
inline string unit_escape( const string& s ) {
    string out;
    for (size_t i=0; i<s.size(); ++i) {
        unsigned char c = s[i];
        if (c == '/') out += '-';
        else if (isalnum( c ) || c == ':' || c == '_' || (c == '.' && i > 0)) out += (char)c;
        else {
            char buf[5];
            snprintf( buf, sizeof(buf), "\\x%02x", c );
            out += buf;
        }
    }
    return out;
}

// transient unit running a group's backup
inline string backup_unit_name( const string& group ) {
    return "btrfs-snap-cpp-backup-" + unit_escape( group ) + ".service";
}

inline string duration( long secs ) {
    char buf[64];
    if (secs >= 3600) snprintf( buf, sizeof(buf), "%ldh%02ldm", secs/3600, secs%3600/60 );
    else if (secs >= 60) snprintf( buf, sizeof(buf), "%ldm%02lds", secs/60, secs%60 );
    else snprintf( buf, sizeof(buf), "%lds", secs );
    return buf;
}

inline const char* bus_error_text( const sd_bus_error* e, int r ) {
    if (e && e->message) return e->message;
    if (e && e->name) return e->name;
    return strerror( r < 0 ? -r : r );
}

} // namespace bsc
