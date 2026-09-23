#include "DolphinQt/OpenPak/OpenPak.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <variant>

#include <fmt/format.h>

#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/IOS/Network/IP/OpenPakRedirect.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DiscIO/VolumeDisc.h"
#include "DiscIO/VolumeWad.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"
#include "UICommon/GameFile.h"

#include <openpak/account.h>
#include <openpak/api.h>
#include <openpak/network_profile.h>
#include <openpak/platform.h>
#include <openpak/qt/account_dialog.h>
#include <openpak/qt/avatar_cache.h>
#include <openpak/qt/friend_notifier.h>
#include <openpak/qt/host.h>
#include <openpak/qt/host_kit.h>
#include <openpak/qt/prompts.h>
#include <openpak/qt/sign_in_dialog.h>
#include <openpak/qt/toast.h>
#include <openpak/save_sync.h>

namespace OpenPak
{
namespace
{
namespace Api = WebService::OpenPakApi;
using Kind = NextendoToast::Kind;

QString Tr(const char* text)
{
  return QCoreApplication::translate("OpenPak", text);
}

bool GameRunning()
{
  return !Core::IsUninitialized(Core::System::GetInstance());
}

bool SignedIn()
{
  return Common::OpenPakAccount::HasBearer();
}

std::filesystem::path WiiSaveDir(u64 title_id)
{
  return std::filesystem::path(
      Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Configured));
}

// Applies a fetched (or cached) profile to the resolver: redirect suffixes and, when the
// profile carries one, the server address. The built-in source leaves the compiled-in list
// and MAIN_WII_OPENPAK_SERVER alone -- they are the fallback, not a second copy.
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

QString SourceWord(openpak::NetworkProfile::Source source)
{
  switch (source)
  {
  case openpak::NetworkProfile::Source::Fetched:
    return Tr("fetched");
  case openpak::NetworkProfile::Source::Cached:
    return Tr("cached");
  case openpak::NetworkProfile::Source::BuiltIn:
    return Tr("built in");
  }
  return {};
}

QString NetworkStatusLine(long long version, openpak::NetworkProfile::Source source)
{
  const QString what = source == openpak::NetworkProfile::Source::BuiltIn ?
                           SourceWord(source) :
                           Tr("version %1, %2").arg(version).arg(SourceWord(source));
  return Tr("Network settings: %1.").arg(what);
}

// The Qt library's view of Dolphin: a Wii host with one identity and no console account.
class DolphinHost final : public openpak::qt::Host
{
public:
  using Host::Host;

  const GameListModel* games = nullptr;
  QWidget* main_window = nullptr;
  std::function<void()> sign_in;

  openpak::qt::Family GetFamily() const override { return openpak::qt::Family::Wii; }
  bool IsLinked() const override { return SignedIn(); }
  void SignIn() override
  {
    if (sign_in)
      sign_in();
  }
  void SignOut() override;
  void RefreshFriendCache() override {}
  void NotifyFriendRequestSent(const QString&) override {}
  QString JoinFriendSession(u64) override { return {}; }
  void EnsureChatConnected() override {}
  NextendoChatClient* GetChatClient() override { return nullptr; }

  std::shared_ptr<const UICommon::GameFile> FindGame(u64 title_id) const
  {
    if (!games || title_id == 0)
      return nullptr;
    for (int i = 0; i < games->rowCount(QModelIndex()); ++i)
    {
      auto game = games->GetGameFile(i);
      if (game && game->GetTitleID() == title_id && DiscIO::IsWii(game->GetPlatform()))
        return game;
    }
    return nullptr;
  }

  static u64 IdOf(const std::string& hex)
  {
    u64 id = 0;
    const auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
    return error == std::errc{} && end == hex.data() + hex.size() ? id : 0;
  }

  QString ResolveGameName(const std::string& app_id_hex,
                          const std::string& hint_name) const override
  {
    if (const auto game = FindGame(IdOf(app_id_hex)))
      return QString::fromStdString(
          game->GetName(UICommon::GameFile::Variant::LongAndPossiblyCustom));
    return QString::fromStdString(hint_name);
  }

  QString ResolveGameIcon(const std::string& app_id_hex) const override
  {
    const auto game = FindGame(IdOf(app_id_hex));
    if (!game)
      return {};
    const UICommon::GameBanner& banner = game->GetBannerImage();
    if (banner.buffer.empty() || banner.width == 0 || banner.height == 0)
      return {};
    const QImage image(reinterpret_cast<const uchar*>(banner.buffer.data()),
                       static_cast<int>(banner.width), static_cast<int>(banner.height),
                       QImage::Format_ARGB32);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1(png.toBase64());
  }

  std::string GetLocalAppId() const override
  {
    auto& system = Core::System::GetInstance();
    if (Core::IsUninitialized(system) || !system.IsWii())
      return {};
    const u64 title_id = SConfig::GetInstance().GetTitleID();
    return title_id == 0 ? std::string{} : fmt::format("{:016x}", title_id);
  }

  void QuickStart(u64) override {}
  void ManualSaveDownload(u64) override {}
  std::filesystem::path SaveDirectory(u64 title_id) override { return WiiSaveDir(title_id); }

  std::vector<Title> InstalledTitles() const override
  {
    std::vector<Title> titles;
    std::set<u64> seen;
    if (!games)
      return titles;
    for (int i = 0; i < games->rowCount(QModelIndex()); ++i)
    {
      const auto game = games->GetGameFile(i);
      if (!game || !DiscIO::IsWii(game->GetPlatform()) || game->GetTitleID() == 0 ||
          !seen.insert(game->GetTitleID()).second)
      {
        continue;
      }
      titles.push_back({game->GetTitleID(),
                        QString::fromStdString(game->GetName(
                            UICommon::GameFile::Variant::LongAndPossiblyCustom))});
    }
    return titles;
  }

  QString AccentColor() const override
  {
    return qApp->palette().color(QPalette::Highlight).name();
  }
  bool IsDarkTheme() const override { return Settings::Instance().IsThemeDark(); }
  bool NotificationsEnabled() const override
  {
    return Config::Get(Config::MAIN_OPENPAK_NOTIFICATIONS);
  }
  void SetNotificationsEnabled(bool enabled) override
  {
    Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_NOTIFICATIONS, enabled);
  }
  // The setting lists Bottom right, Bottom left, Top right, Top left; the toast counts
  // TopRight, TopLeft, BottomRight, BottomLeft.
  int NotificationCorner() const override
  {
    static constexpr int kToToast[] = {2, 3, 0, 1};
    return kToToast[std::clamp(Config::Get(Config::MAIN_OPENPAK_NOTIFICATION_CORNER), 0, 3)];
  }
  void SetNotificationCorner(int corner) override
  {
    static constexpr int kFromToast[] = {2, 3, 0, 1};
    Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_NOTIFICATION_CORNER,
                             kFromToast[std::clamp(corner, 0, 3)]);
  }
  bool RedirectEnabled() const override { return Config::Get(Config::MAIN_WII_OPENPAK_ENABLE); }
  bool CloudSyncEnabled() const override { return Config::Get(Config::MAIN_OPENPAK_CLOUD_SAVE); }
  void SetCloudSyncEnabled(bool enabled) override
  {
    Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_CLOUD_SAVE, enabled);
  }
  std::string ServerIp() const override { return Core::OpenPakRedirect::ServerAddress(); }
  std::string NatIp() const override { return {}; }
  void SetGuestInputSuspended(bool) override {}
  openpak::qt::Navigation* CreateNavigation(QObject*) override { return nullptr; }

  void AccountChanged(bool linked)
  {
    if (linked)
      emit AccountLinked();
    else
      emit AccountUnlinked();
  }
};

DolphinHost* g_host = nullptr;
NextendoToast* g_toast = nullptr;
openpak::qt::FriendNotifier* g_friends = nullptr;
std::function<void()> g_open_settings;
std::optional<QPixmap> g_avatar; // not a QPixmap: no QPixmap before QApplication

// The title this run is about, captured before it boots (or when it starts, for a boot that
// did not come through the main window), so the push on stop cannot race the config's
// teardown; and the titles whose automatic sync waits for a conflict to be resolved.
u64 g_run_title = 0;
std::set<u64> g_paused;

void Toast(const QString& text, Kind kind)
{
  if (g_toast)
    g_toast->Show(text, {}, {}, kind);
}

QString GameNameOf(u64 title_id)
{
  QString name = g_host ? g_host->ResolveGameName(fmt::format("{:016x}", title_id), {}) : QString{};
  return name.isEmpty() ? QString::fromStdString(fmt::format("{:016X}", title_id)) : name;
}

void LoadAvatar()
{
  g_avatar.reset();
  if (!SignedIn())
    return;
  std::thread([] {
    auto profile = std::make_shared<Api::Profile>(Api::GetProfile());
    QueueOnObject(g_host, [profile] {
      if (!profile->ok || profile->image_base64.empty())
        return;
      const QPixmap source = Nextendo::AvatarCache::Get("self", profile->image_base64, 64);
      if (source.isNull())
        return;
      QPixmap round(20, 20);
      round.fill(Qt::transparent);
      QPainter painter(&round);
      painter.setRenderHint(QPainter::Antialiasing);
      QPainterPath clip;
      clip.addEllipse(0, 0, 20, 20);
      painter.setClipPath(clip);
      painter.drawPixmap(0, 0, 20, 20, source);
      g_avatar = round;
    });
  }).detach();
}

void AfterSignIn()
{
  Toast(Tr("Signed in as %1.").arg(QString::fromStdString(Common::OpenPakAccount::GetUsername())),
        Kind::Account);
  LoadAvatar();
  if (g_friends)
    g_friends->Reset();
  g_host->AccountChanged(true);
}

// Sign in to OpenPak (UX spec §3.3). The request runs off the UI thread; an error keeps the
// dialog open. True when it signed in.
bool ShowSignIn(QWidget* parent)
{
  if (GameRunning())
    return false;
  OpenPakSignInDialog dialog(parent);
  const std::string device = Config::Get(Config::MAIN_OPENPAK_DEVICE_NAME);
  dialog.SetDeviceName(device.empty() ? openpak::qt::DefaultDeviceName(QStringLiteral("Dolphin")) :
                                        QString::fromStdString(device));
  dialog.SetSubmitter(&openpak::qt::SignInAccountOnly);
  if (dialog.exec() != QDialog::Accepted || !SignedIn())
    return false;
  Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_DEVICE_NAME, dialog.DeviceName().toStdString());
  AfterSignIn();
  return true;
}

void DolphinHost::SignOut()
{
  openpak::qt::SignOutAndRevoke();
  g_avatar.reset();
  Toast(Tr("Signed out of OpenPak."), Kind::Account);
  if (g_friends)
    g_friends->Reset();
  AccountChanged(false);
}

void OpenWindow(int page)
{
  OpenPakAccountDialog dialog(g_host, g_host->main_window, page);
  dialog.exec();
}

// The connect prompt (UX spec §3.2), once per install on a plain interactive launch.
void MaybeAskToConnect()
{
  if (Config::Get(Config::MAIN_OPENPAK_CONNECT_ASKED))
    return;
  Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_CONNECT_ASKED, true);
  if (SignedIn())
    return;
  if (!openpak::qt::AskToConnect(g_host->main_window, openpak::qt::Family::Wii))
    return;
  if (ShowSignIn(g_host->main_window))
  {
    Config::SetBaseOrCurrent(Config::MAIN_WII_OPENPAK_ENABLE, true);
    Config::SetBaseOrCurrent(Config::MAIN_OPENPAK_CLOUD_SAVE, true);
  }
}

// A stored sign-in the server no longer honours starts offline, with a toast (UX spec §5.1).
void CheckStoredSignIn()
{
  if (!SignedIn())
    return;
  std::thread([] {
    auto profile = std::make_shared<Api::Profile>(Api::GetProfile());
    QueueOnObject(g_host, [profile] {
      if (!profile->ok && !SignedIn())
      {
        Toast(Tr("The OpenPak sign-in for %1 has expired. Sign in again from the OpenPak menu.")
                  .arg(QStringLiteral("Dolphin")),
              Kind::Account);
        g_host->AccountChanged(false);
        return;
      }
      if (profile->ok && !profile->name.empty() &&
          profile->name != Common::OpenPakAccount::GetUsername())
      {
        Common::OpenPakAccount::SaveBearerOnly(profile->name,
                                               Common::OpenPakAccount::GetBearer());
      }
    });
  }).detach();
  LoadAvatar();
}

std::optional<u64> WiiTitleOf(const BootParameters& parameters)
{
  if (const auto* disc = std::get_if<BootParameters::Disc>(&parameters.parameters))
  {
    if (disc->volume && disc->volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
      return disc->volume->GetTitleID();
    return std::nullopt;
  }
  if (const auto* wad = std::get_if<DiscIO::VolumeWAD>(&parameters.parameters))
    return wad->GetTitleID();
  if (const auto* nand = std::get_if<BootParameters::NANDTitle>(&parameters.parameters))
    return nand->id;
  return std::nullopt;
}

void OnEmulationStateChanged(Core::State state)
{
  auto& system = Core::System::GetInstance();
  if (state == Core::State::Running && g_run_title == 0 && system.IsWii())
  {
    g_run_title = SConfig::GetInstance().GetTitleID();
    return;
  }
  if (state != Core::State::Uninitialized || g_run_title == 0)
    return;

  const u64 title_id = std::exchange(g_run_title, 0);
  if (!Config::Get(Config::MAIN_OPENPAK_CLOUD_SAVE) || !SignedIn() || g_paused.contains(title_id))
    return;
  const std::filesystem::path dir = WiiSaveDir(title_id);
  // Local I/O now, before anything else touches the NAND; the network after, off this thread.
  std::vector<u8> zip = Nextendo::SaveSync::CaptureOnExit(dir, title_id);
  if (zip.empty())
    return;
  const QString name = GameNameOf(title_id);
  std::thread([dir, title_id, name, zip = std::move(zip)]() mutable {
    const std::string error = Nextendo::SaveSync::PushCaptured(dir, title_id, std::move(zip));
    QueueOnObject(g_host, [name, error] {
      if (error.empty())
        Toast(Tr("Save for %1 uploaded to OpenPak.").arg(name), Kind::Saves);
      else
        Toast(Tr("The save for %1 did not upload: %2").arg(name, QString::fromStdString(error)),
              Kind::Saves);
    });
  }).detach();
}
}  // namespace

void Init()
{
  openpak::Platform::SetDirectories(File::GetUserPath(D_CONFIG_IDX),
                                    File::GetUserPath(D_CACHE_IDX));
  openpak::Platform::SetClient("dolphin", Common::GetScmDescStr());
  // The website is read once, before the first request (the library keeps it for the run).
  if (const std::string website = Config::Get(Config::MAIN_OPENPAK_WEBSITE); !website.empty())
    qputenv("OPENPAK_API", QByteArray::fromStdString(website));
  Api::SetSavesPlatform("wii");
  if (const std::string device = Config::Get(Config::MAIN_OPENPAK_DEVICE_NAME); !device.empty())
    Api::SetSaveDevice(device);

  // One conditional request at launch (never per game), off the UI thread: what is OpenPak,
  // and what should this emulator send it? Offline it keeps the last-known-good or the
  // compiled-in list.
  std::thread([] { ApplyNetworkProfile(openpak::NetworkProfile::Fetch("wii")); }).detach();
}

void Attach(QWidget* main_window, QMenuBar* menu_bar, const GameListModel* games,
            bool interactive, std::function<void()> open_settings)
{
  g_host = new DolphinHost(main_window);
  g_host->games = games;
  g_host->main_window = main_window;
  g_host->sign_in = [main_window] { ShowSignIn(main_window); };
  openpak::qt::Host::SetCurrent(g_host);
  g_open_settings = std::move(open_settings);

  g_toast = new NextendoToast(main_window);
  QObject::connect(g_toast, &NextendoToast::clicked, g_host, [](NextendoToast::Kind kind) {
    switch (kind)
    {
    case Kind::Online:
    case Kind::Offline:
    case Kind::Request:
      if (SignedIn())
        OpenWindow(OpenPakAccountDialog::kFriendsPage);
      break;
    case Kind::Saves:
      if (SignedIn())
        OpenWindow(OpenPakAccountDialog::kCloudSavesPage);
      break;
    case Kind::Account:
      if (SignedIn())
        OpenWindow(OpenPakAccountDialog::kAccountPage);
      else
        ShowSignIn(g_host->main_window);
      break;
    default:
      break;
    }
  });
  g_friends = new openpak::qt::FriendNotifier(g_toast, g_host);

  openpak::qt::MenuHooks hooks;
  hooks.game_running = &GameRunning;
  hooks.openpak_on = [] { return Config::Get(Config::MAIN_WII_OPENPAK_ENABLE); };
  hooks.signed_in_as = [] {
    return SignedIn() ? QString::fromStdString(Common::OpenPakAccount::GetUsername()) : QString{};
  };
  hooks.avatar = [] { return g_avatar.value_or(QPixmap{}); };
  hooks.sign_in = [main_window] { ShowSignIn(main_window); };
  hooks.sign_out = [] { g_host->SignOut(); };
  hooks.open_window = &OpenWindow;
  hooks.open_settings = [] {
    if (g_open_settings)
      g_open_settings();
  };
  openpak::qt::AddOpenPakMenu(menu_bar, std::move(hooks));

  QObject::connect(&Settings::Instance(), &Settings::EmulationStateChanged, g_host,
                   &OnEmulationStateChanged);

  CheckStoredSignIn();
  if (interactive)
    QTimer::singleShot(0, g_host, &MaybeAskToConnect);
}

void BeforeBoot(QWidget* parent, const BootParameters& parameters)
{
  g_run_title = 0;
  const std::optional<u64> title = WiiTitleOf(parameters);
  if (!title || *title == 0)
    return;
  g_run_title = *title;
  if (!Config::Get(Config::MAIN_OPENPAK_CLOUD_SAVE) || !SignedIn())
    return;

  const u64 title_id = *title;
  const std::filesystem::path dir = WiiSaveDir(title_id);
  auto waiting = std::make_shared<std::atomic<bool>>(true);
  auto outcome = std::make_shared<std::atomic<int>>(-1);
  std::thread([dir, title_id, waiting, outcome] {
    const auto result = Nextendo::SaveSync::PullBeforeLaunch(dir, title_id,
                                                             [waiting] { return waiting->load(); });
    outcome->store(static_cast<int>(result));
  }).detach();

  // "Checking cloud save..." with Skip, for five seconds at most (UX spec §5.1).
  QDialog dialog(parent);
  dialog.setWindowTitle(Tr("OpenPak"));
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(Tr("Checking cloud save...")));
  auto* bar = new QProgressBar;
  bar->setRange(0, 0);
  bar->setTextVisible(false);
  layout->addWidget(bar);
  auto* buttons = new QDialogButtonBox;
  buttons->addButton(Tr("Skip"), QDialogButtonBox::RejectRole);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  QElapsedTimer clock;
  clock.start();
  QTimer poll;
  poll.setInterval(50);
  QObject::connect(&poll, &QTimer::timeout, &dialog, [&] {
    if (outcome->load() >= 0 || clock.elapsed() >= 5000)
      dialog.accept();
  });
  poll.start();
  // Most checks answer at once; the dialog only shows for one that does not.
  QTimer::singleShot(300, &dialog, [&dialog, outcome] {
    if (outcome->load() < 0)
      dialog.show();
  });
  QEventLoop loop;
  QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
  loop.exec();
  poll.stop();
  waiting->store(false);

  const int result = outcome->load();
  const QString name = GameNameOf(title_id);
  if (result == static_cast<int>(Nextendo::SaveSync::PullOutcome::Pulled))
  {
    g_paused.erase(title_id);
    Toast(Tr("Cloud save for %1 downloaded; the previous local copy was kept beside it.")
              .arg(name),
          Kind::Saves);
  }
  else if (result == static_cast<int>(Nextendo::SaveSync::PullOutcome::BothExist))
  {
    // Starts on the local save; automatic sync for this title waits for the choice.
    g_paused.insert(title_id);
    Toast(Tr("%1 has a save here and a different one in the cloud. Choose one on the Cloud "
             "saves page.")
              .arg(name),
          Kind::Saves);
  }
  else if (result == static_cast<int>(Nextendo::SaveSync::PullOutcome::Nothing))
  {
    g_paused.erase(title_id);
  }
}

QWidget* CreateSettingsPane()
{
  auto* pane = new QWidget;
  auto* layout = new QVBoxLayout(pane);
  layout->setContentsMargins(0, 0, 0, 0);

  // Account
  auto* account = new QGroupBox(Tr("Account"));
  auto* account_layout = new QVBoxLayout(account);
  auto* enable = new ConfigBool(Tr("Connect Nintendo WFC to OpenPak"),
                                Config::MAIN_WII_OPENPAK_ENABLE);
  enable->SetDescription(Tr("When this is off the emulator behaves exactly as upstream does: "
                            "offline, and nothing is sent anywhere."));
  account_layout->addWidget(enable);

  auto* account_row = new QHBoxLayout;
  auto* account_text = new QLabel;
  account_text->setWordWrap(true);
  auto* account_button = new QPushButton;
  account_row->addWidget(account_text, 1);
  account_row->addWidget(account_button);
  account_layout->addLayout(account_row);

  auto* open = new QPushButton(Tr("Open OpenPak..."));
  auto* open_row = new QHBoxLayout;
  open_row->addWidget(open);
  open_row->addStretch(1);
  account_layout->addLayout(open_row);

  auto* cloud = new ConfigBool(Tr("Sync cloud saves automatically when a game starts and stops"),
                               Config::MAIN_OPENPAK_CLOUD_SAVE);
  account_layout->addWidget(cloud);
  layout->addWidget(account);

  // Notifications
  auto* notifications = new QGroupBox(Tr("Notifications"));
  auto* notifications_layout = new QFormLayout(notifications);
  notifications_layout->addRow(
      new ConfigBool(Tr("Show notifications"), Config::MAIN_OPENPAK_NOTIFICATIONS));
  notifications_layout->addRow(
      Tr("Notification corner"),
      new ConfigChoice({Tr("Bottom right"), Tr("Bottom left"), Tr("Top right"), Tr("Top left")},
                       Config::MAIN_OPENPAK_NOTIFICATION_CORNER));
  layout->addWidget(notifications);

  // Advanced, collapsed
  auto* advanced_toggle = new QToolButton;
  advanced_toggle->setText(Tr("Advanced"));
  advanced_toggle->setCheckable(true);
  advanced_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  advanced_toggle->setArrowType(Qt::RightArrow);
  advanced_toggle->setAutoRaise(true);
  layout->addWidget(advanced_toggle);
  auto* advanced = new QWidget;
  auto* advanced_layout = new QFormLayout(advanced);
  auto* website = new ConfigText(Config::MAIN_OPENPAK_WEBSITE);
  website->setPlaceholderText(QStringLiteral("https://openpak.org"));
  website->setToolTip(Tr("Where the account lives and where you sign in. Leave this at "
                         "openpak.org unless you run your own deployment."));
  advanced_layout->addRow(Tr("Website"), website);
  auto* refresh = new QPushButton(Tr("Refresh network settings"));
  auto* network_status = new QLabel;
  network_status->setWordWrap(true);
  if (const auto stored = openpak::NetworkProfile::LoadStored("wii"))
    network_status->setText(
        NetworkStatusLine(stored->version, openpak::NetworkProfile::Source::Cached));
  else
    network_status->setText(NetworkStatusLine(0, openpak::NetworkProfile::Source::BuiltIn));
  auto* refresh_row = new QHBoxLayout;
  refresh_row->addWidget(refresh);
  refresh_row->addWidget(network_status, 1);
  advanced_layout->addRow(refresh_row);
  advanced->hide();
  layout->addWidget(advanced);
  QObject::connect(advanced_toggle, &QToolButton::toggled, advanced,
                   [advanced, advanced_toggle](bool open_) {
                     advanced->setVisible(open_);
                     advanced_toggle->setArrowType(open_ ? Qt::DownArrow : Qt::RightArrow);
                   });
  layout->addStretch(1);

  // No network on the UI thread: the refresh runs beside it and reports back.
  QObject::connect(refresh, &QPushButton::clicked, refresh, [refresh, network_status] {
    refresh->setEnabled(false);
    network_status->setText(Tr("Checking..."));
    QPointer<QLabel> status{network_status};
    QPointer<QPushButton> button{refresh};
    std::thread([status, button] {
      const auto applied = openpak::NetworkProfile::Refresh("wii");
      ApplyNetworkProfile(applied);
      QueueOnObject(qApp, [status, button, version = applied.profile.version,
                           source = applied.source] {
        if (status)
          status->setText(NetworkStatusLine(version, source));
        if (button)
          button->setEnabled(true);
      });
    }).detach();
  });

  const auto update = [enable, account_text, account_button] {
    const bool running = GameRunning();
    enable->setEnabled(!running);
    if (SignedIn())
    {
      account_text->setText(
          Tr("Signed in as %1").arg(QString::fromStdString(Common::OpenPakAccount::GetUsername())));
      account_button->setText(Tr("Sign out..."));
    }
    else
    {
      account_text->setText(Tr("Not signed in"));
      account_button->setText(Tr("Sign in..."));
    }
    account_button->setEnabled(!running);
    account_button->setToolTip(running ? Tr("Stop the running game first.") : QString{});
  };
  update();
  QObject::connect(account_button, &QPushButton::clicked, pane, [pane, update] {
    if (SignedIn())
    {
      if (openpak::qt::ConfirmSignOut(pane->window()))
        g_host->SignOut();
    }
    else
    {
      ShowSignIn(pane->window());
    }
    update();
  });
  QObject::connect(open, &QPushButton::clicked, pane,
                   [] { OpenWindow(OpenPakAccountDialog::kAccountPage); });
  QObject::connect(&Settings::Instance(), &Settings::EmulationStateChanged, pane,
                   [update](Core::State) { update(); });
  if (g_host)
  {
    QObject::connect(g_host, &openpak::qt::Host::AccountLinked, pane, update);
    QObject::connect(g_host, &openpak::qt::Host::AccountUnlinked, pane, update);
  }
  return pane;
}
}  // namespace OpenPak
