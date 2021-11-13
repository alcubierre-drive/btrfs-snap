#include "config.hpp"
#include "snap.hpp"

#include <iostream>
#include <boost/property_tree/exceptions.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

int parse_config( string fname, vector<vector<pair<string,string>>>& config,
        vector<string>& sections ) {
    boost::property_tree::ptree pt;
    try {
        boost::property_tree::ini_parser::read_ini(fname, pt);
    } catch(boost::property_tree::ini_parser::ini_parser_error& path) {
        ERR( "config parser error '" << path.what() << "'" );
        return EXIT_FAILURE;
    }

    for (auto& section : pt) {
        sections.push_back( section.first );
        vector<pair<string,string>> cf;
        for (auto& key : section.second)
            cf.push_back( pair<string,string> { key.first, key.second.get_value<string>() } );
        config.push_back( cf );
    }

    return EXIT_SUCCESS;
}

static bool parse_bool( string s ) {
    if (s == "True" || s == "true" || s == "1")
        return true;
    if (s == "False" || s == "false" || s == "0")
        return false;
    WARN( "'" << s << "' not a boolean. return false." );
    return false;
}

void set_params( const vector<pair<string,string>>& params ) {
    for (auto& p: params) {
        if (p.first == "host_name") {
            snapshot_setup::host_name = p.second;
        } else if (p.first == "backup_name") {
            snapshot_setup::backup_name = p.second;
        } else if (p.first == "backup_dir") {
            snapshot_setup::backup_dir = p.second;
        } else if (p.first == "snapshot_dir") {
            snapshot_setup::snapshot_dir = p.second;
        } else if (p.first == "remote_snapshot_dir") {
            snapshot_setup::remote_snapshot_dir = p.second;
        } else if (p.first == "keep_remote_snapshots_num") {
            snapshot_setup::keep_remote_snapshots_num = std::stoi(p.second);
        } else if (p.first == "keep_snapshots_num") {
            snapshot_setup::keep_snapshots_num = std::stoi(p.second);
        } else if (p.first == "dry_run") {
            snapshot_setup::dry_run = parse_bool(p.second);
        } else if (p.first == "pre_command") {
            snapshot_setup::pre_command = p.second;
        } else if (p.first == "post_command") {
            snapshot_setup::post_command = p.second;
        } else if (p.first == "transfer") {
            snapshot_setup::transfer = parse_bool(p.second);
        } else if (p.first == "create") {
            snapshot_setup::create = parse_bool(p.second);
        } else {
            WARN( "key '" << p.first << "' unknown." );
        }
    }
}
