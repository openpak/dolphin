// SPDX-License-Identifier: GPL-2.0-or-later

// OpenPak cloud saves around a Wii title's run, called from the emulation thread in
// MainAndroid.cpp. Every step is a no-op unless an account is signed in and
// "Sync cloud saves automatically" is on.

#pragma once

struct BootParameters;

namespace OpenPakNative
{
// Before BootCore: pulls the newest cloud copy of the title's save. Blocks for at most five
// seconds (or until the player taps Skip) and never stops the boot.
void BeforeBoot(const BootParameters& boot);

// Once the core runs: shows what the pull did as an on-screen message.
void AfterBoot();

// After Core::Shutdown: captures the save (local I/O) and uploads it off this thread.
void AfterShutdown();
}  // namespace OpenPakNative
