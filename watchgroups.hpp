#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

// a device a watch group depends on
struct watch_device {
    string spec;        // as written: "UUID=...", "PARTUUID=...", "/dev/disk/..."
    string key;         // normalized lookup key: "uuid:<lower>", "dev:<path>", ...
    string mountpoint;  // fstab target ("" if the device has no fstab entry)
    string source;      // where it was configured: "watch section", "watch_devices", "fstab"
};

// backup sections sharing the same "watch = <name>", plus the optional
// "[watch <name>]" section configuring the group's disks
struct watch_group {
    string name;
    vector<string> sections;
    vector<watch_device> devices;
    vector<string> mountpoints;  // to mount before / unmount after a backup
};

// "watch <group>" sections configure a group and are not backup jobs.
// returns true (and sets group) for such a section name.
bool watch_section_group( const string& section, string& group );

// normalize an fstab-style source (TAG=value or /dev path) to a lookup key.
// returns "" if the spec is not a device reference we can watch.
string watch_normalize_spec( const string& spec );

// evaluate the config file (with the same inheritance rules btrfs-snap-cpp
// uses) and resolve each group's devices. fstab_file "" = /etc/fstab.
// returns EXIT_SUCCESS or EXIT_FAILURE; problems with single sections are
// reported as warnings.
int load_watch_groups( const string& config_file, const string& fstab_file,
        vector<watch_group>& groups );

const watch_group* find_watch_group( const vector<watch_group>& groups, const string& name );

// is anything mounted at target (per /proc/self/mountinfo)?
bool watch_is_mounted( const string& target );
