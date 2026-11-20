// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Preserve the last game of each platform across launcher relaunches.
#pragma once

#include "GamePlatform.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PS4SessionLogs
{
	constexpr const char* MARKER = "/data/PS4SX2/logs/session-platform.txt";
	struct ArchiveResult
	{
		const char* directory = "previous-menu";
		int moved = 0;
		int error = 0;
	};
	inline ArchiveResult ArchivePrevious()
	{
		ArchiveResult result;
		char platform[16]{};
		if (FILE* file = std::fopen(MARKER, "rb"))
		{
			std::fread(platform, 1, sizeof(platform) - 1, file);
			std::fclose(file);
			if (std::strcmp(platform, "PS1") == 0)
				result.directory = "last-game-PS1";
			else if (std::strcmp(platform, "PS2") == 0)
				result.directory = "last-game-PS2";
			else
				result.directory = "last-game-other";
		}
		char directory[128];
		std::snprintf(directory, sizeof(directory), "/data/PS4SX2/logs/%s", result.directory);
		// Only these app-owned files rotate; other files and folders are untouched. crash.log is
		// here because ps4_crashlog writes it during the session being archived (MigrateLegacyLogs
		// has already moved it into logs/), so each archive keeps its crash with its logs.
		constexpr const char* names[] = {
			"PCSX2-vm-boot.log", "PCSX2-vm-stdout.log", "PCSX2-vm-stderr.log", "emulog.txt", "crash.log"};
		// (AI-assisted) Keep the last KEEP sessions per platform: <dir> is the newest, then
		// <dir>.2 ... <dir>.KEEP. Rotate only when there is a session to archive.
		constexpr int KEEP = 5;
		if (access("/data/PS4SX2/logs/PCSX2-vm-boot.log", F_OK) == 0 && access(directory, F_OK) == 0)
		{
			char older[144], newer[144];
			std::snprintf(older, sizeof(older), "%s.%d", directory, KEEP);
			for (const char* name : names)
			{
				char path[176];
				std::snprintf(path, sizeof(path), "%s/%s", older, name);
				unlink(path);
			}
			// rmdir fails if someone put their own files there; renames onto it then fail and
			// the archive falls back to the old one-session behaviour instead of losing them.
			rmdir(older);
			for (int index = KEEP - 1; index >= 2; --index)
			{
				std::snprintf(older, sizeof(older), "%s.%d", directory, index + 1);
				std::snprintf(newer, sizeof(newer), "%s.%d", directory, index);
				rename(newer, older);
			}
			std::snprintf(older, sizeof(older), "%s.2", directory);
			rename(directory, older);
		}
		if (mkdir(directory, 0777) != 0 && errno != EEXIST)
		{
			result.error = errno;
			return result;
		}
		for (const char* name : names)
		{
			char source[160], target[160];
			std::snprintf(source, sizeof(source), "/data/PS4SX2/logs/%s", name);
			std::snprintf(target, sizeof(target), "%s/%s", directory, name);
			if (rename(source, target) == 0)
				++result.moved;
			else if (errno == ENOENT)
				unlink(target); // Do not mix an old file into the new archived session.
			else
				result.error = errno;
		}
		if (!result.error)
			unlink(MARKER);
		return result;
	}
	inline bool MarkGame(GamePlatform platform)
	{
		const char* value = platform == GamePlatform::PS1 ? "PS1" :
			platform == GamePlatform::PS2 ? "PS2" : "Other";
		const int fd = open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return false;
		const auto size = std::strlen(value);
		const bool written = write(fd, value, size) == static_cast<ssize_t>(size);
		const bool synced = fsync(fd) == 0;
		close(fd);
		return written && synced;
	}
}
