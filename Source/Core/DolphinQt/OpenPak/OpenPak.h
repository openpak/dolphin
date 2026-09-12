// OpenPak account and cloud saves for Dolphin (WD-1, prds/platform-wii-ds-prd.md).
// WFC has no accounts, so Dolphin's OpenPak account is a website sign-in and nothing
// more: it exists to put a name on cloud saves and to carry the bearer token they
// upload with. Wii NAND saves live in per-title directories under the emulated NAND,
// which map onto the saves service as one versioned blob per title id.
#pragma once

#include <QWidget>

namespace OpenPak
{
// One-time set-up: point the client library at the user's config and cache dirs,
// name this host's saves platform ("wii"), and watch emulation state so a title's
// save is pulled when a Wii game boots and pushed when it stops. Call once, after
// UICommon::Init(). Safe when the user never signs in: everything gates on it.
void Init();

// The Settings → Wii → OpenPak section: account row (who is signed in, sign in /
// sign out) and the cloud-save toggle. Parented to the pane; owned by Qt.
QWidget* CreateWiiPaneSection(QWidget* parent);
}  // namespace OpenPak

class OpenPakSignInDialog : public QWidget
{
  Q_OBJECT
public:
  explicit OpenPakSignInDialog(QWidget* parent = nullptr);

private:
  void CreateMainLayout();
  void OnSignInButtonClicked();
  void OnSignOutButtonClicked();
  void RefreshAccountState();

  class QLabel* m_account_status;
  class QLineEdit* m_email_edit;
  class QLineEdit* m_password_edit;
  class QLabel* m_status_label;
  class QPushButton* m_sign_in_button;
  class QPushButton* m_sign_out_button;
};
