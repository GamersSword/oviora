#pragma once
namespace oviora::gfx {
// G-SYNC off for Oviora only: a driver profile for Oviora.exe with the per-program
// "Monitor Technology" set to fixed refresh (the NVIDIA Control Panel's program setting).
// With G-SYNC on in a window the panel refreshed only when the stream presented, and the
// mouse cursor moved at the stream's uneven 50-60 fps (user report 2026-11-23). Other games
// keep G-SYNC. The driver reads the profile when a program starts, so the first run after
// it is written still has G-SYNC. Slow (driver database): call off the UI thread.
// OVIORA_TEST_NO_GSYNC_PROFILE skips it. Returns a short result for the log.
const char* ensureFixedRefreshProfile();
}
