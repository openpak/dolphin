// OpenPak: the applied network profile (emulators/prds/emulator-network-profile-prd.md §4c).
// At launch the fetched profile replaces the compiled-in suffix list and, when it carries
// one, the server address; until then the compiled-in list and MAIN_WII_OPENPAK_SERVER
// apply. This holder is the seam between the profile fetched in the Qt layer and the
// resolver inside IOS, which never blocks a game on the network.
#pragma once

#include <string>
#include <vector>

namespace Core::OpenPakRedirect
{
// Replaces the compiled-in suffix list. Exact names belong here too: a full hostname
// matches only itself as a suffix pattern, which is exactly the semantics it needs.
void SetRedirectSuffixes(std::vector<std::string> suffixes);

// Overrides MAIN_WII_OPENPAK_SERVER while set. The setting stays the fallback.
void SetServerAddress(std::string address);

// Whether hostname ends with one of the applied suffixes (compiled-in until replaced).
bool RedirectSuffixMatch(const std::string& hostname);

// The address a redirected name resolves to: the applied profile's address when one is
// applied, else MAIN_WII_OPENPAK_SERVER.
std::string ServerAddress();
}  // namespace Core::OpenPakRedirect
