// SPDX-License-Identifier: GPL-3.0-only
#include "GatewayConfig.h"
#include "GatewaySessionManager.h"
#include "PhpGatewayClient.h"

#include "oviora/Log.h"
#include "oviora/remoteplay/ChiakiBackend.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kServiceName[] = L"OvioraGecit";

struct CliOptions {
    bool serviceMode = false;
    bool showHelp = false;
    std::filesystem::path iniPath;
    std::filesystem::path mockH264Path;
};

std::filesystem::path executableDir() {
    wchar_t buf[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
}

CliOptions parseCli(int argc, wchar_t** argv, const std::filesystem::path& exeDir) {
    CliOptions opts;
    opts.iniPath = exeDir / L"gecit.ini";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--hizmet") {
            opts.serviceMode = true;
        } else if (arg == L"--yardim" || arg == L"-h" || arg == L"--help") {
            opts.showHelp = true;
        } else if (arg == L"--deneme-kaynak" && i + 1 < argc) {
            opts.mockH264Path = argv[++i];
        } else if (arg == L"--ayar" && i + 1 < argc) {
            opts.iniPath = argv[++i];
        }
    }
    return opts;
}

CliOptions g_options;
std::filesystem::path g_exeDir;
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_serviceStatus{};
std::atomic<QCoreApplication*> g_app{nullptr};
std::atomic<bool> g_stopSignaled{false};

void reportServiceStatus(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHintMs = 0) {
    if (!g_statusHandle) return;
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = currentState;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHintMs;
    g_serviceStatus.dwControlsAccepted =
        (currentState == SERVICE_START_PENDING || currentState == SERVICE_STOPPED)
            ? 0
            : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void requestAppQuit() {
    g_stopSignaled.store(true);
    if (QCoreApplication* app = g_app.load()) {
        QMetaObject::invokeMethod(app, &QCoreApplication::quit, Qt::QueuedConnection);
    }
}

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        requestAppQuit();
        return TRUE;
    default:
        return FALSE;
    }
}

DWORD WINAPI serviceCtrlHandlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        requestAppQuit();
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

int runGateway(const CliOptions& opts, const std::filesystem::path& exeDir) {
    auto& logger = oviora::Logger::instance();
    logger.setConsoleEnabled(!opts.serviceMode);
    logger.setRotation(20ULL * 1024ULL * 1024ULL, 20ULL * 1024ULL * 1024ULL);
    logger.openFile((exeDir / L"logs" / L"gecit.log").wstring(), true);
    logger.startAsync();

    oviora::gecit::GatewayConfig cfg;
    std::string cfgErr;
    if (!oviora::gecit::loadGatewayConfig(opts.iniPath, exeDir, cfg, &cfgErr, true)) {
        oviora::log::error("gecit", std::format("yapilandirma hatasi: {}", cfgErr));
        logger.flush();
        return 2;
    }

    if (opts.mockH264Path.empty()) {
        const auto chiakiInit = oviora::remoteplay::initializeChiaki();
        if (!chiakiInit.ok) {
            oviora::log::error("gecit", std::format("chiaki_lib_init basarisiz code={}", chiakiInit.code));
            cfg.wipeKey();
            logger.flush();
            return 3;
        }
    }

    int qtArgc = 1;
    char appName[] = "oviora-gecit";
    char* qtArgv[] = {appName, nullptr};
    QCoreApplication app(qtArgc, qtArgv);
    g_app.store(&app);

    oviora::gecit::PhpGatewayClient php(cfg);
    oviora::gecit::GatewaySessionManager manager(cfg, php, opts.mockH264Path);

    // GatewayConfig's key was copied into PhpGatewayClient; wipe the config copy now.
    cfg.wipeKey();

    php.setOnJobs([&manager](std::vector<oviora::gecit::GwJob> jobs) {
        manager.handleJobs(std::move(jobs));
    });

    if (g_stopSignaled.load()) {
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    }

    oviora::log::info("gecit", std::format("baslatildi hizmet={} udp={}-{} maxOturum={} deneme={}",
                                           opts.serviceMode, cfg.udpBegin, cfg.udpEnd,
                                           cfg.maxSessions, !opts.mockH264Path.empty()));
    php.startPolling();

    if (opts.serviceMode) {
        reportServiceStatus(SERVICE_RUNNING);
    }

    const int rc = app.exec();

    oviora::log::info("gecit", "kapatiliyor...");
    php.stopPolling();
    manager.shutdownAll();
    g_app.store(nullptr);
    logger.flush();
    return rc;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, serviceCtrlHandlerEx, nullptr);
    if (!g_statusHandle) return;
    reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
    const int rc = runGateway(g_options, g_exeDir);
    reportServiceStatus(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    g_exeDir = executableDir();
    g_options = parseCli(argc, argv, g_exeDir);

    if (g_options.showHelp) {
        std::cout << "Kullanim: oviora-gecit.exe [--hizmet] [--ayar <gecit.ini>] [--deneme-kaynak <h264>]\n";
        return 0;
    }

    if (g_options.serviceMode) {
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(kServiceName), serviceMain},
            {nullptr, nullptr}
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            const DWORD err = GetLastError();
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                std::cerr << "Hizmet denetleyicisi baglantisi yok; Windows hizmeti olarak SCM uzerinden baslatilmali.\n";
            }
            return 1;
        }
        return 0;
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    return runGateway(g_options, g_exeDir);
}
