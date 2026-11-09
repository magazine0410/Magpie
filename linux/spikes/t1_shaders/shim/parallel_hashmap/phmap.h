// Stand-in for parallel-hashmap: the spike only needs the container interface.
#pragma once
#include <unordered_map>
#include <unordered_set>

namespace phmap {
template <typename K, typename V>
using flat_hash_map = std::unordered_map<K, V>;
template <typename K>
using flat_hash_set = std::unordered_set<K>;
}
