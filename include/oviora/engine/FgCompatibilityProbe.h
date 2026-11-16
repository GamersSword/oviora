#pragma once
#include <atomic>
#include <string>
#include <windows.h>
namespace oviora::pipeline { struct EnhanceGraphDesc; }
namespace oviora::gfx { class D3D12DeviceContext; }
namespace oviora::engine {
// Installed by the executable that implements --fg-compat-probe.
void setFgCompatibilityProbeExecutable(std::wstring path);
bool checkFgCompatibility(const pipeline::EnhanceGraphDesc&, const gfx::D3D12DeviceContext&, const std::atomic<bool>& cancel);
int runFgCompatibilityProbe(HANDLE mapping);
}
