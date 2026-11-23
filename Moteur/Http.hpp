// *****************************************************************************
// Tapir Translator - engine: HTTP(S) client based on WinHTTP
//
//   Used by the online translators (TraducteursEnLigne.cpp) to call DeepL,
//   Microsoft, Google, MyMemory and LibreTranslate.
//
//   It goes through the Windows network stack: the system proxy (WPAD /
//   Windows settings) and the certificates of the Windows store. This works on
//   corporate networks that inspect TLS with their own root certificate,
//   where a client with its own certificate bundle would fail.
//
//   Synchronous API: one call = one request, the caller's thread waits for
//   the answer (or the timeout).
// *****************************************************************************

#ifndef TT_MOTEUR_HTTP_HPP
#define TT_MOTEUR_HTTP_HPP

#include <string>
#include <vector>

namespace Http {

// Result of a request.
struct Reponse {
	int code = 0;			// HTTP status code; 0 = no response at all (network, host name, timeout)
	std::string corps;		// response body (raw bytes, usually UTF-8 JSON)
	std::string erreur;		// human-readable explanation when code == 0
};

// Sends one HTTP request and waits for the complete response.
//   methode : "GET", "POST"...
//   url     : http://... or https://... (HTTPS is detected from the scheme)
//   entetes : extra headers, each one as "Name: value"
//   corps   : request body (may be empty)
//   delaiMs : timeout applied to each phase (resolve, connect, send, receive)
// Returns the status code and body; code == 0 and erreur filled on failure.
// An HTTP error status (4xx, 5xx) is not a failure here: the caller reads it.
Reponse Requete (const std::string& methode, const std::string& url, const std::vector<std::string>& entetes,
				 const std::string& corps, int delaiMs = 20000);

// Percent-encodes a URL parameter (UTF-8 bytes, RFC 3986): letters, digits
// and "-_.~" are kept, every other byte becomes %XX.
std::string EncoderUrl (const std::string& texte);

}	// namespace Http

#endif
