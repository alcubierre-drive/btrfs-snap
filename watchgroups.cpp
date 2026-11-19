#include "watchgroups.hpp"
#include "config.hpp"
#include "snap.hpp"

#include <libmount/libmount.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

static string lower( string s ) {
    for (auto& c: s) c = std::tolower( (unsigned char)c );
    return s;
}

static string strip_quotes( string s ) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr( 1, s.size()-2 );
    return s;
}

static string strip_trailing_slashes( string s ) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

static vector<string> split_ws( const string& s ) {
    vector<string> out;
    std::istringstream ss( s );
    for (string w; ss >> w; ) out.push_back( w );
    return out;
}

bool watch_section_group( const string& section, string& group ) {
    static const string prefix = "watch ";
    if (section.compare( 0, prefix.size(), prefix ) != 0)
        return false;
    group = section.substr( prefix.size() );
    size_t b = group.find_first_not_of( " \t" ), e = group.find_last_not_of( " \t" );
    group = b == string::npos ? "" : group.substr( b, e-b+1 );
    return true;
}

string watch_normalize_spec( const string& spec ) {
    if (spec.rfind( "/dev/", 0 ) == 0)
        return "dev:" + spec;
    size_t eq = spec.find( '=' );
    if (eq == string::npos)
        return "";
    string tag = spec.substr( 0, eq );
    string value = strip_quotes( spec.substr( eq+1 ) );
    if (value.empty())
        return "";
    if (tag == "UUID") return "uuid:" + lower( value );
    if (tag == "PARTUUID") return "partuuid:" + lower( value );
    if (tag == "LABEL") return "label:" + value;
    if (tag == "PARTLABEL") return "partlabel:" + value;
    return "";
}

// fstab entry whose target is the longest prefix of path
static struct libmnt_fs* fstab_entry_for_path( struct libmnt_table* tb, const string& path_ ) {
    string path = strip_trailing_slashes( path_ );
    struct libmnt_fs* best = nullptr;
    size_t best_len = 0;
    struct libmnt_iter* it = mnt_new_iter( MNT_ITER_FORWARD );
    struct libmnt_fs* fs;
    while (mnt_table_next_fs( tb, it, &fs ) == 0) {
        if (mnt_fs_is_swaparea( fs ) || mnt_fs_is_pseudofs( fs ) || mnt_fs_is_netfs( fs ))
            continue;
        const char* t = mnt_fs_get_target( fs );
        if (!t || t[0] != '/') continue;
        string target = strip_trailing_slashes( t );
        bool prefix = path == target || target == "/" ||
            (path.size() > target.size() && path.compare( 0, target.size(), target ) == 0 &&
             path[target.size()] == '/');
        if (prefix && target.size() >= best_len) {
            best = fs;
            best_len = target.size();
        }
    }
    mnt_free_iter( it );
    return best;
}

// fstab target of the entry whose source is the same device as key
static string fstab_target_for_key( struct libmnt_table* tb, const string& key ) {
    string target;
    struct libmnt_iter* it = mnt_new_iter( MNT_ITER_FORWARD );
    struct libmnt_fs* fs;
    while (mnt_table_next_fs( tb, it, &fs ) == 0) {
        const char* src = mnt_fs_get_source( fs );
        const char* t = mnt_fs_get_target( fs );
        if (src && t && t[0] == '/' && watch_normalize_spec( src ) == key) {
            target = strip_trailing_slashes( t );
            break;
        }
    }
    mnt_free_iter( it );
    return target;
}

// fstab entry mounted exactly at target
static struct libmnt_fs* fstab_entry_for_target( struct libmnt_table* tb, const string& target ) {
    struct libmnt_fs* found = nullptr;
    struct libmnt_iter* it = mnt_new_iter( MNT_ITER_FORWARD );
    struct libmnt_fs* fs;
    while (mnt_table_next_fs( tb, it, &fs ) == 0) {
        const char* t = mnt_fs_get_target( fs );
        if (t && strip_trailing_slashes( t ) == target) {
            found = fs;
            break;
        }
    }
    mnt_free_iter( it );
    return found;
}

static watch_group& get_group( vector<watch_group>& groups, const string& name ) {
    for (auto& e: groups)
        if (e.name == name) return e;
    groups.push_back( watch_group{} );
    groups.back().name = name;
    return groups.back();
}

static void add_device( watch_group& g, const watch_device& d, bool add_mountpoint ) {
    for (auto& e: g.devices)
        if (e.key == d.key) return;
    g.devices.push_back( d );
    if (add_mountpoint && !d.mountpoint.empty() &&
            std::find( g.mountpoints.begin(), g.mountpoints.end(), d.mountpoint ) == g.mountpoints.end())
        g.mountpoints.push_back( d.mountpoint );
}

static void add_specs( watch_group& g, struct libmnt_table* tb, const string& specs,
        const string& source, const string& where, bool add_mountpoint ) {
    for (const string& spec: split_ws( specs )) {
        watch_device d;
        d.spec = spec;
        d.key = watch_normalize_spec( spec );
        d.source = source;
        if (d.key.empty()) {
            WARN( "[" << where << "] cannot watch device '" << spec << "'" );
            continue;
        }
        d.mountpoint = fstab_target_for_key( tb, d.key );
        add_device( g, d, add_mountpoint );
    }
}

int load_watch_groups( const string& config_file, const string& fstab_file,
        vector<watch_group>& groups ) {
    groups.clear();
    vector<vector<pair<string,string>>> config;
    vector<string> sections;
    if (parse_config( config_file, config, sections ))
        return EXIT_FAILURE;

    struct libmnt_table* tb = mnt_new_table();
    if (!tb || mnt_table_parse_fstab( tb, fstab_file.empty() ? nullptr : fstab_file.c_str() )) {
        ERR( "cannot parse fstab '" << (fstab_file.empty() ? "/etc/fstab" : fstab_file) << "'" );
        mnt_unref_table( tb );
        return EXIT_FAILURE;
    }

    // [watch <group>] sections first: they take precedence
    std::map<string, bool> has_watch_section;
    for (unsigned i=0; i<sections.size(); ++i) {
        string name;
        if (!watch_section_group( sections[i], name ))
            continue;
        if (name.empty()) {
            WARN( "[" << sections[i] << "] needs a group name" );
            continue;
        }
        if (has_watch_section[name]) {
            WARN( "[" << sections[i] << "] defined twice; using the first one" );
            continue;
        }
        has_watch_section[name] = true;
        watch_group& g = get_group( groups, name );
        string devices, mount;
        bool have_mount = false;
        for (auto& kv: config[i]) {
            if (kv.first == "devices") devices = kv.second;
            else if (kv.first == "mount") { mount = kv.second; have_mount = true; }
            else WARN( "[" << sections[i] << "] key '" << kv.first << "' unknown." );
        }
        add_specs( g, tb, devices, "watch section", sections[i], !have_mount );
        for (const string& mp_: split_ws( mount )) {
            string mp = strip_trailing_slashes( mp_ );
            g.mountpoints.push_back( mp );
            if (!devices.empty())
                continue;
            // no devices given: watch the fstab source of each mount point
            struct libmnt_fs* fs = fstab_entry_for_target( tb, mp );
            const char* src = fs ? mnt_fs_get_source( fs ) : nullptr;
            watch_device d;
            d.spec = src ? src : "";
            d.key = watch_normalize_spec( d.spec );
            d.mountpoint = mp;
            d.source = "fstab";
            if (!src)
                WARN( "[" << sections[i] << "] mount point '" << mp << "' is not in fstab; set devices =" );
            else if (d.key.empty())
                WARN( "[" << sections[i] << "] fstab source '" << src << "' of '" << mp
                        << "' cannot be watched; set devices =" );
            else
                add_device( g, d, false );
        }
        if (g.devices.empty())
            WARN( "[" << sections[i] << "] has no devices" );
    }

    snapshot_setup_state saved = snapshot_setup_save();
    for (unsigned i=0; i<sections.size(); ++i) {
        string dummy;
        if (watch_section_group( sections[i], dummy ))
            continue;
        set_params( config[i] );
        if (snapshot_setup::watch_group.empty())
            continue;
        watch_group& g = get_group( groups, snapshot_setup::watch_group );
        g.sections.push_back( sections[i] );
        if (has_watch_section[g.name])
            continue;

        if (!snapshot_setup::watch_devices.empty()) {
            add_specs( g, tb, snapshot_setup::watch_devices, "watch_devices", sections[i], true );
        } else if (snapshot_setup::transfer) {
            struct libmnt_fs* fs = fstab_entry_for_path( tb, snapshot_setup::remote_snapshot_dir );
            const char* t = fs ? mnt_fs_get_target( fs ) : nullptr;
            const char* src = fs ? mnt_fs_get_source( fs ) : nullptr;
            if (!fs || !t || !src || strip_trailing_slashes( t ) == "/") {
                WARN( "[" << sections[i] << "] remote_snapshot_dir '" << snapshot_setup::remote_snapshot_dir
                        << "' is not on its own fstab entry; configure [watch " << g.name << "]" );
                continue;
            }
            watch_device d;
            d.spec = src;
            d.key = watch_normalize_spec( src );
            d.mountpoint = strip_trailing_slashes( t );
            d.source = "fstab";
            if (d.key.empty()) {
                WARN( "[" << sections[i] << "] fstab source '" << src << "' of '" << t
                        << "' cannot be watched; configure [watch " << g.name << "]" );
                continue;
            }
            add_device( g, d, true );
        } else {
            WARN( "[" << sections[i] << "] has watch = " << g.name
                    << " but transfer disabled; configure [watch " << g.name << "]" );
        }
    }
    snapshot_setup_restore( saved );
    mnt_unref_table( tb );

    for (auto& g: groups) {
        if (g.devices.empty())
            WARN( "watch group '" << g.name << "' has no devices and will never trigger" );
        if (g.sections.empty())
            WARN( "watch group '" << g.name << "' has no sections with 'watch = " << g.name << "'" );
    }
    return EXIT_SUCCESS;
}

const watch_group* find_watch_group( const vector<watch_group>& groups, const string& name ) {
    for (auto& g: groups)
        if (g.name == name) return &g;
    return nullptr;
}

bool watch_is_mounted( const string& target ) {
    struct libmnt_table* tb = mnt_new_table();
    bool mounted = false;
    if (tb && mnt_table_parse_mtab( tb, nullptr ) == 0)
        mounted = mnt_table_find_target( tb, target.c_str(), MNT_ITER_BACKWARD ) != nullptr;
    mnt_unref_table( tb );
    return mounted;
}
