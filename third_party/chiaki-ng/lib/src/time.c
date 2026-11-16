// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/time.h>

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

CHIAKI_EXPORT uint64_t chiaki_time_now_monotonic_us()
{
#if _WIN32
	LARGE_INTEGER f;
	if(!QueryPerformanceFrequency(&f))
		return 0;
	LARGE_INTEGER v;
	if(!QueryPerformanceCounter(&v))
		return 0;
	// Oviora: whole seconds and the remainder separately. counter * 1000000 overflowed int64 once Windows had
	// been up ~10.7 days (10 MHz counter): the time went negative, and everything comparing times (subtitle
	// sampling, frame pacing, timeouts) stopped working until a reboot.
	const uint64_t counter = (uint64_t)v.QuadPart;
	const uint64_t freq = (uint64_t)f.QuadPart;
	return (counter / freq) * 1000000 + (counter % freq) * 1000000 / freq;
#else
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000000 + time.tv_nsec / 1000;
#endif
}
