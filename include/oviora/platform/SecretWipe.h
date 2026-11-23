// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: overwriting a secret held in a Qt string before it is let go. The bytes are overwritten where
// they are (also when other copies share the same buffer, so those are wiped too), then the value is
// cleared. Only for buffers the caller filled itself (never QByteArray::fromRawData over constant data).

#ifndef OVIORA_SECRETWIPE_H
#define OVIORA_SECRETWIPE_H

#include <QByteArray>
#include <QString>

#include <cstddef>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SecretWipe
{
inline void zero(void *p, std::size_t n)
{
	if(!p || !n)
		return;
#ifdef Q_OS_WIN
	SecureZeroMemory(p, n);
#else
	volatile unsigned char *b = static_cast<volatile unsigned char *>(p);
	while(n--)
		*b++ = 0;
#endif
}

inline void wipe(QByteArray &b)
{
	if(b.size() > 0)
		zero(const_cast<char *>(b.constData()), std::size_t(b.size()));
	b.clear();
}

inline void wipe(QString &s)
{
	if(s.size() > 0)
		zero(const_cast<QChar *>(s.constData()), std::size_t(s.size()) * sizeof(QChar));
	s.clear();
}
}

#endif // OVIORA_SECRETWIPE_H
