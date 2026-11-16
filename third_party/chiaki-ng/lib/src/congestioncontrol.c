// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/congestioncontrol.h>

#include <stdio.h>
#include <stdlib.h>

#define CONGESTION_CONTROL_INTERVAL_MS 200

/**
 * Oviora test runs only: OVIORA_TEST_REPORT_LOSS="<percent>,<from s>,<to s>" reports that much
 * packet loss to the console between those seconds of the stream (the reported numbers only:
 * nothing is dropped here), to see how the console and the picture follow a weak line.
 */
typedef struct { double share; unsigned from_ticks, to_ticks; } TestLoss;

static TestLoss test_loss_read(void)
{
	TestLoss t = { 0 };
	const char *v = getenv("OVIORA_TEST_REPORT_LOSS");
	double pct = 0, from = 0, to = 0;
	if(v && sscanf(v, "%lf,%lf,%lf", &pct, &from, &to) == 3 && pct > 0 && pct <= 90 && to > from && from >= 0)
	{
		t.share = pct / 100.0;
		t.from_ticks = (unsigned)(from * 1000 / CONGESTION_CONTROL_INTERVAL_MS);
		t.to_ticks = (unsigned)(to * 1000 / CONGESTION_CONTROL_INTERVAL_MS);
	}
	return t;
}

static void *congestion_control_thread_func(void *user)
{
	ChiakiCongestionControl *control = user;
	chiaki_thread_set_affinity(CHIAKI_THREAD_NAME_CONGESTION);

	ChiakiErrorCode err = chiaki_bool_pred_cond_lock(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return NULL;
	const TestLoss test_loss = test_loss_read();
	unsigned tick = 0;
	if(test_loss.share > 0)
		CHIAKI_LOGW(control->takion->log, "Test: reporting %.0f%% packet loss to the console from %us to %us",
			test_loss.share * 100.0, test_loss.from_ticks * CONGESTION_CONTROL_INTERVAL_MS / 1000, test_loss.to_ticks * CONGESTION_CONTROL_INTERVAL_MS / 1000);

	while(true)
	{
		err = chiaki_bool_pred_cond_timedwait(&control->stop_cond, CONGESTION_CONTROL_INTERVAL_MS);
		if(err != CHIAKI_ERR_TIMEOUT)
			break;

		uint64_t received;
		uint64_t lost;
		chiaki_packet_stats_get(control->stats, true, &received, &lost);
		ChiakiTakionCongestionPacket packet = { 0 };
		uint64_t total = received + lost;
		control->packet_loss = total > 0 ? (double)lost / total : 0;
		if(control->packet_loss > control->packet_loss_max)
		{
			CHIAKI_LOGD(control->takion->log, "Clamping reported packet loss: measured=%.1f%% reported_max=%.1f%%",
				control->packet_loss * 100.0, control->packet_loss_max * 100.0);
			lost = total * control->packet_loss_max;
			received = total - lost;
		}
		tick++;
		if(test_loss.share > 0 && tick >= test_loss.from_ticks && tick < test_loss.to_ticks && total > 0)
		{
			lost = (uint64_t)(total * test_loss.share);
			received = total - lost;
		}
		packet.received = (uint16_t)received;
		packet.lost = (uint16_t)lost;
		CHIAKI_LOGV(control->takion->log, "Sending Congestion Control Packet, received: %u, lost: %u",
			(unsigned int)packet.received, (unsigned int)packet.lost);
		chiaki_takion_send_congestion(control->takion, &packet);
	}

	chiaki_bool_pred_cond_unlock(&control->stop_cond);
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_congestion_control_start(ChiakiCongestionControl *control, ChiakiTakion *takion, ChiakiPacketStats *stats, double packet_loss_max)
{
	control->takion = takion;
	control->stats = stats;
	control->packet_loss_max = packet_loss_max;
	control->packet_loss = 0;

	ChiakiErrorCode err = chiaki_bool_pred_cond_init(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_thread_create(&control->thread, congestion_control_thread_func, control);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_bool_pred_cond_fini(&control->stop_cond);
		return err;
	}

	chiaki_thread_set_name(&control->thread, "Chiaki Congestion Control");

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_congestion_control_stop(ChiakiCongestionControl *control)
{
	ChiakiErrorCode err = chiaki_bool_pred_cond_signal(&control->stop_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_thread_join(&control->thread, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	control->thread.thread = 0;

	return chiaki_bool_pred_cond_fini(&control->stop_cond);
}
