#include "DolphinQt/OpenPak/OpenPak.h"

#include <filesystem>
#include <thread>

#include <fmt/format.h>

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/IOS/Network/IP/OpenPakRedirect.h"
#include "Core/System.h"
#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Settings.h"

#include <openpak/account.h>
#include <openpak/api.h>
#include <openpak/network_profile.h>
#include <openpak/platform.h>
#include <openpak/zip_store.h>

namespace OpenPak
{
namespace
{
// Applies a fetched (or cached) profile to the resolver: redirect suffixes and, when the
// profile carries one, the server address. The built-in source leaves the compiled-in list
// and MAIN_WII_OPENPAK_SERVER alone — they are the fallback, not a second copy.
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

std::string ProfileStatusLine()
{
  const auto stored = openpak::NetworkProfile::LoadStored("wii");
  if (!stored)
    return "network profile: built-in";
  return fmt::format("network profile: v{} (cached)", stored->version);
}

// The title whose save this run is about, captured when emulation starts so the
// push-on-stop cannot race the config's teardown.
std::string g_run_title_hex;  // 16 lowercase hex digits; empty between runs
std::filesystem::path g_run_save_dir;

std::filesystem::path WiiSaveDir(u64 title_id)
{
  return std::filesystem::path(
      Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Configured));
}

bool DirHasContent(const std::filesystem::path& dir)
{
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
    return false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
  {
    if (entry.is_regular_file(ec))
      return true;
  }
  return false;
}

// Pulls the title's cloud save into its NAND data directory. A local save is
// never overwritten from the automatic path: the cloud copy is for a fresh
// machine; conflicts resolve on openpak.org, which keeps every version.
void PullInBackground(const std::string& title_hex, const std::filesystem::path& dir,
                      bool force)
{
  if (!Common::OpenPakAccount::HasBearer())
    return;
  if (!force && DirHasContent(dir))
  {
    INFO_LOG_FMT(COMMON, "OpenPak save pull {}: local save present, kept",
                 title_hex);
    return;
  }
  auto bytes = WebService::OpenPakApi::PullSave(title_hex);
  if (!bytes || bytes->empty())
    return;
  if (openpak::ZipStore::UnzipToDirectory(*bytes, dir))
    INFO_LOG_FMT(COMMON, "OpenPak save pull {}: applied ({} bytes)", title_hex,
                 bytes->size());
  else
    ERROR_LOG_FMT(COMMON,
                  "OpenPak save pull {}: cloud save is not a readable archive", title_hex);
}

void PushInBackground(const std::string& title_hex, const std::filesystem::path& dir)
{
  if (!Common::OpenPakAccount::HasBearer())
    return;
  auto zip = openpak::ZipStore::ZipDirectory(dir);
  if (zip.empty())
    return;  // the title never wrote a save
  const std::string error = WebService::OpenPakApi::PushSave(title_hex, zip);
  if (!error.empty())
    WARN_LOG_FMT(COMMON, "OpenPak save push {} failed: {}", title_hex, error);
  else
    INFO_LOG_FMT(COMMON, "OpenPak save push {}: {} bytes", title_hex,
                 zip.size());
}

void OnEmulationStateChanged(Core::State state)
{
  if (!Config::Get(Config::MAIN_OPENPAK_CLOUD_SAVE))
    return;

  if (state == Core::State::Running)
  {
    auto& core = Core::System::GetInstance();
    if (!core.IsWii())
      return;
    const u64 title_id = SConfig::GetInstance().GetTitleID();
    if (title_id == 0)
      return;
    g_run_title_hex = fmt::format("{:016x}", title_id);
    g_run_save_dir = WiiSaveDir(title_id);
    std::thread(PullInBackground, g_run_title_hex, g_run_save_dir, false).detach();
  }
  else if (state == Core::State::Uninitialized && !g_run_title_hex.empty())
  {
    std::string title_hex;
    std::swap(title_hex, g_run_title_hex);
    std::thread(PushInBackground, title_hex, g_run_save_dir).detach();
  }
}
}  // namespace

void Init()
{
  openpak::Platform::SetDirectories(File::GetUserPath(D_CONFIG_IDX),
                                    File::GetUserPath(D_CACHE_IDX));
  WebService::OpenPakApi::SetSavesPlatform("wii");

  // One conditional request at launch (never per game): what is OpenPak, and what should
  // this emulator send it? Offline it keeps the last-known-good or the compiled-in list.
  ApplyNetworkProfile(openpak::NetworkProfile::Fetch("wii"));

  QObject::connect(&Settings::Instance(), &Settings::EmulationStateChanged,
                   &Settings::Instance(),
                   [](Core::State state) { OnEmulationStateChanged(state); });
}

QWidget* CreateWiiPaneSection(QWidget* parent)
{
  auto* group = new QGroupBox(QObject::tr("OpenPak"), parent);
  auto* layout = new QGridLayout(group);

  auto* account_status = new QLabel(group);
  auto* account_button = new QPushButton(group);
  auto* cloud_checkbox =
      new ConfigBool(QObject::tr("Sync saves to OpenPak"), Config::MAIN_OPENPAK_CLOUD_SAVE, group);
  cloud_checkbox->SetDescription(
      QObject::tr("Uploads each Wii title's save to your OpenPak account when the game stops, and "
                  "downloads it on a machine with no local save. Every version is kept on "
                  "openpak.org.<br><br><dolphin_emphasis>If unsure, leave this "
                  "unchecked.</dolphin_emphasis>"));

  auto* profile_status = new QLabel(group);
  profile_status->setText(QString::fromStdString(ProfileStatusLine()));

  auto* refresh_profile_button = new QPushButton(QObject::tr("Refresh network settings"), group);
  QObject::connect(refresh_profile_button, &QPushButton::clicked, refresh_profile_button,
                   [profile_status] {
                     const auto applied = openpak::NetworkProfile::Refresh("wii");
                     ApplyNetworkProfile(applied);
                     profile_status->setText(
                         QString::fromStdString(
                             fmt::format("network profile: v{} ({})", applied.profile.version,
                                         openpak::NetworkProfile::SourceName(applied.source))));
                   });

  auto refresh = [account_status, account_button] {
    if (Common::OpenPakAccount::HasBearer())
    {
      account_status->setText(QObject::tr("Signed in as <b>%1</b>.")
                                  .arg(QString::fromStdString(
                                      Common::OpenPakAccount::GetUsername())));
      account_button->setText(QObject::tr("Sign out…"));
    }
    else
    {
      account_status->setText(QObject::tr("Not signed in."));
      account_button->setText(QObject::tr("Sign in…"));
    }
  };
  refresh();

  // The dialog deletes itself on close and re-refreshes the row through this
  // connection; the button outlives it as the receiver and context.
  auto open_dialog = [parent, group, refresh] {
    auto* dialog = new OpenPakSignInDialog(parent);
    QObject::connect(dialog, &QObject::destroyed, group,
                     [refresh] { refresh(); });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
  };
  QObject::connect(account_button, &QPushButton::clicked, group, open_dialog);

  layout->addWidget(account_status, 0, 0);
  layout->addWidget(account_button, 0, 1);
  layout->addWidget(cloud_checkbox, 1, 0, 1, 2);
  layout->addWidget(profile_status, 2, 0, 1, 1);
  layout->addWidget(refresh_profile_button, 2, 1, 1, 1);

  return group;
}
}  // namespace OpenPak

OpenPakSignInDialog::OpenPakSignInDialog(QWidget* parent) : QWidget(parent, Qt::Dialog)
{
  setWindowTitle(tr("Sign in to OpenPak"));
  CreateMainLayout();
  RefreshAccountState();
}

void OpenPakSignInDialog::CreateMainLayout()
{
  auto* layout = new QGridLayout(this);
  m_account_status = new QLabel(this);
  m_email_edit = new QLineEdit(this);
  m_email_edit->setPlaceholderText(tr("you@example.com"));
  m_password_edit = new QLineEdit(this);
  m_password_edit->setPlaceholderText(tr("Password"));
  m_password_edit->setEchoMode(QLineEdit::Password);
  m_status_label = new QLabel(this);
  m_sign_in_button = new QPushButton(tr("Sign in"), this);
  m_sign_out_button = new QPushButton(tr("Sign out"), this);
  m_sign_in_button->setDefault(true);

  layout->addWidget(m_account_status, 0, 0, 1, 2);
  layout->addWidget(new QLabel(tr("Email"), this), 1, 0);
  layout->addWidget(m_email_edit, 1, 1);
  layout->addWidget(new QLabel(tr("Password"), this), 2, 0);
  layout->addWidget(m_password_edit, 2, 1);
  layout->addWidget(m_status_label, 3, 0, 1, 2);
  layout->addWidget(m_sign_in_button, 4, 0);
  layout->addWidget(m_sign_out_button, 4, 1);

  connect(m_sign_in_button, &QPushButton::clicked, this,
          &OpenPakSignInDialog::OnSignInButtonClicked);
  connect(m_sign_out_button, &QPushButton::clicked, this,
          &OpenPakSignInDialog::OnSignOutButtonClicked);
}

void OpenPakSignInDialog::OnSignInButtonClicked()
{
  m_status_label->setText(tr("Signing in…"));
  auto result = WebService::OpenPakApi::SignInAccountOnly(m_email_edit->text().toStdString(),
                                                         m_password_edit->text().toStdString());
  if (!result.ok)
  {
    m_status_label->setText(QString::fromStdString(result.error));
    return;
  }
  Common::OpenPakAccount::SaveBearerOnly(m_email_edit->text().toStdString(), result.bearer);
  m_password_edit->clear();
  RefreshAccountState();
}

void OpenPakSignInDialog::OnSignOutButtonClicked()
{
  Common::OpenPakAccount::Clear();
  RefreshAccountState();
}

void OpenPakSignInDialog::RefreshAccountState()
{
  const bool has_bearer = Common::OpenPakAccount::HasBearer();
  m_email_edit->setEnabled(!has_bearer);
  m_password_edit->setEnabled(!has_bearer);
  m_sign_in_button->setEnabled(!has_bearer);
  m_sign_out_button->setEnabled(has_bearer);
  if (has_bearer)
  {
    m_account_status->setText(tr("Signed in as %1.").arg(
        QString::fromStdString(Common::OpenPakAccount::GetUsername())));
    m_status_label->setText(tr("Cloud saves sync under this account. WFC itself has no "
                               "accounts: your games' friend codes stay as they are."));
  }
  else
  {
    m_account_status->setText(tr("Not signed in."));
    m_status_label->clear();
  }
}
