# btrfs-snap
simple c++ snapshot backup with btrfs for linux

## features
* create local snapshots
* send snapshots to other partitions (ssh not implemented)
* send partial snapshots to other partitions
* use smallest partial snapshot difference possible (depending on data available
  on remote and locally)
* clean up after run, e.g. keep only `n` local and `m` remote snapshots

## technicalities
* `g++ -std=c++17 -Wall -Wextra -pedantic snap.cpp -o snap` with a `c++17`
  capable `g++` version and `linux-headers`
* run with `./snap -d` for a dry run. show help with `./snap -h`.
