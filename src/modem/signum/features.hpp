// features.hpp: what a build of the station contains.
//
// The platform is detected from the compiler, never passed:
//   SIGNUM_PLAT_ESP32    ESP-IDF (ESP_PLATFORM)
//   SIGNUM_PLAT_WINDOWS  _WIN32 (no Windows build yet)
//   SIGNUM_PLAT_LINUX    __linux__ (SIGNUM_PLAT_PI as well on 32/64-bit ARM)
//
// A profile chooses the features; one -D on the compiler line:
//   SIGNUM_PROFILE_FULL   P1/P2/P3 including P1 links (Linux PC, Raspberry Pi)
//   SIGNUM_PROFILE_ESP32  P2/P3 and the P1 call phase; no P1 links
//                          (a P1Station needs ~3 MB while it runs)
// Without a profile: SIGNUM_PROFILE_ESP32 on the ESP32, else SIGNUM_PROFILE_FULL.
// SIGNUM_NO_P1LINK removes P1 links from any profile.
//
// Features, as the code tests them:
//   SIGNUM_P1LINK   P1 ARQ links (p2_station.hpp hands a P1-only peer to a
//                    P1Station); without it such a peer is refused
#pragma once

#if defined(ESP_PLATFORM)
#define SIGNUM_PLAT_ESP32 1
#elif defined(_WIN32)
#define SIGNUM_PLAT_WINDOWS 1
#elif defined(__linux__)
#define SIGNUM_PLAT_LINUX 1
#if defined(__aarch64__) || defined(__arm__)
#define SIGNUM_PLAT_PI 1
#endif
#endif

#if !defined(SIGNUM_PROFILE_FULL) && !defined(SIGNUM_PROFILE_ESP32)
#if defined(SIGNUM_PLAT_ESP32)
#define SIGNUM_PROFILE_ESP32 1
#else
#define SIGNUM_PROFILE_FULL 1
#endif
#endif

#if defined(SIGNUM_PROFILE_FULL) && defined(SIGNUM_PROFILE_ESP32)
#error "features.hpp: choose one SIGNUM_PROFILE_*"
#endif

#if !defined(SIGNUM_PROFILE_ESP32) && !defined(SIGNUM_NO_P1LINK) && !defined(SIGNUM_P1LINK)
#define SIGNUM_P1LINK 1
#endif

namespace fmodem {
#if defined(SIGNUM_P1LINK)
constexpr bool kP1Link = true;
#else
constexpr bool kP1Link = false;
#endif
}  // namespace fmodem
