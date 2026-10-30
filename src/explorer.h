// PHYLKA block explorer, served by the node's local RPC port: the page at "/" and JSON at /api/...
//   /api/info  /api/blocks?count=&before=  /api/block?h=HEIGHT|HASH  /api/tx?id=  /api/address?a=
#pragma once
#include <string>

#include "node.h"

namespace plk {

/// Returns true if `path` belongs to the explorer; fills the body and its content type
bool explorer_route(Node& node, size_t peers, double hashrate, const std::string& path, std::string& body, std::string& content_type);

}  // namespace plk
