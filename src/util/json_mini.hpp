// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// json_mini.hpp: a small JSON reader and string escaper for the station's
// WebSocket interface (src/web/ws_server.hpp). Objects, arrays, strings (with
// escapes, \uXXXX to UTF-8), numbers, true / false / null. No exceptions:
// parse() returns false on malformed input. Header-only, no dependencies
// (meant to build for the ESP32 too).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jsonm {

struct Value {
	enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Value> a;
	std::map<std::string, Value> o;
	const Value *get(const std::string &k) const { auto it = o.find(k); return kind == OBJ && it != o.end() ? &it->second : nullptr; }
	std::string str(const std::string &k, const std::string &def = "") const { const Value *v = get(k); return v && v->kind == STR ? v->s : def; }
	double num(const std::string &k, double def = 0) const { const Value *v = get(k); return v && v->kind == NUM ? v->n : def; }
	bool has(const std::string &k) const { return get(k) != nullptr; }
};

class Parser {
public:
	explicit Parser(const std::string &t) : t_(t) {}
	bool parse(Value &v) { ws(); if (!value(v, 0)) return false; ws(); return i_ == t_.size(); }
private:
	const std::string &t_;
	size_t i_ = 0;
	void ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) i_++; }
	bool lit(const char *w) { size_t k = 0; while (w[k]) { if (i_ + k >= t_.size() || t_[i_ + k] != w[k]) return false; k++; } i_ += k; return true; }
	bool value(Value &v, int depth)
	{
		if (depth > 32 || i_ >= t_.size()) return false;
		const char c = t_[i_];
		if (c == '{') return object(v, depth);
		if (c == '[') return array(v, depth);
		if (c == '"') { v.kind = Value::STR; return string(v.s); }
		if (c == 't') { v.kind = Value::BOOL; v.b = true; return lit("true"); }
		if (c == 'f') { v.kind = Value::BOOL; v.b = false; return lit("false"); }
		if (c == 'n') { v.kind = Value::NUL; return lit("null"); }
		if (c == '-' || (c >= '0' && c <= '9')) {
			const char *p = t_.c_str() + i_; char *e = nullptr;
			v.kind = Value::NUM; v.n = std::strtod(p, &e);
			if (e == p) return false;
			i_ += size_t(e - p);
			return true;
		}
		return false;
	}
	bool object(Value &v, int depth)
	{
		v.kind = Value::OBJ; i_++; ws();
		if (i_ < t_.size() && t_[i_] == '}') { i_++; return true; }
		for (;;) {
			ws();
			std::string k;
			if (i_ >= t_.size() || t_[i_] != '"' || !string(k)) return false;
			ws();
			if (i_ >= t_.size() || t_[i_] != ':') return false;
			i_++; ws();
			Value x;
			if (!value(x, depth + 1)) return false;
			v.o[k] = std::move(x);
			ws();
			if (i_ < t_.size() && t_[i_] == ',') { i_++; continue; }
			if (i_ < t_.size() && t_[i_] == '}') { i_++; return true; }
			return false;
		}
	}
	bool array(Value &v, int depth)
	{
		v.kind = Value::ARR; i_++; ws();
		if (i_ < t_.size() && t_[i_] == ']') { i_++; return true; }
		for (;;) {
			ws();
			Value x;
			if (!value(x, depth + 1)) return false;
			v.a.push_back(std::move(x));
			ws();
			if (i_ < t_.size() && t_[i_] == ',') { i_++; continue; }
			if (i_ < t_.size() && t_[i_] == ']') { i_++; return true; }
			return false;
		}
	}
	static void utf8(std::string &o, uint32_t cp)
	{
		if (cp < 0x80) o += char(cp);
		else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
		else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
	}
	bool hex4(uint32_t &v)
	{
		if (i_ + 4 > t_.size()) return false;
		v = 0;
		for (int k = 0; k < 4; k++) {
			const char c = t_[i_++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
			else return false;
		}
		return true;
	}
	bool string(std::string &o)
	{
		i_++;                                       // the opening quote
		while (i_ < t_.size()) {
			const char c = t_[i_++];
			if (c == '"') return true;
			if (c != '\\') { o += c; continue; }
			if (i_ >= t_.size()) return false;
			const char e = t_[i_++];
			switch (e) {
			case '"': o += '"'; break;
			case '\\': o += '\\'; break;
			case '/': o += '/'; break;
			case 'b': o += '\b'; break;
			case 'f': o += '\f'; break;
			case 'n': o += '\n'; break;
			case 'r': o += '\r'; break;
			case 't': o += '\t'; break;
			case 'u': {
				uint32_t cp;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp < 0xDC00 && i_ + 6 <= t_.size() && t_[i_] == '\\' && t_[i_ + 1] == 'u') {
					i_ += 2; uint32_t lo;
					if (!hex4(lo)) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				utf8(o, cp);
				break;
			}
			default: return false;
			}
		}
		return false;
	}
};

inline bool parse(const std::string &text, Value &v) { return Parser(text).parse(v); }

// A JSON string literal (quotes included). Bytes 0x80-0xFF that are not
// valid UTF-8 (link text is 8-bit) go out as \u00XX, so any byte survives.
inline std::string quote(const std::string &s)
{
	std::string o = "\"";
	for (size_t i = 0; i < s.size(); i++) {
		const unsigned char c = (unsigned char)s[i];
		if (c == '"') o += "\\\"";
		else if (c == '\\') o += "\\\\";
		else if (c == '\n') o += "\\n";
		else if (c == '\r') o += "\\r";
		else if (c == '\t') o += "\\t";
		else if (c < 0x20 || c >= 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
		else o += char(c);
	}
	return o + "\"";
}

}  // namespace jsonm
