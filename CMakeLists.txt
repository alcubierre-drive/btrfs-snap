cmake_minimum_required(VERSION 3.0)

project(
    btrfs-snap
    VERSION 0.9
    LANGUAGES CXX)

add_executable(btrfs-snap snap.cpp main.cpp config.cpp)
