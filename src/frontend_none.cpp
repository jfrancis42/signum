// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// frontend_none.cpp: no front ends (the build uses it when src/web/frontend.cpp is absent).
#include "tnc_core.hpp"

namespace signum {
bool frontend_arg(int &, int, char **) { return false; }
void frontend_usage(FILE *) {}
bool frontend_start(Core &, std::string &) { return true; }
void frontend_pump(Core &) {}
void frontend_stop() {}
}  // namespace signum
