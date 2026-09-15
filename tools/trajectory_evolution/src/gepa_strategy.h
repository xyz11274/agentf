#pragma once

#include "integration_json.h"
#include <array>
#include <set>

namespace trajectory::evolution::integration::gepa {

double mean(const Json& scores, const std::string& field);
bool no_worse(const Json& candidate, const Json& parent);
std::vector<std::size_t> parents(const Json& node);
std::set<std::size_t> ancestors(const Json& nodes, std::size_t index);
void validate_nodes(Json& nodes);
Json frontier_map(const Json& nodes, const std::vector<std::string>& tasks, const Json& config);
std::vector<std::size_t> pool(const Json& mapping);
std::size_t select_parent(const Json& nodes, const Json& mapping, const Json& config, std::size_t round);
Json select_merge(const Json& nodes, const Json& mapping, const Json& attempts,
                  const Json& config, std::size_t round);
Json recombine(const Json& ancestor, const Json& first, const Json& second,
               std::size_t first_id, std::size_t second_id, bool prefer_first);
Json select_components(const Json& components, const std::string& strategy, std::size_t cursor);
std::vector<std::size_t> select_proposals(const Json& nodes, const Json& ids, const Json& config);

}  // namespace trajectory::evolution::integration::gepa
