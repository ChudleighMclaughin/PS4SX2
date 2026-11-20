// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace PS4ThreadAffinity
{
	// Pins the calling emulation worker to one of the six game CPU cores.
	// Create /data/PS4SX2/vm-no-thread-affinity to keep the system scheduler.
	// /data/PS4SX2/vm-cores.txt overrides a role's core, one "ROLE=CORE" per line
	// (EE, VU1, GS, Audio), e.g. "GS=1". Lines that don't match are ignored.
	void PinCurrentThread(const char* role, int core);

	// The role the calling thread passed to PinCurrentThread, or nullptr for any other thread
	// (main/menu thread, helpers). Async-signal-safe: used by the crash report.
	const char* CurrentRole();
} // namespace PS4ThreadAffinity
