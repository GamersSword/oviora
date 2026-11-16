#include "oviora/platform/SecretStore.h"

#include <QByteArray>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

static const QString PREFIX = QStringLiteral("dpapi1:");
// Ties the encrypted values to this app: other programs of the same user need this to open them
static const char ENTROPY[] = "Oviora/settings/v1";

namespace SecretStore
{

QString protect(const QString &plain)
{
	if(plain.isEmpty())
		return plain;
#ifdef Q_OS_WIN
	QByteArray in = plain.toUtf8();
	DATA_BLOB input = { DWORD(in.size()), reinterpret_cast<BYTE *>(in.data()) };
	DATA_BLOB entropy = { DWORD(sizeof(ENTROPY) - 1), reinterpret_cast<BYTE *>(const_cast<char *>(ENTROPY)) };
	DATA_BLOB output = {};
	if(!CryptProtectData(&input, L"Oviora", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
	{
		// Never lose a login over this; it stays as before (logged so the case is visible)
		qWarning("SecretStore: CryptProtectData failed (%lu), kept unprotected", GetLastError());
		return plain;
	}
	QByteArray out(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
	LocalFree(output.pbData);
	SecureZeroMemory(in.data(), size_t(in.size()));
	return PREFIX + QString::fromLatin1(out.toBase64());
#else
	return plain;
#endif
}

QString unprotect(const QString &stored)
{
	if(!stored.startsWith(PREFIX))
		return stored;
#ifdef Q_OS_WIN
	QByteArray raw = QByteArray::fromBase64(stored.mid(PREFIX.size()).toLatin1());
	DATA_BLOB input = { DWORD(raw.size()), reinterpret_cast<BYTE *>(raw.data()) };
	DATA_BLOB entropy = { DWORD(sizeof(ENTROPY) - 1), reinterpret_cast<BYTE *>(const_cast<char *>(ENTROPY)) };
	DATA_BLOB output = {};
	if(!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
		return QString();
	QString plain = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return plain;
#else
	return QString();
#endif
}

bool needsUpgrade(const QString &stored)
{
#ifdef Q_OS_WIN
	return !stored.isEmpty() && !stored.startsWith(PREFIX);
#else
	return false;
#endif
}

}
