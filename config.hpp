#include <string>
#include <vector>
#include <utility>

using std::string;
using std::vector;
using std::pair;

int parse_config( string fname, vector<vector<pair<string,string>>>& config,
        vector<string>& sections );
void set_params( const vector<pair<string,string>>& params );
