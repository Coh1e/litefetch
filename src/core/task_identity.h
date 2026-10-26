#pragma once

#include <string>
#include <string_view>

namespace litefetch {

// Stable task identity: lowercase hex SHA-256 over the caller's canonical
// source bytes. The same source therefore keeps the same identity across
// process restarts and database reopenings.
std::string taskIdentity(std::string_view canonicalSource);

} // namespace litefetch
