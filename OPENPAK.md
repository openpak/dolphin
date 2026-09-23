# Dolphin for OpenPak

Fork of upstream Dolphin with three additions: a way to point the emulated console at OpenPak,
cloud saves driven by the emulator, and the OpenPak menu and window every OpenPak emulator
shares (`emulators/prds/openpak-ux-spec.md`, Wii family). Everything else is upstream, merged as it moves.
Builds: `openpak-v*` tags publish a GitHub Release (`.github/workflows/openpak_release.yml`).
The save-sync and account work builds and links; not yet run against a game.

## WFC redirect (Settings → Wii)

A **Connect Nintendo WFC to OpenPak** checkbox in Config → Wii, mirrored in Settings → OpenPak
(same key, `Core/EnableOpenPak`). With it on, IOS resolves every
name on the applied redirect list to the OpenPak server (`Core/OpenPakServer` in Dolphin.ini),
instead of asking the host resolver. Apply the game's OpenPak stage-0 Gecko code from
`nn-wfc-patcher-wii` too; that is what switches the disc's NAS login to plain HTTP the way
WiiLink's patcher does.

The redirect list and the address are **not compiled in any more** (EP-2,
`emulators/prds/emulator-network-profile-prd.md`). At launch Dolphin fetches the network profile
(`openpak-client`, one conditional request, two seconds); what applies is:

1. the fetched profile (`Source/.../OpenPak/OpenPak.cpp` applies it to
   `Core::OpenPakRedirect`), validated against the compiled-in allowlist — the ceiling —
2. else the last-known-good stored profile (`config/openpak_network_profile.json`),
3. else the compiled-in list and the `OpenPakServer` setting, which are the fallback, not the
   truth.

**Refresh network settings** (Settings → OpenPak → Advanced) re-runs that off the UI thread,
without a restart, so a title on a new hostname works without a new build. Every launch logs which
source was used — fetched, cached or built-in — and the profile version.

## The OpenPak menu, window and settings (UX spec)

- **OpenPak menu**, immediately left of Help (`openpak::qt::AddOpenPakMenu`): *Sign in to
  OpenPak...* / *Signed in as {name}*, Friends, Invitations, Cloud saves, Mods, News, Status,
  *OpenPak settings...*, *OpenPak website*, *Sign out...*. Rebuilt each time it opens; sign in and
  sign out wait for the game to stop; with the WFC connection off the header opens the settings.
- **The OpenPak window** is the library's (`OpenPakAccountDialog`, `Family::Wii`): Account (no
  console identity: `account.no_identity`), Friends read-only with a link to manage them on
  openpak.org, Invitations and News show their not-here panels, Cloud saves (this platform's
  saves only; *Resolve...* opens the conflict dialog), Mods (the catalogue; installing waits for
  D5, Riivolution/texture packs), Status.
- **Sign-in** is the library dialog with a Device name (it names the machine on uploaded save
  versions, `Core/OpenPakDeviceName`); the request runs off the UI thread, errors inline.
  **Sign out** is confirmed and revokes the token. First interactive launch without a game asks
  *Connect to OpenPak?* once (`Core/OpenPakConnectAsked`); signing in from it turns on the WFC
  connection and cloud sync.
- **Settings → OpenPak** (a pane after Wii): the WFC switch, the account row, *Open OpenPak...*,
  *Sync cloud saves automatically...* (`Core/OpenPakCloudSave`, default on), *Show
  notifications* and *Notification corner* (`Core/OpenPakNotifications`,
  `Core/OpenPakNotificationCorner`), and a collapsed Advanced with the Website
  (`Core/OpenPakWebsite`, read at launch) and *Refresh network settings*.
- **Toasts** (library toast, 6 s): signed in / out, an expired stored sign-in, cloud save pulled,
  pushed, push failed and conflict, and friends coming online or asking (the website's friend
  list, polled every 30 s).

## Cloud saves

WFC has no accounts, so Dolphin's OpenPak account is a website sign-in and nothing more
(`SignInAccountOnly`): it names the player and carries the bearer token saves upload with.

A Wii title's save is its NAND directory (`User/Wii/title/<title id>/data`), one versioned blob
per title id under platform `wii`, synced by the library's `SaveSync` as Ryujinx does it: right
before a Wii title boots, the newest cloud copy comes down when the local copy is not already in
step (the previous local copy is kept beside it as `data.openpak-backup`), behind a "Checking
cloud save..." dialog with Skip that gives up after five seconds; when the game stops, the save
goes up off the UI thread. A local save and a cloud save with no shared history are a conflict:
the game starts on the local save, a toast says so, automatic sync for that title pauses, and
the Cloud saves page's *Resolve...* chooses. Server side: `/api/v1/me/saves/{platform}/{title}`
(`saves/prds/cloud-saves-prd.md` CS-06, platform ids S-1).

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
