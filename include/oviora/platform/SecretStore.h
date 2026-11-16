#ifndef OVIORA_SECRETSTORE_H
#define OVIORA_SECRETSTORE_H

#include <QString>

/**
 * Oviora: tokens kept in the settings file are encrypted for the signed-in Windows user (DPAPI) and stored
 * as "dpapi1:<base64>". Reading a plain value (written by an older version) still works; the caller writes
 * it back encrypted. On other systems the value is kept as it is.
 */
namespace SecretStore
{
	/** Encrypted text for the settings file; an empty value stays empty */
	QString protect(const QString &plain);
	/** The plain value; "" if it was encrypted but can't be opened here (other PC or user) */
	QString unprotect(const QString &stored);
	/** True when the stored text is still plain (an older version wrote it) and should be rewritten */
	bool needsUpgrade(const QString &stored);
}

#endif // OVIORA_SECRETSTORE_H
