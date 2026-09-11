# Dolphin for OpenPak

Fork of upstream Dolphin with one addition: a way to point the emulated console at OpenPak.
Everything else is upstream, merged as it moves. Builds: `openpak-v*` tags publish a GitHub
Release (`.github/workflows/openpak_release.yml`). Not yet run against a game.

A **Connect Nintendo WFC to OpenPak** checkbox in Config → Wii. With it on, IOS resolves
every `*.nintendowifi.net`, `*.gamespy.com` and `*.openpak.org` name to the OpenPak server
(`Core/OpenPakServer` in Dolphin.ini, default the production box) instead of asking the host
resolver. Apply the game's OpenPak stage-0 Gecko code from `nn-wfc-patcher-wii` too; that is
what switches the disc's NAS login to plain HTTP the way WiiLink's patcher does.

Server side: the OpenPak network (`account`, `nn-account`, `nn-friends`, `nn-nncs`, `nn-boss`,
`nn-juxtaposition`, `nn-soap` for Wii U and 3DS; `nn-wfc` for Wii and DS) answers both the
Nintendo names and the `openpak.org` names behind one TLS front.
