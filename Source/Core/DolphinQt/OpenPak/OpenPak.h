// OpenPak for Dolphin: the OpenPak menu, the shared account window (openpak-client's Qt
// library, Wii family), the sign-in / connect / sign-out dialogs, the Settings pane and cloud
// saves, as emulators/prds/openpak-ux-spec.md has them for every emulator.
//
// WFC minted no accounts, so Dolphin's OpenPak account is a website sign-in and nothing more:
// it names the player and carries the bearer token cloud saves upload with. Wii NAND saves are
// per-title directories, which map onto the saves service as one versioned blob per title id.
#pragma once

#include <functional>

class GameListModel;
class QMenuBar;
class QWidget;
struct BootParameters;

namespace OpenPak
{
// Before the main window: where the client library keeps its files, which emulator this is, the
// website, the saves platform ("wii"), and the network profile, fetched off the UI thread. Call
// once, after UICommon::Init().
void Init();

// With the main window built: the OpenPak menu (inserted before Help), toasts, the friend and
// cloud-save notifications, the stored sign-in's check, and -- on a plain interactive launch,
// once per install -- the connect prompt. open_settings shows Settings at the OpenPak pane.
void Attach(QWidget* main_window, QMenuBar* menu_bar, const GameListModel* games,
            bool interactive, std::function<void()> open_settings);

// Right before a title boots: the newest cloud copy of its save lands first, with a "Checking
// cloud save..." dialog that gives up after five seconds or on Skip (never blocks a launch).
void BeforeBoot(QWidget* parent, const BootParameters& parameters);

// The Settings -> OpenPak pane (UX spec §3.13).
QWidget* CreateSettingsPane();
}  // namespace OpenPak
