// *****************************************************************************
// Tapir Translator - engine: HTTP(S) client based on WinHTTP
//
//   Used by the online translators (OnlineTranslators.cpp) to call DeepL,
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

#ifndef TT_ENGINE_HTTP_HPP
#define TT_ENGINE_HTTP_HPP

#include <string>
#include <vector>

namespace Http {

// Result of a request.
struct Response {
	int code = 0;			// HTTP status code; 0 = no response at all (network, host name, timeout)
	std::string body;		// response body (raw bytes, usually UTF-8 JSON)
	std::string error;		// human-readable explanation when code == 0
};

// Sends one HTTP request and waits for the complete response.
//   method : "GET", "POST"...
//   url     : http://... or https://... (HTTPS is detected from the scheme)
//   headers : extra headers, each one as "Name: value"
//   body   : request body (may be empty)
//   delayMs : timeout applied to each phase (resolve, connect, send, receive)
// Returns the status code and body; code == 0 and error filled on failure.
// An HTTP error status (4xx, 5xx) is not a failure here: the caller reads it.
Response Request (const std::string& method, const std::string& url, const std::vector<std::string>& headers,
				 const std::string& body, int delayMs = 20000);

// Percent-encodes a URL parameter (UTF-8 bytes, RFC 3986): letters, digits
// and "-_.~" are kept, every other byte becomes %XX.
std::string EncodeUrl (const std::string& text);

}	// namespace Http

#endif
