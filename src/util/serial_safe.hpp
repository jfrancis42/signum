// serial_safe.hpp: opening a radio's serial port without keying the radio.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// Many radios key on RTS or DTR (an Icom with USB SEND = RTS, interfaces wired to either line). Linux
// raises RTS and DTR at every open of a serial port whose speed is not B0, O_NONBLOCK included, before
// the program gets control (tty_port_block_til_ready): clearing them right after the open still keys
// such a radio for an instant (15-23 ms measured on two radios). At B0 the open raises nothing.
// What the kernel's drivers do (read in Linux 6.18):
//   - USB serial chips (cp210x, ftdi_sio, ch341, pl2303, cdc-acm) set no baud rate at B0: the chip
//     keeps the rate it had, so the port works at its last rate while the speed reads B0. Going to B0
//     drops DTR/RTS; going from B0 to a rate raises them again (the driver, inside tcsetattr).
//   - Native UARTs (serial_core) run at 9600 at B0 and also raise DTR/RTS when leaving B0.
//   - A change from one rate to another touches neither line.
// So a USB serial port is used at B0 and left at B0 (it is ready for the next open, ours or another
// program's); its rate is set only while the speed is not B0 anyway (an open that already raised the
// lines), or, when the chip's rate is wrong, by leaving B0 once (a short key-up, reported). The lines are
// cleared before anything else after an open, after every termios change and on close. HUPCL is set:
// the last close of the port (a crash, SIGKILL) lowers both lines too.
//
// SerialSys holds the OS calls; a test can replace it.
#pragma once

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <climits>
#include <string>

namespace util {

struct SerialSys {
	virtual ~SerialSys() = default;
	virtual int open(const char *path, int flags) { return ::open(path, flags); }
	virtual int close(int fd) { return ::close(fd); }
	virtual int modem(int fd, unsigned long req, int bits) { return ::ioctl(fd, req, &bits); }   // TIOCMBIS / TIOCMBIC
	virtual int get(int fd, termios &t) { return ::tcgetattr(fd, &t); }
	virtual int set(int fd, const termios &t) { return ::tcsetattr(fd, TCSANOW, &t); }
	virtual ssize_t read(int fd, void *b, size_t n) { return ::read(fd, b, n); }
	virtual ssize_t write(int fd, const void *b, size_t n) { return ::write(fd, b, n); }
	// A USB serial port (the chip keeps its rate at B0): /sys/class/tty/NAME/device lies under a USB device.
	virtual bool usb(const std::string &path)
	{
		char dev[PATH_MAX], sys[PATH_MAX];
		if (!::realpath(path.c_str(), dev)) return false;
		const std::string d(dev);
		const std::string link = "/sys/class/tty/" + d.substr(d.rfind('/') + 1) + "/device";
		if (!::realpath(link.c_str(), sys)) return false;
		return std::string(sys).find("/usb") != std::string::npos;
	}
	static SerialSys &os() { static SerialSys s; return s; }
};

inline speed_t serial_speed(int baud)
{
	switch (baud) {
	case 1200: return B1200;   case 2400: return B2400;   case 4800: return B4800;   case 9600: return B9600;
	case 38400: return B38400; case 57600: return B57600; case 115200: return B115200;
	default: return B19200;
	}
}

class SafePort {
public:
	explicit SafePort(SerialSys *s = nullptr) : sys_(s ? s : &SerialSys::os()) {}
	~SafePort() { close(); }
	SafePort(const SafePort &) = delete;
	SafePort &operator=(const SafePort &) = delete;
	void use(SerialSys *s) { sys_ = s ? s : &SerialSys::os(); }

	// Raw, non-blocking. baud 0: only the lines are used (keying by RTS/DTR): no rate is set. False when
	// the port cannot be opened or is not a terminal. note() says what this open did to the lines.
	// hold: a USB port that was not at B0 (this open raised the lines already) stays at its rate, so the
	// caller can try rates (change_rate() moves no line) before to_b0().
	bool open(const std::string &path, int baud, bool hold = false)
	{
		close();
		note_.clear(); baud_ = baud; rate_unknown_ = false;
		fd_ = sys_->open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
		if (fd_ < 0) { note_ = path + ": cannot open"; return false; }
		low();                                           // first: before anything else can happen
		termios t;
		if (sys_->get(fd_, t) != 0) { note_ = path + ": not a serial port"; sys_->close(fd_); fd_ = -1; return false; }
		was_b0_ = ::cfgetospeed(&t) == B0;
		usb_ = sys_->usb(path);
		::cfmakeraw(&t);
		t.c_cflag &= ~CRTSCTS;
		t.c_cflag |= CLOCAL | CREAD | HUPCL;
		t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
		rate_ = baud > 0 ? serial_speed(baud) : ::cfgetospeed(&t);
		b0_ = t; ::cfsetispeed(&b0_, B0); ::cfsetospeed(&b0_, B0);
		if (!was_b0_) {
			note_ = path + " was not at speed B0: this open raised RTS/DTR for an instant (the first open after plug-in, or another program set a speed); " +
			        std::string(usb_ ? "it is at B0 from now on" : baud > 0 ? "not a USB port: left at B0 at exit" : "not a USB port: its speed is kept");
			at(rate_);                               // a rate to a rate: no line moves (sets the chip's rate when baud is given)
			if (usb_ && !hold) at(B0);               // the chip keeps that rate at B0
		} else if (usb_ || baud <= 0) {
			at(B0);                                  // nothing raised; the chip keeps whatever rate it had
			rate_unknown_ = baud > 0;
		} else {                                         // a native UART runs at 9600 at B0: the rate is needed
			note_ = path + ": not a USB port: setting its rate from B0 raised RTS/DTR for an instant (cleared at once)";
			at(rate_);
		}
		return true;
	}
	// At B0 on a USB port, opened with a rate: the chip has the rate it was last given, maybe not ours.
	bool rate_unknown() const { return rate_unknown_; }
	// Set the chip's rate anyway: leaving B0 raises RTS/DTR in the driver until the clear right after it.
	void set_rate_now()
	{
		if (fd_ < 0) return;
		at(rate_);
		if (usb_) at(B0);
		rate_unknown_ = false;
	}
	bool raised_at_open() const { return fd_ >= 0 && !was_b0_; }
	bool at_b0() const { return cur_ == B0; }
	// From one rate to another: no line moves. False at B0 (leaving it raises the lines: leave_b0()).
	bool change_rate(int baud)
	{
		if (fd_ < 0 || cur_ == B0) return false;
		at(serial_speed(baud));
		return true;
	}
	// Out of B0 to a rate: the driver raises RTS/DTR until the clear right after (an instant).
	void leave_b0(int baud) { if (fd_ >= 0) at(serial_speed(baud)); }
	void to_b0() { if (fd_ >= 0) at(B0); }
	bool is_usb() const { return usb_; }
	const std::string &note() const { return note_; }
	int fd() const { return fd_; }

	void lines(int bits, bool on) { if (fd_ >= 0) sys_->modem(fd_, on ? TIOCMBIS : TIOCMBIC, bits); }
	void low() { lines(TIOCM_RTS | TIOCM_DTR, false); }
	ssize_t read(void *b, size_t n) { return fd_ < 0 ? -1 : sys_->read(fd_, b, n); }
	ssize_t write(const void *b, size_t n) { return fd_ < 0 ? -1 : sys_->write(fd_, b, n); }

	// Lines low and the port at B0, the fd kept open (async-signal-safe: exit paths, signal handlers).
	// A native UART used for its lines only keeps its speed (a program sharing the port may need it).
	void rest()
	{
		if (fd_ < 0) return;
		low();
		if (usb_ || baud_ > 0) { sys_->set(fd_, b0_); cur_ = B0; low(); }
	}
	void close()
	{
		if (fd_ < 0) return;
		rest();
		sys_->close(fd_);
		fd_ = -1;
	}

private:
	SerialSys *sys_;
	int fd_ = -1, baud_ = 0;
	bool was_b0_ = true, usb_ = false, rate_unknown_ = false;
	speed_t rate_ = B0, cur_ = B0;
	termios b0_{};
	std::string note_;

	void at(speed_t sp)
	{
		termios t = b0_;
		::cfsetispeed(&t, sp); ::cfsetospeed(&t, sp);
		sys_->set(fd_, t);
		cur_ = sp;
		low();
	}
};

}  // namespace util
