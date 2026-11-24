// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/ui/MenuBackdropStore.h"
#include "oviora/engine/SubtitleTap.h"

#include <DirectXPackedVector.h>
#include <dxgiformat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace oviora::ui {

namespace {
std::mutex g_mutex;
QImage g_image;

uint8_t srgb(double x)
{
	x = std::clamp(x, 0.0, 1.0);
	const double v = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
	return uint8_t(std::clamp(std::lround(v * 255), 0L, 255L));
}
}

QImage MenuBackdropStore::toSmall(const engine::SubtitleSample &s, int width, bool blur)
{
	if(s.width == 0 || s.height == 0 || s.pixels.size() < size_t(s.width) * s.height * s.bytesPerPixel)
		return {};
	// ~240 px wide: each output pixel the average of a 3 x 3 grid inside its block (point sampling aliased:
	// bright game text turned into blocks), then a strong blur, so the picture scaled back up is a smooth glass
	const uint32_t step = std::max<uint32_t>(1, s.width / uint32_t(std::max(16, width)));
	const int ow = int(s.width / step), oh = int(s.height / step);
	if(ow < 2 || oh < 2)
		return {};
	static const std::array<uint8_t, 1024> pq = [] {
		std::array<uint8_t, 1024> t{};
		const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
		for(int i = 0; i < 1024; i++)
		{
			const double p = std::pow(i / 1023.0, 1 / m2);
			const double nits = 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1);
			t[size_t(i)] = srgb(nits / 203.0);
		}
		return t;
	}();
	auto pixel = [&](uint32_t x, uint32_t y, int rgb[3]) {
		const uint8_t *in = s.pixels.data() + (size_t(y) * s.width + x) * s.bytesPerPixel;
		switch(s.format)
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			rgb[0] += in[2]; rgb[1] += in[1]; rgb[2] += in[0];
			break;
		case DXGI_FORMAT_R10G10B10A2_UNORM: {
			uint32_t v;
			memcpy(&v, in, 4);
			rgb[0] += pq[v & 0x3ff]; rgb[1] += pq[(v >> 10) & 0x3ff]; rgb[2] += pq[(v >> 20) & 0x3ff];
			break;
		}
		case DXGI_FORMAT_R16G16B16A16_FLOAT: {
			uint16_t h[3];
			memcpy(h, in, 6);
			const double k = 80.0 / 203.0;  // scRGB: 1.0 = 80 nits
			for(int c = 0; c < 3; c++)
				rgb[c] += srgb(DirectX::PackedVector::XMConvertHalfToFloat(h[c]) * k);
			break;
		}
		default:
			rgb[0] += in[0]; rgb[1] += in[1]; rgb[2] += in[2];
			break;
		}
	};
	std::vector<float> a(size_t(ow) * oh * 3), b(a.size());
	const uint32_t q = std::max<uint32_t>(1, (step + 2) / 3);  // at most 3 x 3 samples per block
	for(int y = 0; y < oh; y++)
		for(int x = 0; x < ow; x++)
		{
			int rgb[3] = {};
			int n = 0;
			for(uint32_t dy = q / 2; dy < step; dy += q)
				for(uint32_t dx = q / 2; dx < step; dx += q, n++)
					pixel(uint32_t(x) * step + dx, uint32_t(y) * step + dy, rgb);
			float *o = &a[(size_t(y) * ow + x) * 3];
			for(int c = 0; c < 3; c++)
				o[c] = float(rgb[c]) / float(std::max(1, n));
		}
	// Three box passes each way ~ a Gaussian of about 7 px here (~60 px on a 1080p picture)
	const int r = 4;
	auto pass = [&](const std::vector<float> &in, std::vector<float> &out, bool horizontal) {
		const int len = horizontal ? ow : oh, lines = horizontal ? oh : ow;
		for(int l = 0; l < lines; l++)
			for(int c = 0; c < 3; c++)
			{
				auto at = [&](int i) { i = std::clamp(i, 0, len - 1); return horizontal ? in[(size_t(l) * ow + i) * 3 + c] : in[(size_t(i) * ow + l) * 3 + c]; };
				float sum = 0;
				for(int i = -r; i <= r; i++)
					sum += at(i);
				for(int i = 0; i < len; i++)
				{
					(horizontal ? out[(size_t(l) * ow + i) * 3 + c] : out[(size_t(i) * ow + l) * 3 + c]) = sum / float(2 * r + 1);
					sum += at(i + r + 1) - at(i - r);
				}
			}
	};
	for(int i = 0; blur && i < 3; i++)
	{
		pass(a, b, true);
		pass(b, a, false);
	}
	QImage out(ow, oh, QImage::Format_RGB888);
	for(int y = 0; y < oh; y++)
	{
		uchar *o = out.scanLine(y);
		for(int x = 0; x < ow * 3; x++)
			o[x] = uchar(std::clamp(std::lround(a[size_t(y) * ow * 3 + x]), 0L, 255L));
	}
	return out;
}

void MenuBackdropStore::set(const QImage &image)
{
	std::lock_guard lock(g_mutex);
	g_image = image;
}

QImage MenuBackdropStore::get()
{
	std::lock_guard lock(g_mutex);
	return g_image;
}

QImage MenuBackdropProvider::requestImage(const QString &, QSize *size, const QSize &)
{
	QImage img = MenuBackdropStore::get();
	if(size)
		*size = img.size();
	return img;
}

} // namespace oviora::ui
