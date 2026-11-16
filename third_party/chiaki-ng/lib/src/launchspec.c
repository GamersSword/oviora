// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/launchspec.h>
#include <chiaki/session.h>
#include <chiaki/base64.h>

#include <stdio.h>

static const char launchspec_fmt[] =
	"{"
		"\"sessionId\":\"sessionId4321\","
		"\"streamResolutions\":["
			"%s" // 0: the resolution(s), see stream_resolutions()
		"],"
		"\"network\":{"
			"\"bwKbpsSent\":%u," // 3
			"\"bwLoss\":0.001000,"
			"\"mtu\":%u," // 4
			"\"rtt\":%u," // 5
			"\"ports\":[53,2053]"
		"},"
		"\"slotId\":1,"
		"\"appSpecification\":{"
			"\"minFps\":30,"
			"\"minBandwidth\":0,"
			"\"extTitleId\":\"ps3\","
			"\"version\":1,"
			"\"timeLimit\":1,"
			"\"startTimeout\":100,"
			"\"afkTimeout\":100,"
			"\"afkTimeoutDisconnect\":100"
		"},"
		"\"konan\":{"
			"\"ps3AccessToken\":\"accessToken\","
			"\"ps3RefreshToken\":\"refreshToken\""
		"},\"requestGameSpecification\":{"
			"\"model\":\"bravia_tv\","
			"\"platform\":\"android\","
			"\"audioChannels\":\"5.1\","
			"\"language\":\"sp\","
			"\"acceptButton\":\"X\","
			"\"connectedControllers\":[\"xinput\",\"ds3\",\"ds4\"],"
			"\"yuvCoefficient\":\"bt601\","
			"\"videoEncoderProfile\":\"hw4.1\","
			"\"audioEncoderProfile\":\"audio1\"" // 6
			"%s"
		"},"
		"\"userProfile\":{"
			"\"onlineId\":\"psnId\","
			"\"npId\":\"npId\","
			"\"region\":\"US\","
			"\"languagesUsed\":[\"en\",\"jp\"]"
		"},"
		"%s" // 7
		"%s" // 8
		"\"handshakeKey\":\"%s\"" // 9
	"}";

static const char resolution_fmt[] =
	"{\"resolution\":{\"width\":%u,\"height\":%u},\"maxFps\":%u,\"score\":%u}";

/**
 * Oviora: the requested resolution, and with adaptive_resolutions (PS5) the next lower standard one with
 * a lower score, so the console can step down on a weak line. Unknown sizes get no extra entry.
 */
static int stream_resolutions(char *buf, size_t buf_size, ChiakiLaunchSpec *launch_spec)
{
	int written = snprintf(buf, buf_size, resolution_fmt, launch_spec->width, launch_spec->height, launch_spec->max_fps, 10u);
	if(written < 0 || (size_t)written >= buf_size)
		return -1;
	if(!launch_spec->adaptive_resolutions || !chiaki_target_is_ps5(launch_spec->target))
		return written;
	static const unsigned int ladder[][2] = { { 3840, 2160 }, { 1920, 1080 }, { 1280, 720 }, { 960, 540 }, { 640, 360 } };
	for(size_t i = 0; i + 1 < sizeof(ladder) / sizeof(ladder[0]); i++)
	{
		if(ladder[i][0] != launch_spec->width || ladder[i][1] != launch_spec->height)
			continue;
		int more = snprintf(buf + written, buf_size - (size_t)written, "," /* next entry */);
		if(more < 0 || (size_t)(written + more) >= buf_size)
			return -1;
		written += more;
		more = snprintf(buf + written, buf_size - (size_t)written, resolution_fmt, ladder[i + 1][0], ladder[i + 1][1], launch_spec->max_fps, 5u);
		if(more < 0 || (size_t)(written + more) >= buf_size)
			return -1;
		written += more;
		break;
	}
	return written;
}

CHIAKI_EXPORT int chiaki_launchspec_format(char *buf, size_t buf_size, ChiakiLaunchSpec *launch_spec)
{
	char handshake_key_b64[CHIAKI_HANDSHAKE_KEY_SIZE * 2];
	ChiakiErrorCode err = chiaki_base64_encode(launch_spec->handshake_key, CHIAKI_HANDSHAKE_KEY_SIZE, handshake_key_b64, sizeof(handshake_key_b64));
	if(err != CHIAKI_ERR_SUCCESS)
		return -1;

	char *extras[3];
	if(chiaki_target_is_ps5(launch_spec->target)) // TODO: probably also for ps4, but only 12
	{
		extras[0] = ",\"adaptiveStreamMode\": \"resize\"";
		extras[1] = chiaki_codec_is_h265(launch_spec->codec)
			? "\"videoCodec\":\"hevc\","
			: "\"videoCodec\":\"avc\",";
		extras[2] = chiaki_codec_is_hdr(launch_spec->codec)
			? "\"dynamicRange\":\"HDR\","
			: "\"dynamicRange\":\"SDR\",";
	}
	else
		extras[0] = extras[1] = extras[2] = "";

	char resolutions[256];
	if(stream_resolutions(resolutions, sizeof(resolutions), launch_spec) < 0)
		return -1;

	int written = snprintf(buf, buf_size, launchspec_fmt,
			resolutions,
			launch_spec->bw_kbps_sent, launch_spec->mtu, launch_spec->rtt,
			extras[0], extras[1], extras[2], handshake_key_b64);
	if(written < 0 || written >= buf_size)
		return -1;
	return written;
}
