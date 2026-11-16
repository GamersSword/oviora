// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "oviora/platform/PsnWebView.h"
#include "oviora/RuntimePaths.h"

namespace PSNAuth {
// Where Sony's remote play sign-in ends; the Oviora server turns this address into the PSN login
static const std::string REDIRECT_PAGE = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
}

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <functional>
#include <memory>
#include <string>
#include <thread>

Q_LOGGING_CATEGORY(psnWebLog, "oviora.psnweb")

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include "WebView2.h"

// The interface ids WebView2.h only declares (MinGW has no __uuidof for them and no WebView2 .lib)
static const IID IID_EnvCompleted = { 0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d } };
static const IID IID_ControllerCompleted = { 0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c } };
static const IID IID_NavStarting = { 0x9adbe429, 0xf36d, 0x432b, { 0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3 } };
static const IID IID_NavCompleted = { 0xd33a35bf, 0x1c49, 0x4f98, { 0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c } };
static const IID IID_NewWindow = { 0xd4c185fe, 0xc81c, 0x4989, { 0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51 } };
static const IID IID_NavCompletedArgs2 = { 0xfdf8b738, 0xee1e, 0x4db2, { 0xa3, 0x29, 0x8d, 0x7d, 0x7b, 0x74, 0xd7, 0x92 } };
static const IID IID_ResourceRequested = { 0xab00b74c, 0x15f1, 0x4646, { 0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71 } };
static const IID IID_ScriptDone = { 0x49511172, 0xcc67, 0x4bca, { 0x99, 0x23, 0x13, 0x71, 0x12, 0xf4, 0xc4, 0xcc } };
static const IID IID_ClearDone = { 0xe9710a06, 0x1d1d, 0x49b2, { 0x82, 0x34, 0x22, 0x6f, 0x35, 0x84, 0x6a, 0xe5 } };
static const IID IID_WebView2_2 = { 0x9e8f0cf8, 0xe670, 0x4b5e, { 0xb2, 0xbc, 0x73, 0xe0, 0x61, 0xe3, 0x18, 0x4c } };
static const IID IID_WebView2_13 = { 0xf75f09a8, 0x667e, 0x4983, { 0x88, 0xd6, 0xc8, 0x77, 0x3f, 0x31, 0x5e, 0x84 } };
static const IID IID_Profile2 = { 0xfa740d4b, 0x5eae, 0x4344, { 0xa8, 0xad, 0x74, 0xbe, 0x31, 0x92, 0x53, 0x97 } };

// A WebView2 completion / event handler around a function (what WRL's Callback<> does with MSVC)
template<typename I, typename... Args>
class Handler : public I
{
public:
	Handler(const IID &iid, std::function<HRESULT(Args...)> fn) : iid(iid), fn(std::move(fn)) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if(IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid))
		{
			*out = static_cast<I *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = InterlockedDecrement(&refs);
		if(!r)
			delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn ? fn(args...) : S_OK; }

private:
	virtual ~Handler() = default;
	LONG refs = 1;
	IID iid;
	std::function<HRESULT(Args...)> fn;
};

template<typename T> struct NoDeduce { using type = T; };

template<typename I, typename... Args>
static I *makeHandler(const IID &iid, typename NoDeduce<std::function<HRESULT(Args...)>>::type fn)
{
	return new Handler<I, Args...>(iid, std::move(fn));
}

typedef HRESULT (STDAPICALLTYPE *CreateEnvFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT (STDAPICALLTYPE *VersionFn)(PCWSTR, LPWSTR *);

static HMODULE loader()
{
	static HMODULE lib = nullptr;
	static bool tried = false;
	if(!tried)
	{
		tried = true;
		QString path = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/WebView2Loader.dll");
		lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if(!lib)
			qCInfo(psnWebLog) << "WebView2Loader.dll not found next to the program";
	}
	return lib;
}

static int &cachedAvailability()
{
	// -1 = pending background probe, 0 = unavailable, 1 = available (main thread only)
	static int cached = -1;
	return cached;
}

static int &runtimeMajor()
{
	static int major = 0;
	return major;
}

// Sony's sign-in turns embedded browsers away ("Something went wrong"): the page is told it runs in
// the Edge it is (without the "Microsoft Edge WebView2" brand), like the QtWebEngine way did with Sec-Ch-Ua
static QString secChUa()
{
	const QString v = QString::number(runtimeMajor() > 0 ? runtimeMajor() : 140);
	return QStringLiteral("\"Not(A:Brand\";v=\"99\", \"Microsoft Edge\";v=\"%1\", \"Chromium\";v=\"%1\"").arg(v);
}

static const wchar_t *BRAND_SCRIPT = LR"JS(
(() => {
  const d = navigator.userAgentData;
  if (!d) return;
  const keep = (list) => (list || []).filter((b) => !/WebView/i.test(b.brand));
  const proto = Object.getPrototypeOf(d);
  const brands = keep(d.brands);
  try { Object.defineProperty(proto, 'brands', { get: () => brands }); } catch (e) {}
  const high = proto.getHighEntropyValues;
  if (high) proto.getHighEntropyValues = function (hints) {
    return high.call(this, hints).then((r) => {
      if (r.brands) r.brands = keep(r.brands);
      if (r.fullVersionList) r.fullVersionList = keep(r.fullVersionList);
      return r;
    });
  };
})();
)JS";

static QString wstr(LPWSTR s)
{
	QString r = s ? QString::fromWCharArray(s) : QString();
	if(s)
		CoTaskMemFree(s);
	return r;
}
#endif

struct PsnWebView::Impl
{
#ifdef Q_OS_WIN
	ICoreWebView2Environment *env = nullptr;
	ICoreWebView2Controller *controller = nullptr;
	ICoreWebView2 *web = nullptr;
	EventRegistrationToken nav_start = {}, nav_done = {}, new_window = {}, resource = {};
#endif
	bool creating = false, closing = false, visible = true, test_redirect_sent = false;
	QUrl pending_url;
	QRectF rect;
	quint64 generation = 0;  // an open() after close() must not get the old environment's answers
};

static QString dataFolder()
{
	// Its own folder, never the Edge profile.
	// The new Oviora's own data folder: the old Oviora's one must never be touched (it is deleted on close)
	return QString::fromStdWString((oviora::runtime::localDataDirectory() / L"webview2").wstring());
}

PsnWebView::PsnWebView(QWindow *window, QObject *parent)
	: QObject(parent), d(new Impl), window(window)
{
	// Testing without clicking: YAKAMOZ_TEST_WEBVIEW=1 opens "PSN hesabını bağla" a while after start
	if(qEnvironmentVariableIntValue("YAKAMOZ_TEST_WEBVIEW") == 1)
		QTimer::singleShot(9000, this, []() {
			for(QWindow *w : QGuiApplication::allWindows())
			{
				auto quick = qobject_cast<QQuickWindow *>(w);
				if(!quick)
					continue;
				for(QQuickItem *item : quick->contentItem()->childItems())
					if(item->metaObject()->indexOfMethod("showPsnLinkDialog()") >= 0)
					{
						qCInfo(psnWebLog) << "test: opening the PSN link dialog";
						QMetaObject::invokeMethod(item, "showPsnLinkDialog");
						return;
					}
			}
			qCWarning(psnWebLog) << "test: main QML item not found";
		});
	// A folder left by a crash still holds a sign-in: gone before anything else
	QTimer::singleShot(3000, this, [this]() {
		if(!d->creating && state_ == "idle" && QFileInfo::exists(dataFolder()))
			QDir(dataFolder()).removeRecursively();
	});
#ifdef Q_OS_WIN
	static bool probing = false;
	if(cachedAvailability() < 0 && !probing)
	{
		probing = true;
		if(qEnvironmentVariableIntValue("YAKAMOZ_TEST_WEBVIEW_YOK") == 1)
			cachedAvailability() = 0;
		else
		{
			const QString dllPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/WebView2Loader.dll");
			const QPointer<PsnWebView> weak(this);
			std::thread([weak, dllPath]() {
				bool found = false;
				bool loaderMissing = false;
				QString version;
				int major = 0;
				HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dllPath.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if(!lib)
				{
					loaderMissing = true;
				}
				else
				{
					auto versionFn = reinterpret_cast<VersionFn>(reinterpret_cast<void *>(GetProcAddress(lib, "GetAvailableCoreWebView2BrowserVersionString")));
					LPWSTR v = nullptr;
					if(versionFn && SUCCEEDED(versionFn(nullptr, &v)) && v)
					{
						version = wstr(v);
						major = version.section('.', 0, 0).toInt();
						found = true;
					}
					FreeLibrary(lib);
				}
				if(QCoreApplication *app = QCoreApplication::instance())
				{
					QMetaObject::invokeMethod(app, [weak, found, loaderMissing, version, major]() {
						if(loaderMissing)
							qCInfo(psnWebLog) << "WebView2Loader.dll not found next to the program";
						else if(found)
							qCInfo(psnWebLog) << "WebView2 runtime" << version;
						else
							qCInfo(psnWebLog) << "WebView2 runtime not installed";
						cachedAvailability() = found ? 1 : 0;
						if(found)
							runtimeMajor() = major;
						if(weak)
							emit weak->availableChanged();
					}, Qt::QueuedConnection);
				}
			}).detach();
		}
	}
#endif
}

PsnWebView::~PsnWebView()
{
#ifdef Q_OS_WIN
	if(d->controller)
	{
		d->controller->Close();
		d->controller->Release();
	}
	if(d->web)
		d->web->Release();
	if(d->env)
		d->env->Release();
#endif
	delete d;
}

bool PsnWebView::available() const
{
#ifdef Q_OS_WIN
	return cachedAvailability() == 1;
#else
	return false;
#endif
}

void PsnWebView::setState(const QString &state, const QString &error)
{
	state_ = state;
	error_text = error;
	emit stateChanged();
}

void PsnWebView::applyBounds()
{
#ifdef Q_OS_WIN
	if(!d->controller || !window)
		return;
	const qreal dpr = window->devicePixelRatio();
	RECT r;
	r.left = LONG(qRound(d->rect.x() * dpr));
	r.top = LONG(qRound(d->rect.y() * dpr));
	r.right = LONG(qRound((d->rect.x() + d->rect.width()) * dpr));
	r.bottom = LONG(qRound((d->rect.y() + d->rect.height()) * dpr));
	d->controller->put_Bounds(r);
	d->controller->put_IsVisible(d->visible && !d->closing && d->rect.width() > 1 && d->rect.height() > 1);
#endif
}

void PsnWebView::setRect(qreal x, qreal y, qreal w, qreal h)
{
	QRectF r(x, y, w, h);
	if(r == d->rect)
		return;
	d->rect = r;
	applyBounds();
}

void PsnWebView::setVisible(bool visible)
{
	if(d->visible == visible)
		return;
	d->visible = visible;
	applyBounds();
}

void PsnWebView::open(const QUrl &url, qreal x, qreal y, qreal w, qreal h)
{
	// Only Sony's sign-in pages (the address comes from the server or the pairing dialog)
	const QString host = url.host().toLower();
	if(url.scheme() != "https" || !(host == "sony.com" || host.endsWith(".sony.com") || host.endsWith(".sonyentertainmentnetwork.com")
		|| host.endsWith(".playstation.com") || host.endsWith(".playstation.net")))
	{
		qWarning("PsnWebView: refused a non-Sony address");
		return;
	}
	d->rect = QRectF(x, y, w, h);
	d->pending_url = url;
	d->closing = false;
	d->visible = true;
	d->test_redirect_sent = false;
#ifdef Q_OS_WIN
	if(d->web)
	{
		applyBounds();
		d->web->Navigate(reinterpret_cast<LPCWSTR>(url.toString(QUrl::FullyEncoded).utf16()));
		setState("loading");
		return;
	}
	if(d->creating)
		return;
	HMODULE lib = loader();
	auto create = lib ? reinterpret_cast<CreateEnvFn>(reinterpret_cast<void *>(GetProcAddress(lib, "CreateCoreWebView2EnvironmentWithOptions"))) : nullptr;
	if(!create || !window)
	{
		setState("error", tr("Microsoft Edge WebView2 bulunamadı."));
		return;
	}
	const QString folder = dataFolder();
	QDir(folder).removeRecursively();
	QDir().mkpath(folder);
	d->creating = true;
	const quint64 gen = ++d->generation;
	setState("starting");
	const HWND parent = reinterpret_cast<HWND>(window->winId());
	auto env_done = makeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(IID_EnvCompleted,
		[this, gen, parent](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
			if(gen != d->generation)
				return S_OK;
			if(FAILED(hr) || !env)
			{
				d->creating = false;
				qCWarning(psnWebLog) << "WebView2 environment failed" << Qt::hex << hr;
				setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0')));
				return S_OK;
			}
			env->AddRef();
			d->env = env;
			auto ctrl_done = makeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller *>(IID_ControllerCompleted,
				[this, gen](HRESULT hr, ICoreWebView2Controller *controller) -> HRESULT {
					if(gen != d->generation)
						return S_OK;
					d->creating = false;
					if(FAILED(hr) || !controller)
					{
						qCWarning(psnWebLog) << "WebView2 controller failed" << Qt::hex << hr;
						setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0')));
						return S_OK;
					}
					controller->AddRef();
					d->controller = controller;
					controller->get_CoreWebView2(&d->web);
					if(!d->web)
					{
						setState("error", tr("Tarayıcı penceresi açılamadı."));
						return S_OK;
					}
					ICoreWebView2Settings *settings = nullptr;
					if(SUCCEEDED(d->web->get_Settings(&settings)) && settings)
					{
						settings->put_AreDevToolsEnabled(FALSE);
						settings->put_IsStatusBarEnabled(FALSE);
						settings->put_AreDefaultContextMenusEnabled(TRUE);
						settings->Release();
					}
					d->web->AddScriptToExecuteOnDocumentCreated(BRAND_SCRIPT, nullptr);
					for(const wchar_t *filter : { L"https://*.sony.com/*", L"https://*.sonyentertainmentnetwork.com/*", L"https://*.playstation.com/*", L"https://*.playstation.net/*" })
						d->web->AddWebResourceRequestedFilter(filter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
					d->web->add_WebResourceRequested(makeHandler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *>(IID_ResourceRequested,
						[](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *args) -> HRESULT {
							ICoreWebView2WebResourceRequest *request = nullptr;
							ICoreWebView2HttpRequestHeaders *headers = nullptr;
							if(SUCCEEDED(args->get_Request(&request)) && request && SUCCEEDED(request->get_Headers(&headers)) && headers)
							{
								static const QString ua = secChUa();
								headers->SetHeader(L"Sec-Ch-Ua", reinterpret_cast<LPCWSTR>(ua.utf16()));
								headers->Release();
							}
							if(request)
								request->Release();
							return S_OK;
						}), &d->resource);
					d->web->add_NavigationStarting(makeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *>(IID_NavStarting,
						[this](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
							LPWSTR raw = nullptr;
							args->get_Uri(&raw);
							const QString uri = wstr(raw);
							// The same check as Chiaki.checkPsnRedirectURL: the end of Sony's sign-in
							if(uri.startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE)))
							{
								args->put_Cancel(TRUE);
								qCInfo(psnWebLog) << "Sony's redirect caught and cancelled, handing it to the link flow (code"
									<< (uri.contains("code=") ? "present)" : "missing)");
								QMetaObject::invokeMethod(this, [this, uri]() { emit redirected(uri); }, Qt::QueuedConnection);
							}
							else
								setState("loading");
							return S_OK;
						}), &d->nav_start);
					d->web->add_NavigationCompleted(makeHandler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *>(IID_NavCompleted,
						[this](ICoreWebView2 *web, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
							BOOL ok = FALSE;
							args->get_IsSuccess(&ok);
							LPWSTR raw = nullptr;
							web->get_Source(&raw);
							const QString source = wstr(raw);
							if(source.startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE)))
								return S_OK;  // the cancelled redirect
							COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
							args->get_WebErrorStatus(&status);
							int http = 0;
							ICoreWebView2NavigationCompletedEventArgs2 *args2 = nullptr;
							if(SUCCEEDED(args->QueryInterface(IID_NavCompletedArgs2, reinterpret_cast<void **>(&args2))) && args2)
							{
								args2->get_HttpStatusCode(&http);
								args2->Release();
							}
							const QUrl src(source);
							qCInfo(psnWebLog) << "page loaded" << src.host() + src.path() << "ok" << bool(ok) << "status" << int(status) << "http" << http;
							// Only when Sony cannot be reached at all is the browser-and-paste way better;
							// Sony's own error pages (and cancelled navigations) stay on screen
							const bool unreachable = !ok && status >= COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT
								&& status <= COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED;
							if(unreachable)
								setState("error", tr("Sony'nin sayfası yüklenemedi. İnternet bağlantını kontrol et."));
							else
								setState("ready");
							if(qEnvironmentVariableIsSet("YAKAMOZ_TEST_WEBVIEW"))
								QTimer::singleShot(5000, this, [this]() {
									if(!d->web)
										return;
									d->web->ExecuteScript(L"(navigator.userAgentData ? JSON.stringify(navigator.userAgentData.brands) : '') + ' | ' + document.title + ' | ' + (document.body ? document.body.innerText.slice(0, 200) : '')",
										makeHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(IID_ScriptDone, [](HRESULT, LPCWSTR json) -> HRESULT {
											qCInfo(psnWebLog) << "test: page says" << (json ? QString::fromWCharArray(json) : QString());
											return S_OK;
										}));
								});
							// Testing without a Sony login: the page "finishes" with a made-up redirect
							if(!unreachable && !d->test_redirect_sent && qEnvironmentVariableIntValue("YAKAMOZ_TEST_WEBVIEW_YONLENDIR") == 1)
							{
								d->test_redirect_sent = true;
								QTimer::singleShot(4000, this, [this]() {
									if(!d->web)
										return;
									qCInfo(psnWebLog) << "test: the page navigates to a made-up redirect";
									const QString js = QStringLiteral("location.href = '%1?code=TESTKOD&cid=00000000-0000-0000-0000-000000000000';")
										.arg(QString::fromStdString(PSNAuth::REDIRECT_PAGE));
									d->web->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), nullptr);
								});
							}
							return S_OK;
						}), &d->nav_done);
					// Links that want a new window stay in this one (no stray browser windows)
					d->web->add_NewWindowRequested(makeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *>(IID_NewWindow,
						[](ICoreWebView2 *web, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
							LPWSTR raw = nullptr;
							args->get_Uri(&raw);
							args->put_Handled(TRUE);
							if(raw)
								web->Navigate(raw);
							CoTaskMemFree(raw);
							return S_OK;
						}), &d->new_window);
					applyBounds();
					setState("loading");
					if(d->pending_url.isValid())
						d->web->Navigate(reinterpret_cast<LPCWSTR>(d->pending_url.toString(QUrl::FullyEncoded).utf16()));
					return S_OK;
				});
			HRESULT chr = env->CreateCoreWebView2Controller(parent, ctrl_done);
			ctrl_done->Release();
			if(FAILED(chr))
			{
				d->creating = false;
				setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(chr), 8, 16, QLatin1Char('0')));
			}
			return S_OK;
		});
	const QString native = QDir::toNativeSeparators(folder);
	HRESULT hr = create(nullptr, reinterpret_cast<LPCWSTR>(native.utf16()), nullptr, env_done);
	env_done->Release();
	if(FAILED(hr))
	{
		d->creating = false;
		qCWarning(psnWebLog) << "CreateCoreWebView2EnvironmentWithOptions failed" << Qt::hex << hr;
		setState("error", tr("Microsoft Edge WebView2 başlatılamadı (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0')));
	}
#else
	setState("error", tr("Bu sistemde yerleşik tarayıcı yok."));
#endif
}

void PsnWebView::close()
{
	d->pending_url = QUrl();
	d->generation++;  // a controller still on its way is dropped
	d->creating = false;
#ifdef Q_OS_WIN
	if(!d->controller)
	{
		if(d->env)
		{
			d->env->Release();
			d->env = nullptr;
		}
		setState("idle");
		return;
	}
	d->closing = true;
	d->controller->put_IsVisible(FALSE);
	ICoreWebView2Controller *controller = d->controller;
	ICoreWebView2 *web = d->web;
	ICoreWebView2Environment *env = d->env;
	d->controller = nullptr;
	d->web = nullptr;
	d->env = nullptr;
	// Then the browser lets go of the folder and it is deleted
	auto finish = [this, controller, web, env]() {
		if(web)
		{
			web->remove_NavigationStarting(d->nav_start);
			web->remove_NavigationCompleted(d->nav_done);
			web->remove_NewWindowRequested(d->new_window);
			web->remove_WebResourceRequested(d->resource);
			web->Release();
		}
		controller->Close();
		controller->Release();
		if(env)
			env->Release();
		qCInfo(psnWebLog) << "closed, sign-in data cleared";
		QTimer::singleShot(2500, this, [this]() {
			if(!d->web && !d->creating)
				QDir(dataFolder()).removeRecursively();
		});
	};
	// Privacy: nothing of the Sony sign-in stays behind (cookies, storage, cache)
	bool async = false;
	if(web)
	{
		ICoreWebView2_2 *web2 = nullptr;
		if(SUCCEEDED(web->QueryInterface(IID_WebView2_2, reinterpret_cast<void **>(&web2))) && web2)
		{
			ICoreWebView2CookieManager *cookies = nullptr;
			if(SUCCEEDED(web2->get_CookieManager(&cookies)) && cookies)
			{
				cookies->DeleteAllCookies();
				cookies->Release();
			}
			web2->Release();
		}
		ICoreWebView2_13 *web13 = nullptr;
		if(SUCCEEDED(web->QueryInterface(IID_WebView2_13, reinterpret_cast<void **>(&web13))) && web13)
		{
			ICoreWebView2Profile *profile = nullptr;
			ICoreWebView2Profile2 *profile2 = nullptr;
			if(SUCCEEDED(web13->get_Profile(&profile)) && profile
				&& SUCCEEDED(profile->QueryInterface(IID_Profile2, reinterpret_cast<void **>(&profile2))) && profile2)
			{
				auto done = std::make_shared<bool>(false);
				auto once = [done, finish]() {
					if(*done)
						return;
					*done = true;
					finish();
				};
				auto cleared = makeHandler<ICoreWebView2ClearBrowsingDataCompletedHandler, HRESULT>(IID_ClearDone,
					[this, once](HRESULT) -> HRESULT {
						QMetaObject::invokeMethod(this, once, Qt::QueuedConnection);
						return S_OK;
					});
				async = SUCCEEDED(profile2->ClearBrowsingDataAll(cleared));
				cleared->Release();
				if(async)
					QTimer::singleShot(3000, this, once);  // never wait forever
				profile2->Release();
			}
			if(profile)
				profile->Release();
			web13->Release();
		}
	}
	if(!async)
		finish();
#endif
	setState("idle");
}
