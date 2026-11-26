// pty_excl.hpp: a client that leaves a pty slave exclusive (TIOCEXCL) must not lock the next one out.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The exclusive flag lives on the tty, not on the opener's descriptor, and the tty stays alive while
// Signum holds its own slave descriptor (it does, so the master never sees EIO between sessions). A
// client that sets TIOCEXCL and is killed, or exits on an error path without TIOCNXCL, therefore leaves
// the flag set for good, and the next open fails with EBUSY ("serial port busy").
// Any descriptor on the tty may clear it (TIOCNXCL), so Signum clears it through its own slave
// descriptor, but only when no other process has the slave open (a live client keeps its protection).
#pragma once
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace signum {

// Is the exclusive flag set on the tty behind fd? (TIOCGEXCL, Linux 3.8+; false if unsupported.)
inline bool pty_excl_set(int fd)
{
#ifdef TIOCGEXCL
	int v = 0;
	return ::ioctl(fd, TIOCGEXCL, &v) == 0 && v != 0;
#else
	(void)fd;
	return false;
#endif
}

// Does a process other than this one have `slave_path` (e.g. /dev/pts/5) open? Scans /proc/*/fd; a
// process whose descriptors cannot be read (another user's) counts as absent.
inline bool pty_other_opener(const std::string &slave_path)
{
	DIR *proc = ::opendir("/proc");
	if (!proc) return true;                              // cannot tell: assume a client is there
	const std::string self = std::to_string(::getpid());
	bool found = false;
	char target[512];
	while (!found) {
		const dirent *e = ::readdir(proc);
		if (!e) break;
		if (e->d_name[0] < '0' || e->d_name[0] > '9' || self == e->d_name) continue;
		const std::string dir = std::string("/proc/") + e->d_name + "/fd";
		DIR *fds = ::opendir(dir.c_str());
		if (!fds) continue;
		while (const dirent *f = ::readdir(fds)) {
			if (f->d_name[0] == '.') continue;
			const ssize_t n = ::readlink((dir + "/" + f->d_name).c_str(), target, sizeof target - 1);
			if (n > 0 && slave_path == std::string(target, size_t(n))) { found = true; break; }
		}
		::closedir(fds);
	}
	::closedir(proc);
	return found;
}

// Clear a flag a departed client left. True when it was set and has been cleared.
inline bool pty_clear_leftover_excl(int slave_fd, const std::string &slave_path)
{
	if (!pty_excl_set(slave_fd) || pty_other_opener(slave_path)) return false;
#ifdef TIOCNXCL
	return ::ioctl(slave_fd, TIOCNXCL) == 0;
#else
	return false;
#endif
}

}  // namespace signum
