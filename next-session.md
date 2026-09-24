# Next session — dolphin

Updated 2026-09-24.

Upstream Dolphin (Wii) plus the OpenPak WFC redirect, the launch-time network profile, and a
website sign-in that exists to name cloud saves (WFC minted no console identity). Latest tag
`v0.2.4`; tags moved from `openpak-v*` to `v*.*.*` on 09-23 and CI builds only on those.

Current status 2026-09-24: `v0.2.4`. `v0.2.0` shipped WD-1/EP-2/EP-5, the release recipes,
the UX-spec menu, window, dialogs and settings (Wii family) and Android OpenPak (UX spec N3,
debug-signed APK); `v0.2.1`–`v0.2.4` are Windows/macOS CI fixes and the signed redirect
ceiling (openpak-client e180a57).

## Where things stand

- WFC redirect checkbox (Config → Wii); redirect list and server come from the network
  profile (EP-2, EP-5): fetched, validated against the compiled-in allowlist, else
  last-known-good, else built-in. Refresh button, source/version log line.
- Account + cloud saves (WD-1): store-only zip of the title's NAND save directory, upload on
  stop, pull on boot when the local directory is empty; conflicts resolve on openpak.org.
  Builds and links; not yet run against a game.
- Release recipes for every OS target upstream has (c274293).

## Next steps

- First WFC match on OpenPak (E5 gate): a Dolphin title matching against nn-wfc, beside the
  stage-0 Gecko code from `nn-wfc-patcher-wii`.

## Pointers

- [`../prds/`](../prds/README.md) — emulator-wide PRDs (`emulators/prds/` in the workspace):
  emulator-integration-prd.md (E5), emulator-network-profile-prd.md.
- `OPENPAK.md` — this fork's own readme.

## Scratch (research and throwaway work)

Decompiles, Ghidra projects, dumps, exefs/romfs extracts, packet captures,
strace and emulator logs, probe harnesses: put them in
`~/REPOS/Openpak/scratch/<topic>`. That folder is a local mount of the media pool,
outside every repository, so nothing in it is committed. Never use `/tmp` (a
shared 15 GB RAM disk) or elsewhere on `/home` for this. Keys and signing
material never go there. Rule: `docs/playbooks/conventions.md` in the workspace.
