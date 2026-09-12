// OpenPak: see OpenPakRedirect.h.
#include "Core/IOS/Network/IP/OpenPakRedirect.h"

#include <mutex>

#include "Core/Config/MainSettings.h"

namespace Core::OpenPakRedirect
{
namespace
{
std::mutex g_mutex;
std::vector<std::string> g_suffixes;  // empty: the compiled-in list applies
std::string g_address;                // empty: MAIN_WII_OPENPAK_SERVER applies
}  // namespace

void SetRedirectSuffixes(std::vector<std::string> suffixes)
{
  std::lock_guard lock(g_mutex);
  g_suffixes = std::move(suffixes);
}

void SetServerAddress(std::string address)
{
  std::lock_guard lock(g_mutex);
  g_address = std::move(address);
}

bool RedirectSuffixMatch(const std::string& hostname)
{
  std::lock_guard lock(g_mutex);
  for (const std::string& suffix : g_suffixes)
  {
    if (hostname.size() >= suffix.size() &&
        hostname.compare(hostname.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
      return true;
    }
  }
  return false;
}

std::string ServerAddress()
{
  std::lock_guard lock(g_mutex);
  if (!g_address.empty())
    return g_address;
  return Config::Get(Config::MAIN_WII_OPENPAK_SERVER);
}
}  // namespace Core::OpenPakRedirect
