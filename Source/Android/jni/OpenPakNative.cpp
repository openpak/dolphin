// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenPak surface the Android app calls into (the Wii family: account-only sign-in, no
// console identity). openpak-client has no UI of its own; this is the whole bridge: plain calls
// in, JSON strings out, so the Kotlin screens need no JNI object marshalling. Every call that
// touches the network blocks, and Kotlin makes it from Dispatchers.IO, never the main thread.

#include "jni/OpenPakNative.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>
#include <jni.h>
#include <nlohmann/json.hpp>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/IOS/Network/IP/OpenPakRedirect.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

#include <openpak/account.h>
#include <openpak/api.h>
#include <openpak/nat_check.h>
#include <openpak/network_profile.h>
#include <openpak/platform.h>
#include <openpak/save_sync.h>

namespace
{
using nlohmann::json;
namespace Api = WebService::OpenPakApi;
namespace Account = Common::OpenPakAccount;
namespace SaveSync = Nextendo::SaveSync;

constexpr char PLATFORM[] = "wii";

// What the Kotlin side words: OpenPakNotifier's kinds.
enum class Event : jint
{
  Pulled = 0,
  Conflict = 1,
  Pushed = 2,
  PushFailed = 3,
};

jclass s_notifier_class = nullptr;
jmethodID s_notifier_text = nullptr;
jmethodID s_notifier_notify = nullptr;
jmethodID s_notifier_checking = nullptr;

std::string TitleHex(u64 title_id)
{
  return fmt::format("{:016x}", title_id);
}

std::filesystem::path WiiSaveDir(u64 title_id)
{
  return std::filesystem::path(
      Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Configured));
}

bool SyncWanted()
{
  return Account::HasBearer() && Config::Get(Config::MAIN_OPENPAK_CLOUD_SAVE);
}

bool NotificationsWanted()
{
  return Config::Get(Config::MAIN_OPENPAK_NOTIFICATIONS);
}

// Applies a fetched (or cached) profile to the resolver inside IOS, as the desktop build does.
// The built-in source leaves the compiled-in list and MAIN_WII_OPENPAK_SERVER alone.
void ApplyNetworkProfile(const openpak::NetworkProfile::Result& applied)
{
  if (applied.source == openpak::NetworkProfile::Source::BuiltIn)
    return;
  std::vector<std::string> suffixes = applied.profile.suffixes;
  suffixes.insert(suffixes.end(), applied.profile.exact.begin(), applied.profile.exact.end());
  Core::OpenPakRedirect::SetRedirectSuffixes(suffixes);
  if (!applied.profile.server_address.empty())
    Core::OpenPakRedirect::SetServerAddress(applied.profile.server_address);
}

// "{0}" of "Network settings: {0}."
std::string NetworkSummary(const openpak::NetworkProfile::Result& applied)
{
  if (applied.source == openpak::NetworkProfile::Source::BuiltIn)
    return openpak::NetworkProfile::SourceName(applied.source);
  return fmt::format("v{} ({})", applied.profile.version,
                     openpak::NetworkProfile::SourceName(applied.source));
}

// The localized sentence for an event, from res/values/openpak_strings.xml.
std::string EventText(JNIEnv* env, Event event, const std::string& name, const std::string& detail)
{
  if (!s_notifier_class)
    return {};
  auto* text = static_cast<jstring>(env->CallStaticObjectMethod(
      s_notifier_class, s_notifier_text, static_cast<jint>(event), ToJString(env, name),
      ToJString(env, detail)));
  return text ? GetJString(env, text) : std::string{};
}

void NotifyApp(Event event, const std::string& name, const std::string& detail)
{
  if (!s_notifier_class || !NotificationsWanted())
    return;
  JNIEnv* env = IDCache::GetEnvForThread();
  env->CallStaticVoidMethod(s_notifier_class, s_notifier_notify, static_cast<jint>(event),
                            ToJString(env, name), ToJString(env, detail));
}

void ShowChecking(bool show)
{
  if (!s_notifier_class)
    return;
  JNIEnv* env = IDCache::GetEnvForThread();
  env->CallStaticVoidMethod(s_notifier_class, s_notifier_checking, show ? JNI_TRUE : JNI_FALSE);
}

// The Wii title a boot is about, or nothing (GameCube, homebrew, the System Menu).
std::optional<u64> WiiTitleOf(const BootParameters& boot)
{
  std::optional<u64> title_id;
  if (const auto* disc = std::get_if<BootParameters::Disc>(&boot.parameters))
  {
    if (disc->volume && disc->volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
      title_id = disc->volume->GetTitleID();
  }
  else if (const auto* wad = std::get_if<DiscIO::VolumeWAD>(&boot.parameters))
  {
    title_id = wad->GetTitleID();
  }
  else if (const auto* nand = std::get_if<BootParameters::NANDTitle>(&boot.parameters))
  {
    title_id = nand->id;
  }
  // System titles (00000001-...) keep no game save worth syncing.
  if (!title_id || *title_id == 0 || (*title_id >> 32) == 0x00000001)
    return std::nullopt;
  return title_id;
}

// This run's title, captured before boot so the push on stop cannot race the config's teardown.
struct Run
{
  u64 title_id = 0;
  std::filesystem::path save_dir;
  std::string name;
  bool conflict = false;              // both sides have a save: automatic sync waits
  std::optional<Event> pull_message;  // shown once the core runs
};
std::mutex s_run_mutex;
std::optional<Run> s_run;

// The pull in flight, shared with the thread that runs it: the emulation thread stops waiting
// after five seconds or on Skip, and the pull then leaves the local save alone.
struct Pull
{
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool skipped = false;
  std::atomic<bool> wanted{true};
  SaveSync::PullOutcome outcome = SaveSync::PullOutcome::Nothing;
};
std::mutex s_pull_mutex;
std::shared_ptr<Pull> s_pull;

void RefreshNetworkProfileInBackground()
{
  std::thread([] {
    ApplyNetworkProfile(openpak::NetworkProfile::Fetch(PLATFORM));
  }).detach();
}

json FriendJson(const Api::AccountFriend& f)
{
  return json{{"account_id", f.account_id},
              {"name", f.display_name},
              {"friend_code", f.friend_code},
              {"online", f.online},
              {"title_id", f.title_id},
              {"console", f.console_namespace},
              {"online_since", f.online_since},
              {"friends_since", f.friends_since}};
}

std::optional<u64> ParseTitle(const std::string& hex)
{
  try
  {
    size_t used = 0;
    const u64 value = std::stoull(hex, &used, 16);
    if (used != hex.size())
      return std::nullopt;
    return value;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

// The sign-in error as a string-table key, so the screen shows the spec's words and never an
// HTTP code: credentials, rate_limited, unreachable, or server with the server's own sentence.
json SignInError(const std::string& error)
{
  if (error == "Wrong email or password.")
    return json{{"ok", false}, {"code", "credentials"}};
  if (error == "Too many attempts. Wait a minute and try again.")
    return json{{"ok", false}, {"code", "rate_limited"}};
  const bool machine_text = error.empty() || error.starts_with("Could not reach") ||
                            error.starts_with("Sign-in failed (HTTP") ||
                            error.starts_with("Unexpected") || error.starts_with("The website");
  if (machine_text)
  {
    if (!error.empty())
      WARN_LOG_FMT(COMMON, "OpenPak sign-in failed: {}", error);
    return json{{"ok", false}, {"code", "unreachable"}};
  }
  return json{{"ok", false}, {"code", "server"}, {"message", error}};
}

jstring Text(JNIEnv* env, const json& value)
{
  return ToJString(env, value.dump());
}
}  // namespace

namespace OpenPakNative
{
void BeforeBoot(const BootParameters& boot)
{
  {
    std::lock_guard lock(s_run_mutex);
    s_run.reset();
  }
  // NetPlay brings its own saves from the host; they are not this machine's to sync.
  if (!SyncWanted() || NetPlay::IsNetPlayRunning())
    return;
  const std::optional<u64> title_id = WiiTitleOf(boot);
  if (!title_id)
    return;

  Run run;
  run.title_id = *title_id;
  run.save_dir = WiiSaveDir(*title_id);

  auto pull = std::make_shared<Pull>();
  {
    std::lock_guard lock(s_pull_mutex);
    s_pull = pull;
  }
  ShowChecking(true);
  std::thread([pull, dir = run.save_dir, id = run.title_id] {
    SaveSync::PullOutcome outcome = SaveSync::PullOutcome::Nothing;
    try
    {
      outcome = SaveSync::PullBeforeLaunch(dir, id, [pull] { return pull->wanted.load(); });
    }
    catch (const std::exception& e)
    {
      WARN_LOG_FMT(COMMON, "OpenPak save pull {:016x} failed: {}", id, e.what());
    }
    std::lock_guard lock(pull->mutex);
    pull->outcome = outcome;
    pull->done = true;
    pull->cv.notify_all();
  }).detach();

  // Never block a launch: five seconds at most, and Skip ends the wait at once.
  {
    std::unique_lock lock(pull->mutex);
    pull->cv.wait_for(lock, std::chrono::seconds(5), [&] { return pull->done || pull->skipped; });
    if (pull->done)
    {
      if (pull->outcome == SaveSync::PullOutcome::Pulled)
        run.pull_message = Event::Pulled;
      else if (pull->outcome == SaveSync::PullOutcome::BothExist)
      {
        run.pull_message = Event::Conflict;
        run.conflict = true;
      }
    }
    else
    {
      pull->wanted = false;
      INFO_LOG_FMT(COMMON, "OpenPak save pull {:016x}: not waiting any longer, booting local",
                   run.title_id);
    }
  }
  {
    std::lock_guard lock(s_pull_mutex);
    s_pull.reset();
  }
  ShowChecking(false);

  std::lock_guard lock(s_run_mutex);
  s_run = std::move(run);
}

void AfterBoot()
{
  std::lock_guard lock(s_run_mutex);
  if (!s_run)
    return;
  s_run->name = SConfig::GetInstance().GetTitleName();
  if (s_run->name.empty())
    s_run->name = TitleHex(s_run->title_id);
  if (!s_run->pull_message || !NotificationsWanted())
    return;
  JNIEnv* env = IDCache::GetEnvForThread();
  const std::string text = EventText(env, *s_run->pull_message, s_run->name, {});
  if (!text.empty())
    OSD::AddMessage(text, 6000);
  s_run->pull_message.reset();
}

void AfterShutdown()
{
  std::optional<Run> run;
  {
    std::lock_guard lock(s_run_mutex);
    std::swap(run, s_run);
  }
  // No name: the core never ran (the boot failed), so there is nothing new to send.
  if (!run || run->name.empty() || run->conflict || !SyncWanted())
    return;
  std::vector<u8> zip = SaveSync::CaptureOnExit(run->save_dir, run->title_id);
  if (zip.empty())
    return;
  std::thread([run = std::move(*run), zip = std::move(zip)]() mutable {
    std::string error;
    try
    {
      error = SaveSync::PushCaptured(run.save_dir, run.title_id, std::move(zip));
    }
    catch (const std::exception& e)
    {
      error = e.what();
    }
    if (error.empty())
    {
      NotifyApp(Event::Pushed, run.name, {});
    }
    else
    {
      WARN_LOG_FMT(COMMON, "OpenPak save push {:016x} failed: {}", run.title_id, error);
      NotifyApp(Event::PushFailed, run.name, error);
    }
  }).detach();
}
}  // namespace OpenPakNative

extern "C" {

JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_init(
    JNIEnv* env, jclass, jstring jversion, jstring jdevice)
{
  static std::once_flag once;
  std::call_once(once, [&] {
    jclass notifier =
        env->FindClass("org/dolphinemu/dolphinemu/features/openpak/model/OpenPakNotifier");
    s_notifier_class = static_cast<jclass>(env->NewGlobalRef(notifier));
    s_notifier_text = env->GetStaticMethodID(
        s_notifier_class, "text", "(ILjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    s_notifier_notify = env->GetStaticMethodID(s_notifier_class, "notify",
                                               "(ILjava/lang/String;Ljava/lang/String;)V");
    s_notifier_checking = env->GetStaticMethodID(s_notifier_class, "checking", "(Z)V");

    openpak::Platform::SetDirectories(File::GetUserPath(D_CONFIG_IDX),
                                      File::GetUserPath(D_CACHE_IDX));
    openpak::Platform::SetClient("dolphin", GetJString(env, jversion));
    Api::SetSavesPlatform(PLATFORM);
    Api::SetSaveDevice(GetJString(env, jdevice));

    // One conditional request per app start (never per game), and only when the player asked
    // for OpenPak: off means upstream's behaviour, with nothing sent anywhere.
    if (Config::Get(Config::MAIN_WII_OPENPAK_ENABLE))
      RefreshNetworkProfileInBackground();
  });
}

// Who is signed in, as the last sign-in left it. Local only; never blocks on the network.
JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_state(
    JNIEnv* env, jclass)
{
  const bool signed_in = Account::HasBearer();
  return Text(env, json{{"signed_in", signed_in},
                        {"name", signed_in ? Account::GetUsername() : std::string{}},
                        {"website", Api::BaseUrl()},
                        {"status_url", Api::StatusUrl()}});
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_signIn(
    JNIEnv* env, jclass, jstring jemail, jstring jpassword, jstring jdevice)
{
  const std::string email = GetJString(env, jemail);
  Api::SetDeviceName(GetJString(env, jdevice));
  const Api::LoginResult result = Api::SignInAccountOnly(email, GetJString(env, jpassword));
  if (!result.ok)
    return Text(env, SignInError(result.error));

  // The profile call reads the stored bearer, so keep it first, then keep it again under the
  // account's own name.
  Account::SaveBearerOnly(email, result.bearer);
  std::string name = email;
  const Api::Profile profile = Api::GetProfile();
  if (profile.ok && !profile.name.empty())
  {
    name = profile.name;
    Account::SaveBearerOnly(name, result.bearer);
  }
  return Text(env, json{{"ok", true}, {"name", name}});
}

// Presence ends now: the token is forgotten here and revoked on the server.
JNIEXPORT void JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_signOut(
    JNIEnv*, jclass)
{
  const std::string bearer = Account::GetBearer();
  Account::Clear();
  if (!bearer.empty())
    Api::RevokeToken(bearer);
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_profile(
    JNIEnv* env, jclass)
{
  const Api::Profile p = Api::GetProfile();
  if (!p.ok)
    return Text(env, json{{"ok", false}, {"error", p.error}});
  return Text(env, json{{"ok", true},
                        {"name", p.name},
                        {"account_id", p.account_id},
                        {"friend_code", p.friend_code},
                        {"image", p.image_base64},
                        {"linked", p.linked_platforms}});
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_friends(
    JNIEnv* env, jclass)
{
  const Api::AccountFriends list = Api::GetAccountFriends();
  if (!list.ok)
    return Text(env, json{{"ok", false}, {"error", list.error}});
  json friends = json::array();
  for (const auto& f : list.friends)
    friends.push_back(FriendJson(f));
  return Text(env, json{{"ok", true}, {"friends", friends}});
}

// The account's Wii saves, with how the local copy of each stands.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_cloudSaves(JNIEnv* env, jclass)
{
  const Api::CloudSaves list = Api::GetCloudSaves();
  if (!list.ok)
    return Text(env, json{{"ok", false}, {"error", list.error}});
  json saves = json::array();
  for (const auto& save : list.saves)
  {
    if (save.platform != PLATFORM)
      continue;
    json versions = json::array();
    for (const auto& v : save.versions)
    {
      versions.push_back(json{{"id", v.id},
                              {"number", v.number},
                              {"conflict", v.conflict},
                              {"size", v.size},
                              {"device", v.device},
                              {"saved_at", v.saved_at}});
    }
    json local = {{"state", "none"}, {"version", ""}, {"last_written", 0}};
    if (const auto id = ParseTitle(save.title_id))
    {
      const int newest = save.versions.empty() ? 0 : save.versions.front().number;
      const SaveSync::LocalCopy copy = SaveSync::Compare(WiiSaveDir(*id), newest);
      const char* state = "none";
      switch (copy.state)
      {
      case SaveSync::LocalState::NoLocal:
        state = "none";
        break;
      case SaveSync::LocalState::InStep:
        state = "in_step";
        break;
      case SaveSync::LocalState::ChangedHere:
        state = "changed_here";
        break;
      case SaveSync::LocalState::CloudNewer:
        state = "cloud_newer";
        break;
      case SaveSync::LocalState::NoHistory:
        state = "no_history";
        break;
      }
      local = {{"state", state}, {"version", copy.version}, {"last_written", copy.last_written}};
    }
    saves.push_back(json{{"title_id", save.title_id},
                         {"name", save.name},
                         {"versions", versions},
                         {"local", local}});
  }
  return Text(env, json{{"ok", true},
                        {"saves", saves},
                        {"used", list.allowance_used},
                        {"allowance", list.allowance}});
}

// Delete from cloud: every stored version of the title goes. Empty on success.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_deleteSave(JNIEnv* env, jclass,
                                                                               jlongArray jids)
{
  const jsize count = env->GetArrayLength(jids);
  std::vector<jlong> ids(static_cast<size_t>(count));
  env->GetLongArrayRegion(jids, 0, count, ids.data());
  for (const jlong id : ids)
  {
    const std::string error = Api::DeleteSaveVersion(static_cast<s64>(id));
    if (!error.empty())
      return ToJString(env, error);
  }
  return ToJString(env, "");
}

// Take the cloud's: the newest cloud copy replaces the local one (kept beside it).
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_downloadSave(JNIEnv* env,
                                                                                 jclass,
                                                                                 jstring jtitle)
{
  const auto id = ParseTitle(GetJString(env, jtitle));
  if (!id)
    return ToJString(env, "Not a title id.");
  return ToJString(env, SaveSync::Download(WiiSaveDir(*id), *id));
}

// Keep this machine's: the local copy goes up as the version after newest_cloud.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_uploadSave(JNIEnv* env, jclass,
                                                                               jstring jtitle,
                                                                               jint newest)
{
  const auto id = ParseTitle(GetJString(env, jtitle));
  if (!id)
    return ToJString(env, "Not a title id.");
  return ToJString(env, SaveSync::Upload(WiiSaveDir(*id), *id, newest));
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_hasLocalSave(JNIEnv* env,
                                                                                 jclass,
                                                                                 jstring jtitle)
{
  const auto id = ParseTitle(GetJString(env, jtitle));
  if (!id)
    return JNI_FALSE;
  std::error_code ec;
  return std::filesystem::is_directory(WiiSaveDir(*id), ec) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_mods(
    JNIEnv* env, jclass, jstring jtitle)
{
  json mods = json::array();
  for (const auto& mod : Api::GetMods(GetJString(env, jtitle)))
  {
    mods.push_back(json{{"id", mod.id},
                        {"name", mod.name},
                        {"version", mod.version},
                        {"author", mod.author},
                        {"licence", mod.licence},
                        {"summary", mod.summary}});
  }
  return Text(env, mods);
}

JNIEXPORT jstring JNICALL Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_status(
    JNIEnv* env, jclass)
{
  const Api::ServiceStatus service = Api::GetServiceStatus();
  const Api::NetworkStatus network = Api::GetStatus();
  json services = json::array();
  for (const auto& s : service.services)
  {
    services.push_back(json{{"group", s.group},
                            {"name", s.name},
                            {"up", s.up},
                            {"uptime", s.uptime},
                            {"latency", s.latency}});
  }
  json titles = json::array();
  for (const auto& [id, players] : network.titles)
    titles.push_back(json{{"id", id}, {"players", players}});
  json networks = json::array();
  for (const auto& [id, players] : network.networks)
    networks.push_back(json{{"id", id}, {"players", players}});
  return Text(env, json{{"ok", service.ok},
                        {"url", service.url},
                        {"headline", service.headline},
                        {"sub", service.sub},
                        {"state", service.state},
                        {"services", services},
                        {"players_ok", network.ok},
                        {"players", network.players_online},
                        {"titles", titles},
                        {"networks", networks}});
}

// Test connection: the round trip to the website and the NAT type the WFC NAT check sees.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_testConnection(JNIEnv* env,
                                                                                   jclass)
{
  json out;
  if (const auto ping = Api::PingBackend())
    out["ping_ms"] = *ping;
  if (const auto targets = openpak::nat::Targets(Core::OpenPakRedirect::ServerAddress()))
  {
    const openpak::nat::Result nat = openpak::nat::Run(targets->first, targets->second);
    out["nat"] = std::string(1, nat.Type());
  }
  return Text(env, out);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_refreshNetwork(JNIEnv* env,
                                                                                   jclass)
{
  const auto applied = openpak::NetworkProfile::Refresh(PLATFORM);
  ApplyNetworkProfile(applied);
  return ToJString(env, NetworkSummary(applied));
}

// The stored profile, without asking anybody.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_networkSummary(JNIEnv* env,
                                                                                   jclass)
{
  const auto stored = openpak::NetworkProfile::LoadStored(PLATFORM);
  if (!stored)
    return ToJString(env, openpak::NetworkProfile::SourceName(
                              openpak::NetworkProfile::Source::BuiltIn));
  return ToJString(env, fmt::format("v{} ({})", stored->version,
                                    openpak::NetworkProfile::SourceName(
                                        openpak::NetworkProfile::Source::Cached)));
}

// The running Wii title, 16 hex digits, or empty.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_runningTitle(JNIEnv* env, jclass)
{
  if (!Core::IsRunning(Core::System::GetInstance()))
    return ToJString(env, "");
  const u64 id = SConfig::GetInstance().GetTitleID();
  return ToJString(env, id == 0 ? std::string{} : TitleHex(id));
}

// Skip on the "Checking cloud save..." line: boot now on the local save.
JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_openpak_model_OpenPakNative_skipCloudPull(JNIEnv*, jclass)
{
  std::shared_ptr<Pull> pull;
  {
    std::lock_guard lock(s_pull_mutex);
    pull = s_pull;
  }
  if (!pull)
    return;
  std::lock_guard lock(pull->mutex);
  pull->skipped = true;
  pull->wanted = false;
  pull->cv.notify_all();
}

}  // extern "C"
