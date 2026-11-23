// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "oviora/platform/PsnWebView.h"
#include "oviora/platform/SecretWipe.h"
#include "oviora/RuntimePaths.h"

namespace PSNAuth {
// Where Sony's remote play sign-in ends; the Oviora server turns this address into the PSN login
static const std::string REDIRECT_PAGE = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
// Oyun bilgisi: Sony's SSO cookie page, read in the same browser after the sign-in (JSON with the value)
static const char *SSO_COOKIE_PAGE = "https://ca.account.sony.com/api/v1/ssocookie";
}

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <thread>

Q_LOGGING_CATEGORY(psnWebLog, "oviora.psnweb")

// Only Sony's own pages (https, Sony / PlayStation hosts) are ever loaded
static bool isSonyUrl(const QUrl &url)
{
	const QString host = url.host().toLower();
	return url.scheme() == "https" && (host == "sony.com" || host.endsWith(".sony.com") || host.endsWith(".sonyentertainmentnetwork.com")
		|| host.endsWith(".playstation.com") || host.endsWith(".playstation.net"));
}

static bool isSsoCookiePage(const QString &address)
{
	const QUrl url(address);
	const QUrl sso(QString::fromLatin1(PSNAuth::SSO_COOKIE_PAGE));
	return url.scheme() == "https" && url.host().compare(sso.host(), Qt::CaseInsensitive) == 0 && url.path() == sso.path();
}

/**
 * The value of "npsso" from ExecuteScript's answer to document.body.innerText: that answer is the page's
 * text as a JSON string, and the text is Sony's JSON object. Read by hand into buffers reserved up front
 * (no reallocation leaves a stray copy, no JSON document keeps one); the text buffer is wiped here, the
 * result by the caller. Empty when the page has no usable value.
 */
static QByteArray npssoFromScriptResult(const wchar_t *json)
{
	QByteArray value;
	if(!json || json[0] != L'"')
		return value;
	const std::size_t len = std::wcslen(json);
	QByteArray text;
	text.reserve(qsizetype(len));
	for(std::size_t i = 1; i < len; ++i)
	{
		wchar_t c = json[i];
		if(c == L'"')
			break;
		if(c == L'\\' && i + 1 < len)
		{
			c = json[++i];
			switch(c)
			{
				case L'n': c = L'\n'; break;
				case L'r': c = L'\r'; break;
				case L't': c = L'\t'; break;
				case L'b': c = L'\b'; break;
				case L'f': c = L'\f'; break;
				case L'u':
				{
					unsigned code = 0;
					int digits = 0;
					for(; digits < 4 && i + 1 < len; ++digits)
					{
						const wchar_t h = json[i + 1];
						const int v = h >= L'0' && h <= L'9' ? int(h - L'0') : h >= L'a' && h <= L'f' ? int(h - L'a') + 10
							: h >= L'A' && h <= L'F' ? int(h - L'A') + 10 : -1;
						if(v < 0)
							break;
						code = code * 16 + unsigned(v);
						++i;
					}
					c = digits == 4 ? wchar_t(code) : L'?';
					break;
				}
				default: break;  // \" \\ \/ stand for themselves
			}
		}
		text.append(c > 0 && c < 0x80 ? char(c) : '?');
	}
	// {"npsso":"<value>"}: the key, then a quoted value of letters, digits, '-', '_' or '.'
	auto space = [&text](qsizetype p) {
		while(p < text.size() && (text.at(p) == ' ' || text.at(p) == '\n' || text.at(p) == '\r' || text.at(p) == '\t'))
			++p;
		return p;
	};
	qsizetype at = text.indexOf("\"npsso\"");
	if(at >= 0)
	{
		at = space(at + 7);
		if(at < text.size() && text.at(at) == ':')
		{
			at = space(at + 1);
			if(at < text.size() && text.at(at) == '"')
			{
				static const qsizetype MAX = 512;
				value.reserve(MAX);
				qsizetype p = at + 1;
				for(; p < text.size() && value.size() < MAX; ++p)
				{
					const char ch = text.at(p);
					if((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
						value.append(ch);
					else
						break;
				}
				if(p >= text.size() || text.at(p) != '"' || value.size() < 16)
					SecretWipe::wipe(value);
			}
		}
	}
	SecretWipe::wipe(text);
	return value;
}

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
	// Oyun bilgisi: the linked PSN account (> 0) the sign-in is for; reading: the SSO page is being read
	// (the browser is hidden then, so the value is never on screen); script: its text was asked for
	int wide_link = 0;
	bool wide_reading = false, wide_script = false;
	// "PSN hesabını bağla" + "Oyun bilgisini de aç": link_wide asked for it; held: the redirect was caught
	// and the page waits hidden for the linked account's id; linked: the current SSO read is one of these
	bool link_wide = false, held = false, linked = false;
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
	d->controller->put_IsVisible(d->visible && !d->closing && !d->wide_reading && !d->held && d->rect.width() > 1 && d->rect.height() > 1);
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
	if(!isSonyUrl(url))
	{
		qWarning("PsnWebView: refused a non-Sony address");
		return;
	}
	// A page still held for (or reading) a linked account's Oyun bilgisi is never reused for a new sign-in
	if(d->held || d->linked)
		close();
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
	if(d->env)
	{
		d->env->Release();
		d->env = nullptr;
	}
	const QString folder = dataFolder();
	QDir(folder).removeRecursively();
	QDir().mkpath(folder);
	d->creating = true;
	const quint64 gen = ++d->generation;
	setState("starting");
	const HWND parent = reinterpret_cast<HWND>(window->winId());
	QPointer<PsnWebView> self(this);
	auto env_done = makeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(IID_EnvCompleted,
		[self, gen, parent](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
			if(!self || gen != self->d->generation)
				return S_OK;
			if(FAILED(hr) || !env)
			{
				self->d->creating = false;
				qCWarning(psnWebLog) << "WebView2 environment failed" << Qt::hex << hr;
				self->setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0')));
				return S_OK;
			}
			env->AddRef();
			self->d->env = env;
			auto ctrl_done = makeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller *>(IID_ControllerCompleted,
				[self, gen](HRESULT hr, ICoreWebView2Controller *controller) -> HRESULT {
					if(!self || gen != self->d->generation)
					{
						if(controller)
							controller->Close();
						return S_OK;
					}
					self->d->creating = false;
					if(FAILED(hr) || !controller)
					{
						qCWarning(psnWebLog) << "WebView2 controller failed" << Qt::hex << hr;
						self->setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(hr), 8, 16, QLatin1Char('0')));
						return S_OK;
					}
					controller->AddRef();
					self->d->controller = controller;
					controller->get_CoreWebView2(&self->d->web);
					if(!self->d->web)
					{
						self->setState("error", tr("Tarayıcı penceresi açılamadı."));
						return S_OK;
					}
					ICoreWebView2Settings *settings = nullptr;
					if(SUCCEEDED(self->d->web->get_Settings(&settings)) && settings)
					{
						settings->put_AreDevToolsEnabled(FALSE);
						settings->put_IsStatusBarEnabled(FALSE);
						settings->put_AreDefaultContextMenusEnabled(TRUE);
						settings->Release();
					}
					self->d->web->AddScriptToExecuteOnDocumentCreated(BRAND_SCRIPT, nullptr);
					for(const wchar_t *filter : { L"https://*.sony.com/*", L"https://*.sonyentertainmentnetwork.com/*", L"https://*.playstation.com/*", L"https://*.playstation.net/*" })
						self->d->web->AddWebResourceRequestedFilter(filter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
					if(auto *h = makeHandler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *>(IID_ResourceRequested,
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
						}))
					{
						self->d->web->add_WebResourceRequested(h, &self->d->resource);
						h->Release();
					}
					if(auto *h = makeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *>(IID_NavStarting,
						[self](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
							if(!self)
								return S_OK;
							LPWSTR raw = nullptr;
							args->get_Uri(&raw);
							const QString uri = wstr(raw);
							// The same check as Chiaki.checkPsnRedirectURL: the end of Sony's sign-in
							if(uri.startsWith(QString::fromStdString(PSNAuth::REDIRECT_PAGE)))
							{
								args->put_Cancel(TRUE);
								qCInfo(psnWebLog) << "Sony's redirect caught and cancelled, handing it to the link flow (code"
									<< (uri.contains("code=") ? "present)" : "missing)");
								// Oyun bilgisi: the code is not needed (dropped here); the same browser reads the SSO page next
								if(self->d->wide_link > 0)
									QMetaObject::invokeMethod(self.data(), [self]() { if(self) self->startWideRead(); }, Qt::QueuedConnection);
								else
								{
									// "Oyun bilgisini de aç": the session is kept, out of sight, for the SSO page after psn_link
									if(self->d->link_wide && !self->d->held)
										self->holdForWide();
									QMetaObject::invokeMethod(self.data(), [self, uri]() { if(self) emit self->redirected(uri); }, Qt::QueuedConnection);
								}
							}
							else
								self->setState("loading");
							return S_OK;
						}))
					{
						self->d->web->add_NavigationStarting(h, &self->d->nav_start);
						h->Release();
					}
					if(auto *h = makeHandler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *>(IID_NavCompleted,
						[self](ICoreWebView2 *web, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
							if(!self)
								return S_OK;
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
							// Oyun bilgisi: only the SSO page's own load counts (the cancelled redirect's end is ignored;
							// a page that never comes is caught by startWideRead's timeout). No test hook runs here.
							if(self->d->wide_reading)
							{
								if(!isSsoCookiePage(source) || self->d->wide_script)
									return S_OK;
								if(!ok)
								{
									self->finishWide(false, tr("Sony'nin sayfası yüklenemedi. İnternet bağlantını kontrol et."));
									return S_OK;
								}
								self->d->wide_script = true;
								const quint64 scriptGen = self->d->generation;
								auto read = makeHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(IID_ScriptDone,
									[self, scriptGen](HRESULT hr, LPCWSTR json) -> HRESULT {
										// The value is only ever in this buffer and the request the sink sends
										QByteArray value = SUCCEEDED(hr) ? npssoFromScriptResult(json) : QByteArray();
										if(!self || scriptGen != self->d->generation || !self->d->wide_reading)
										{
											SecretWipe::wipe(value);
											return S_OK;
										}
										if(value.isEmpty() || !self->wide_sink)
										{
											SecretWipe::wipe(value);
											qCInfo(psnWebLog) << "Oyun bilgisi: Sony's SSO page had no sign-in";
											self->finishWide(false, tr("Sony girişi okunamadı. Yeniden dene."));
											return S_OK;
										}
										qCInfo(psnWebLog) << "Oyun bilgisi: Sony's sign-in read, handed to the server";
										self->wide_sink(self->d->wide_link, value, self->d->linked);
										SecretWipe::wipe(value);
										self->finishWide(true, QString());
										return S_OK;
									});
								if(FAILED(web->ExecuteScript(L"document.body ? document.body.innerText : ''", read)))
									self->finishWide(false, tr("Sony girişi okunamadı. Yeniden dene."));
								read->Release();
								return S_OK;
							}
							// Only when Sony cannot be reached at all is the browser-and-paste way better;
							// Sony's own error pages (and cancelled navigations) stay on screen
							const bool unreachable = !ok && status >= COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT
								&& status <= COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED;
							if(unreachable)
								self->setState("error", tr("Sony'nin sayfası yüklenemedi. İnternet bağlantını kontrol et."));
							else
								self->setState("ready");
							// (never during an Oyun bilgisi sign-in: the SSO page's text must not reach the log)
							if(qEnvironmentVariableIsSet("YAKAMOZ_TEST_WEBVIEW") && self->d->wide_link <= 0 && !self->d->held && !self->d->linked)
								QTimer::singleShot(5000, self.data(), [self]() {
									if(!self || !self->d->web || self->d->wide_link > 0 || self->d->wide_reading || self->d->held || self->d->linked)
										return;
									auto *done = makeHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(IID_ScriptDone, [](HRESULT, LPCWSTR json) -> HRESULT {
										qCInfo(psnWebLog) << "test: page says" << (json ? QString::fromWCharArray(json) : QString());
										return S_OK;
									});
									self->d->web->ExecuteScript(L"(navigator.userAgentData ? JSON.stringify(navigator.userAgentData.brands) : '') + ' | ' + document.title + ' | ' + (document.body ? document.body.innerText.slice(0, 200) : '')",
										done);
									done->Release();
								});
							// Testing without a Sony login: the page "finishes" with a made-up redirect
							if(!unreachable && !self->d->test_redirect_sent && self->d->wide_link <= 0 && qEnvironmentVariableIntValue("YAKAMOZ_TEST_WEBVIEW_YONLENDIR") == 1)
							{
								self->d->test_redirect_sent = true;
								QTimer::singleShot(4000, self.data(), [self]() {
									if(!self || !self->d->web)
										return;
									qCInfo(psnWebLog) << "test: the page navigates to a made-up redirect";
									const QString js = QStringLiteral("location.href = '%1?code=TESTKOD&cid=00000000-0000-0000-0000-000000000000';")
										.arg(QString::fromStdString(PSNAuth::REDIRECT_PAGE));
									self->d->web->ExecuteScript(reinterpret_cast<LPCWSTR>(js.utf16()), nullptr);
								});
							}
							return S_OK;
						}))
					{
						self->d->web->add_NavigationCompleted(h, &self->d->nav_done);
						h->Release();
					}
					// Links that want a new window stay in this one (no stray browser windows)
					if(auto *h = makeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *>(IID_NewWindow,
						[](ICoreWebView2 *web, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
							LPWSTR raw = nullptr;
							args->get_Uri(&raw);
							args->put_Handled(TRUE);
							if(raw)
								web->Navigate(raw);
							CoTaskMemFree(raw);
							return S_OK;
						}))
					{
						self->d->web->add_NewWindowRequested(h, &self->d->new_window);
						h->Release();
					}
					self->applyBounds();
					self->setState("loading");
					if(self->d->pending_url.isValid())
						self->d->web->Navigate(reinterpret_cast<LPCWSTR>(self->d->pending_url.toString(QUrl::FullyEncoded).utf16()));
					return S_OK;
				});
			HRESULT chr = env->CreateCoreWebView2Controller(parent, ctrl_done);
			ctrl_done->Release();
			if(FAILED(chr))
			{
				self->d->creating = false;
				self->setState("error", tr("Tarayıcı penceresi açılamadı (0x%1).").arg(quint32(chr), 8, 16, QLatin1Char('0')));
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

void PsnWebView::setWideLink(int link)
{
	d->wide_link = link > 0 ? link : 0;
	d->wide_reading = false;
	d->wide_script = false;
}

void PsnWebView::setLinkWide(bool on)
{
	// Only before the redirect: a page already held stays held
	if(!d->held)
		d->link_wide = on;
}

bool PsnWebView::holdingForWide() const
{
	return d->held || d->linked;
}

void PsnWebView::holdForWide()
{
	d->held = true;
	applyBounds();  // hidden from the redirect on: nothing more of the session is ever on screen
	qCInfo(psnWebLog) << "PSN link with Oyun bilgisi: page held for the linked account";
	// psn_link never answering (or its answer lost) must not keep a signed-in page around
	const quint64 gen = d->generation;
	QTimer::singleShot(90000, this, [this, gen]() {
		if(gen == d->generation && d->held)
		{
			qCInfo(psnWebLog) << "PSN link with Oyun bilgisi: no linked account in time, page closed";
			d->held = false;
			d->linked = true;  // finishWide() below reports it as the soft failure
			finishWide(false, QString());
		}
	});
}

void PsnWebView::readWideFor(int link)
{
	if(!d->held)
		return;
	d->held = false;
	d->link_wide = false;
	d->linked = true;
	if(link <= 0 || !d->web)
	{
		finishWide(false, QString());
		return;
	}
	d->wide_link = link;
	startWideRead();
}

void PsnWebView::startWideRead()
{
#ifdef Q_OS_WIN
	if(!d->web || d->wide_link <= 0 || d->wide_reading)
		return;
	const QUrl sso(QString::fromLatin1(PSNAuth::SSO_COOKIE_PAGE));
	// The same Sony-only rule as open()
	if(!isSonyUrl(sso))
	{
		finishWide(false, tr("Sony girişi okunamadı. Yeniden dene."));
		return;
	}
	d->wide_reading = true;
	d->wide_script = false;
	applyBounds();  // hidden from here on: the page's text is never on screen
	setState("loading");
	qCInfo(psnWebLog) << "Oyun bilgisi: sign-in done, reading Sony's SSO page";
	d->web->Navigate(reinterpret_cast<LPCWSTR>(sso.toString(QUrl::FullyEncoded).utf16()));
	const quint64 gen = d->generation;
	QTimer::singleShot(20000, this, [this, gen]() {
		if(gen == d->generation && d->wide_reading)
			finishWide(false, tr("Sony'nin sayfası yüklenemedi. İnternet bağlantını kontrol et."));
	});
#endif
}

void PsnWebView::finishWide(bool sent, const QString &error)
{
	// Stays hidden until it is closed (the SSO page is still in it)
	d->visible = false;
	d->wide_reading = false;
	applyBounds();
	d->wide_script = false;
	d->wide_link = 0;
	d->held = false;
	const bool linked = d->linked;
	d->linked = false;
	if(linked && !sent)
		qCInfo(psnWebLog) << "PSN link with Oyun bilgisi: Oyun bilgisi not read (the PSN link stays)";
	// Out of the browser's own callback: the page closes and its folder (cookies, storage, cache) is deleted
	QMetaObject::invokeMethod(this, [this, sent, error, linked]() {
		close();
		// After "PSN hesabını bağla" the popup is gone and the link counts: only a soft word when it failed
		// (the server's own answer to a sent value comes from OvioraServer)
		if(!linked)
			emit wideFinished(sent, error);
		else if(!sent)
			emit linkWideFailed();
	}, Qt::QueuedConnection);
}

void PsnWebView::close()
{
	d->pending_url = QUrl();
	d->wide_link = 0;
	d->wide_reading = false;
	d->wide_script = false;
	d->link_wide = false;
	d->held = false;
	d->linked = false;
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
	const EventRegistrationToken nav_start = d->nav_start;
	const EventRegistrationToken nav_done = d->nav_done;
	const EventRegistrationToken new_window = d->new_window;
	const EventRegistrationToken resource = d->resource;
	d->controller = nullptr;
	d->web = nullptr;
	d->env = nullptr;
	d->nav_start = {};
	d->nav_done = {};
	d->new_window = {};
	d->resource = {};
	const QString folder = dataFolder();
	QPointer<PsnWebView> self(this);
	// Then the browser lets go of the folder and it is deleted
	auto finish = [self, controller, web, env, nav_start, nav_done, new_window, resource, folder]() {
		if(web)
		{
			web->remove_NavigationStarting(nav_start);
			web->remove_NavigationCompleted(nav_done);
			web->remove_NewWindowRequested(new_window);
			web->remove_WebResourceRequested(resource);
			web->Release();
		}
		controller->Close();
		controller->Release();
		if(env)
			env->Release();
		qCInfo(psnWebLog) << "closed, sign-in data cleared";
		if(self)
		{
			QTimer::singleShot(2500, self.data(), [self, folder]() {
				if(self && !self->d->web && !self->d->creating)
					QDir(folder).removeRecursively();
			});
		}
		else if(QCoreApplication *app = QCoreApplication::instance())
		{
			QTimer::singleShot(2500, app, [folder]() {
				QDir(folder).removeRecursively();
			});
		}
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
				QObject *ctx = QCoreApplication::instance() ? static_cast<QObject *>(QCoreApplication::instance()) : static_cast<QObject *>(this);
				auto cleared = makeHandler<ICoreWebView2ClearBrowsingDataCompletedHandler, HRESULT>(IID_ClearDone,
					[once, ctxPtr = QPointer<QObject>(ctx)](HRESULT) -> HRESULT {
						if(ctxPtr)
							QMetaObject::invokeMethod(ctxPtr.data(), once, Qt::QueuedConnection);
						else
							once();
						return S_OK;
					});
				async = SUCCEEDED(profile2->ClearBrowsingDataAll(cleared));
				cleared->Release();
				if(async)
					QTimer::singleShot(3000, ctx, once);  // never wait forever
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
