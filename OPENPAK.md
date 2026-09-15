# Dolphin for OpenPak

Fork of upstream Dolphin with two additions: a way to point the emulated console at OpenPak,
and cloud saves driven by the emulator. Everything else is upstream, merged as it moves.
Builds: `openpak-v*` tags publish a GitHub Release (`.github/workflows/openpak_release.yml`).
The save-sync and account work builds and links; not yet run against a game.

## WFC redirect (Settings → Wii)

A **Connect Nintendo WFC to OpenPak** checkbox in Config → Wii. With it on, IOS resolves every
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

**Refresh network settings** in the OpenPak section of Config → Wii re-runs that without a
restart, so a title on a new hostname works without a new build. Every launch logs which
source was used — fetched, cached or built-in — and the profile version.

## OpenPak account and cloud saves (Settings → Wii → OpenPak)

WFC has no accounts, so Dolphin's OpenPak account is a website sign-in and nothing more: it
exists to put a name on cloud saves and to carry the bearer token they upload with
(`SignInAccountOnly`; no console identity is minted, because WFC minted none).

A **Sync saves to OpenPak** checkbox (per title id, all Wii titles): when the game stops, the
title's NAND save directory (`User/Wii/title/<title id>/data`) is zipped store-only and
uploaded to `saves` under platform `wii` and the title id as key; when a game boots on a
machine whose local save directory is empty, the cloud copy is downloaded and applied. A local
save is never overwritten by the automatic path — conflicts resolve on openpak.org, which
keeps every version. Server side: the same `/api/v1/me/saves/{platform}/{title}` the website
and phone app use (`saves/prds/cloud-saves-prd.md` CS-06, platform ids S-1).

The zip is `openpak::ZipStore` (store method, no compression, no dependencies); Wii NAND saves
are directory trees, so one versioned blob per title holds the whole folder. Entries the
reader does not understand are skipped; it never writes outside the target directory.

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
