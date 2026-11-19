# btrfs-snap-cpp
simple c++ snapshot backup with btrfs for linux

## features
* create local snapshots
* send snapshots to other partitions (ssh not implemented)
* send partial snapshots to other partitions
* use smallest partial snapshot difference possible (depending on data available
  on remote and locally)
* clean up after run, e.g. keep only `n` local and `m` remote snapshots
* daemon mode: watch for backup disks and ask what to do when they are
  connected (mount + backup / mount only / nothing, then unmount / keep)

## technicalities
* build with `cmake -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build`
  (or `make`); needs a
  `c++17` compiler, boost (property_tree), libmount and libsystemd.
  Everything is one executable, `btrfs-snap-cpp`.
* run with `btrfs-snap-cpp -d` for a dry run. show help with `btrfs-snap-cpp -h`.
* config file: `/etc/btrfs-snap-cpp.conf` (falls back to the old
  `/etc/btrfs-snap.conf`), see `btrfs-snap-cpp.conf`. `cmake --install`
  puts the commented sample there only if no config exists yet.
* install with prefix `/usr`: the D-Bus policy and the polkit action are only
  read from `/usr/share/...`, and the config then goes to `/etc` (with the
  default prefix `/usr/local` it would be `/usr/local/etc`).

## daemon mode
* sections join a *watch group* with `watch = <group>`. The group's disks are
  taken from fstab (the entry holding `remote_snapshot_dir`), or configured
  explicitly:
  ```ini
  [watch hdd]
  devices = UUID=5f2c...  PARTUUID=1234abcd-01   # all must be connected
  mount   = /mnt/backup-hdd                      # optional, default: fstab
  ```
  Without `devices`, the disks are the fstab sources of the `mount` points:
  ```ini
  [watch hdd]
  mount = /mnt/backup-hdd    # fstab: UUID=5f2c... /mnt/backup-hdd btrfs noauto 0 0
  ```
* `btrfs-snap-cpp --daemon` (root, `btrfs-snap-cpp.service`) watches devices
  and asks questions over D-Bus; backups run as
  `btrfs-snap-cpp-backup-<group>.service`
  (logs: `journalctl -u btrfs-snap-cpp-backup-<group>`).
* `btrfs-snap-cpp agent` (user, `btrfs-snap-cpp-agent.service`) shows the
  questions as desktop notifications.
* without root: `btrfs-snap-cpp status | backup [-w] <group> | mount <group> |
  umount <group> | stop <group> | answer <id> <choice> | reload | ask [<group>]`.
  Changes are authorized by polkit (`org.btrfssnapcpp.manage`, no password
  for the active local session).
* `btrfs-snap-cpp ask [<group>]` brings a group's question up again (every
  group without `<group>`): the pending one if there is one, else the one
  for its state (connected: mount / backup, mounted: unmount). As always,
  nothing happens unless it is answered. Handy on a key binding.
* setup: `sudo cmake --install build`, then
  `systemctl enable --now btrfs-snap-cpp` and
  `systemctl --user enable --now btrfs-snap-cpp-agent`.
