# Next session — dolphin

Updated 2026-09-15.

Upstream Dolphin (Wii) plus the OpenPak WFC redirect, the launch-time network profile, and a
website sign-in that exists to name cloud saves (WFC minted no console identity). Released
through `openpak-v0.1.1`; the cloud-save/profile/sign-in work and the full-OS release recipes
are committed but untagged.

## Where things stand

- WFC redirect checkbox (Config → Wii); redirect list and server come from the network
  profile (EP-2, EP-5): fetched, validated against the compiled-in allowlist, else
  last-known-good, else built-in. Refresh button, source/version log line.
- Account + cloud saves (WD-1): store-only zip of the title's NAND save directory, upload on
  stop, pull on boot when the local directory is empty; conflicts resolve on openpak.org.
  Builds and links; not yet run against a game.
- Release recipes for every OS target upstream has (c274293). Untagged.

## Next steps

- Local build, cut `openpak-v0.1.2` with WD-1/EP-2/EP-5.
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
