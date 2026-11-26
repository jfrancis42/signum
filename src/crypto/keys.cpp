// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// KeyStore member functions (out of keys.hpp, so that they build in parallel).
#include "keys.hpp"

namespace signum::crypto {

void KeyStore::set_dir(const std::string &dir)
{
	if (!dir.empty()) { dir_ = dir; return; }
	if (const char *e = std::getenv("SIGNUM_KEY_DIR")) if (*e) { dir_ = e; return; }
	const char *x = std::getenv("XDG_CONFIG_HOME"), *h = std::getenv("HOME");
	dir_ = x && *x ? std::string(x) + "/signum/keys" : std::string(h ? h : ".") + "/.config/signum/keys";
}

std::string KeyStore::lock(bool create)
{
	if (lock_fd_ >= 0) return "";
	struct stat sb{};
	if (!create && ::stat(dir_.c_str(), &sb) != 0) return "";
	std::string e = mkdirs();
	if (!e.empty()) return lock_err_ = e;
	const int fd = ::open(path("lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) return lock_err_ = "cannot open " + path("lock") + ": " + std::strerror(errno);
	if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		char b[32] = {0};
		const ssize_t n = ::pread(fd, b, sizeof b - 1, 0);
		std::string pid = n > 0 ? std::string(b, size_t(n)) : "";
		while (!pid.empty() && !std::isdigit(uint8_t(pid.back()))) pid.pop_back();
		::close(fd);
		return lock_err_ = "the key directory " + dir_ + " is in use by another Signum" + (pid.empty() ? "" : " (process " + pid + ")") +
		                   ": two Signums must not share one key directory (they would share the station key and overwrite each other's"
		                   " known stations); give this one its own with --key-dir DIR";
	}
	const std::string me = std::to_string(long(::getpid())) + "\n";
	if (::ftruncate(fd, 0) == 0 && ::pwrite(fd, me.data(), me.size(), 0) < 0) {}
	lock_fd_ = fd;
	lock_err_.clear();
	return "";
}

std::string KeyStore::load()
{
	std::string err;
	me_ = KeyPair();
	peers_.clear(); psks_.clear(); received_.clear();
	std::string t;
	if (read_file("station.key", t)) {
		std::string e;
		auto bl = parse_blocks(t, &e);
		if (bl.size() == 1 && bl[0].kind == "PRIVATE KEY") keypair_from_private(bl[0].key, me_);
		else err += "station.key is damaged (" + (e.empty() ? std::string("no private key block") : e) + "); ";
		wipe(&t[0], t.size());
		struct stat sb{};
		if (::stat(path("station.key").c_str(), &sb) == 0 && (sb.st_mode & 077)) err += "station.key can be read by others: chmod 600 " + path("station.key") + "; ";
	}
	reread_peers(); reread_psks(); reread_received();
	return err;
}

std::string KeyStore::generate(bool force)
{
	const std::string le = ensure_lock();
	if (!le.empty()) return le;
	if (!force) reread_station();                  // a key written meanwhile (by hand) is kept, not replaced
	if (me_.set && !force) return "this station already has a key pair (KEYGEN FORCE replaces it; every peer must then learn the new public key)";
	KeyPair kp;
	if (!generate_keypair(kp)) return "the random number generator failed";
	return set_private(kp.priv);
}

std::string KeyStore::set_private(const Key32 &priv)
{
	KeyPair kp;
	if (!keypair_from_private(priv, kp)) return "not a valid private key";
	const std::string le = ensure_lock();
	if (!le.empty()) return le;
	const std::string e = write_file("station.key", make_block({"PRIVATE KEY", "", priv}), true);
	if (!e.empty()) return e;
	me_.priv = kp.priv; me_.pub = kp.pub; me_.set = true;
	return "";
}

std::string KeyStore::add_peer(const std::string &call, const Key32 &pub, bool verified)
{
	if (!valid_call(call)) return "not a call sign: " + call;
	if (me_.set && pub == me_.pub) return "that is this station's own public key (fingerprint " + fingerprint(pub) + "), not " + upper(call) + "'s";
	const std::string le = ensure_lock();
	if (!le.empty()) return le;
	reread_peers(); reread_received();
	Peer p{pub, verified, (long long)std::time(nullptr)};
	auto it = peers_.find(upper(call));
	if (it != peers_.end() && it->second.pub == pub) p.since = it->second.since, p.verified = verified || it->second.verified;
	peers_[upper(call)] = p;
	received_.erase(upper(call));
	const std::string e = save_peers();
	return e.empty() ? save_received() : e;
}

std::string KeyStore::verify_peer(const std::string &call, bool v, const Key32 *expect)
{
	const std::string le = ensure_lock();
	if (!le.empty()) return le;
	reread_peers();
	auto it = peers_.find(upper(call));
	if (it == peers_.end()) return "no known key for " + upper(call) + (expect ? " (the key it presented is " + fingerprint(*expect) + ": add it, or replace)" : "");
	if (expect && it->second.pub != *expect)
		return "the key stored for " + upper(call) + " (" + fingerprint(it->second.pub) + ") is not the key you are verifying (" + fingerprint(*expect) +
		       "): nothing was marked verified. If its key really changed, replace the stored key (CRYPTO PEER REPLACE " + upper(call) + ")";
	it->second.verified = v;
	return save_peers();
}

void KeyStore::reread_peers()
{
	std::string t;
	peers_.clear();
	if (read_file("peers", t)) for (auto &f : lines_fields(t)) {
		Peer p;
		if (f.size() >= 3 && valid_call(f[0]) && unb64_32(f[1], p.pub)) { p.verified = f[2] == "verified"; p.since = f.size() > 3 ? std::atoll(f[3].c_str()) : 0; peers_[upper(f[0])] = p; }
	}
}

std::vector<std::vector<std::string>> KeyStore::lines_fields(const std::string &t)
{
	std::vector<std::vector<std::string>> r;
	size_t a = 0;
	while (a < t.size()) {
		size_t b = t.find('\n', a);
		if (b == std::string::npos) b = t.size();
		std::string l = t.substr(a, b - a);
		a = b + 1;
		if (l.empty() || l[0] == '#') continue;
		std::vector<std::string> f;
		size_t i = 0;
		while (i < l.size()) {
			while (i < l.size() && std::isspace(uint8_t(l[i]))) i++;
			size_t j = i;
			while (j < l.size() && !std::isspace(uint8_t(l[j]))) j++;
			if (j > i) f.push_back(l.substr(i, j - i));
			i = j;
		}
		if (!f.empty()) r.push_back(f);
	}
	return r;
}

std::string KeyStore::mkdirs() const
{
	std::string p;
	size_t i = 0;
	while (i <= dir_.size()) {
		size_t j = dir_.find('/', i + 1);
		if (j == std::string::npos) j = dir_.size();
		p = dir_.substr(0, j);
		if (!p.empty() && ::mkdir(p.c_str(), j == dir_.size() ? 0700 : 0755) != 0 && errno != EEXIST) return "cannot create " + p + ": " + std::strerror(errno);
		i = j;
		if (j == dir_.size()) break;
	}
	::chmod(dir_.c_str(), 0700);
	return "";
}

std::string KeyStore::write_file(const char *f, const std::string &t, bool secret) const
{
	(void)secret;
	const std::string e = mkdirs();
	if (!e.empty()) return e;
	const std::string tmp = path(f) + ".new";
	const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return "cannot write " + tmp + ": " + std::strerror(errno);
	::fchmod(fd, 0600);
	size_t o = 0;
	while (o < t.size()) { const ssize_t k = ::write(fd, t.data() + o, t.size() - o); if (k <= 0) { ::close(fd); return "cannot write " + tmp; } o += size_t(k); }
	::fsync(fd);
	::close(fd);
	if (::rename(tmp.c_str(), path(f).c_str()) != 0) return "cannot replace " + path(f) + ": " + std::strerror(errno);
	return "";
}

std::string KeyStore::save_peers() const
{
	std::string t = "# Signum: known stations' public keys (CALL KEY verified|unverified SINCE)\n";
	for (auto &p : peers_) t += p.first + " " + b64(p.second.pub.data(), 32) + (p.second.verified ? " verified " : " unverified ") + std::to_string(p.second.since) + "\n";
	return write_file("peers", t, false);
}

}  // namespace signum::crypto

namespace signum::crypto {

bool unb64_32(const std::string &s, Key32 &k)
{
	std::string t;
	for (char c : s) if (!std::isspace(uint8_t(c))) t += c;
	if (t.size() != 44 || t[43] != '=' || t[42] == '=') return false;
	unsigned char o[33];
	if (EVP_DecodeBlock(o, reinterpret_cast<const unsigned char *>(t.data()), int(t.size())) != 33) return false;
	std::memcpy(k.data(), o, 32);
	wipe(o, sizeof o);
	return true;
}

std::string sas(const Key32 &handshake_hash)
{
	Bytes b = to_bytes("Signum SAS v1");
	b.insert(b.end(), handshake_hash.begin(), handshake_hash.end());
	const Key32 h = sha256(b);
	const uint32_t v = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
	char o[16];
	std::snprintf(o, sizeof o, "%04u %04u", (v % 100000000u) / 10000u, v % 10000u);
	return o;
}

std::string make_block(const KeyBlock &b, const char *eol)
{
	std::string s = "-----BEGIN SIGNUM " + b.kind + "-----" + eol;
	if (!b.name.empty()) s += std::string(b.kind == "PRE-SHARED KEY" ? "Name: " : "Call: ") + b.name + eol;
	if (b.kind == "PUBLIC KEY") s += "Fingerprint: " + fingerprint(b.key) + eol;
	if (b.kind == "PRIVATE KEY") { Key32 pub; if (x25519_public(b.key, pub)) s += "Fingerprint: " + fingerprint(pub) + " (of its public key)" + eol; s += std::string("Comment: SECRET. Anyone with this block can pose as this station.") + eol; }
	if (b.kind == "PRE-SHARED KEY") s += std::string("Comment: SECRET. Anyone with this block can read and send this traffic.") + eol;
	s += "Key: " + b64(b.key.data(), 32) + eol;
	s += "Check: " + block_check(b.kind, b.key) + eol;
	s += "-----END SIGNUM " + b.kind + "-----" + eol;
	return s;
}

std::vector<KeyBlock> parse_blocks(const std::string &text, std::string *err)
{
	std::vector<KeyBlock> out;
	std::vector<std::string> lines;
	{
		std::string l;
		for (char c : text) { if (c == '\r' || c == '\n') { lines.push_back(l); l.clear(); } else l += c; }
		lines.push_back(l);
	}
	auto trim = [](std::string s) { while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace(uint8_t(s[i]))) i++; return s.substr(i); };
	for (size_t i = 0; i < lines.size(); i++) {
		const std::string l = trim(lines[i]);
		const std::string pre = "-----BEGIN SIGNUM ";
		if (l.compare(0, pre.size(), pre) != 0 || l.size() < pre.size() + 5 || l.substr(l.size() - 5) != "-----") continue;
		KeyBlock b;
		b.kind = l.substr(pre.size(), l.size() - pre.size() - 5);
		if (b.kind != "PUBLIC KEY" && b.kind != "PRIVATE KEY" && b.kind != "PRE-SHARED KEY") continue;
		std::string key, check;
		size_t j = i + 1;
		bool ended = false;
		for (; j < lines.size() && j < i + 12; j++) {
			const std::string m = trim(lines[j]);
			if (m == "-----END SIGNUM " + b.kind + "-----") { ended = true; break; }
			const size_t c = m.find(':');
			if (c == std::string::npos) continue;
			const std::string f = m.substr(0, c), v = trim(m.substr(c + 1));
			if (f == "Call" || f == "Name") b.name = upper(v);
			else if (f == "Key") key = v;
			else if (f == "Check") check = v;
		}
		if (!ended) continue;
		i = j;
		if (!unb64_32(key, b.key)) { if (err) *err = "a " + b.kind + " block with a damaged Key line"; continue; }
		if (check != block_check(b.kind, b.key)) { if (err) *err = "a " + b.kind + " block whose Check does not match (a typing or transfer error)"; continue; }
		out.push_back(b);
	}
	return out;
}

}  // namespace signum::crypto
