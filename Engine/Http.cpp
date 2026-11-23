// *****************************************************************************
// Tapir Translator - engine: HTTP(S) client based on WinHTTP (see Http.hpp)
//
//   Implementation of Http::Request (one synchronous WinHTTP request with the
//   automatic system proxy) and Http::EncodeUrl (RFC 3986 percent-encoding).
// *****************************************************************************

#include "Http.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdio>

#pragma comment (lib, "winhttp.lib")

namespace Http {

namespace {

// Converts a UTF-8 string to UTF-16 (WinHTTP only takes wide strings).
// First call computes the length, second call converts.
std::wstring Wide (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

// Builds the error message for a failed WinHTTP call, from GetLastError():
//   "<step_> : error WinHTTP <code>" followed, for the common codes, by a
//   short explanation in parentheses (timeout, unknown host, refused
//   certificate...). Must be called right after the failing call.
//   step_ : name of the step that failed
std::string WinHttpError (const char* step_)
{
	const DWORD code = GetLastError ();
	const char* sens = "";
	switch (code) {
		case ERROR_WINHTTP_TIMEOUT:					sens = " (delai depasse)"; break;
		case ERROR_WINHTTP_NAME_NOT_RESOLVED:		sens = " (nom de serveur introuvable : pas d'Internet ?)"; break;
		case ERROR_WINHTTP_CANNOT_CONNECT:			sens = " (connexion impossible)"; break;
		case ERROR_WINHTTP_SECURE_FAILURE:			sens = " (certificat refuse)"; break;
		case ERROR_WINHTTP_CONNECTION_ERROR:		sens = " (connexion coupee)"; break;
		default: break;
	}
	return std::string (step_) + " : erreur WinHTTP " + std::to_string (code) + sens;
}

// RAII wrapper for a WinHTTP handle: closed automatically on every return path.
struct WinHttpHandle {
	HINTERNET h = nullptr;
	~WinHttpHandle () { if (h != nullptr) WinHttpCloseHandle (h); }
};

}	// namespace


// Sends one HTTP(S) request and reads the whole response (see Http.hpp).
Response Request (const std::string& method, const std::string& url, const std::vector<std::string>& headers,
				 const std::string& body, int delayMs)
{
	// Step 1: split the URL into host, port, path and query string
	// (fixed-size buffers: host 256, path 4096, query 8192 characters)
	Response r;
	const std::wstring urlW = Wide (url);
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof (parts);
	wchar_t host[256] = {};
	wchar_t path[4096] = {};
	wchar_t query[8192] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = 4096;
	parts.lpszExtraInfo = query;
	parts.dwExtraInfoLength = 8192;
	if (!WinHttpCrackUrl (urlW.c_str (), static_cast<DWORD> (urlW.size ()), 0, &parts)) {
		r.error = WinHttpError ("adresse illisible");
		return r;
	}

	// Step 2: WinHTTP session with the automatic system proxy (WPAD, Windows
	// settings); WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1 or later
	WinHttpHandle session, connection, request;
	session.h = WinHttpOpen (L"TapirTranslator/0.5", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
							 WINHTTP_NO_PROXY_BYPASS, 0);
	if (session.h == nullptr) {
		r.error = WinHttpError ("WinHttpOpen");
		return r;
	}
	// Same timeout for name resolution, connection, sending and receiving
	WinHttpSetTimeouts (session.h, delayMs, delayMs, delayMs, delayMs);
	// Step 3: connection to the host (no network traffic yet)
	connection.h = WinHttpConnect (session.h, host, parts.nPort, 0);
	if (connection.h == nullptr) {
		r.error = WinHttpError ("WinHttpConnect");
		return r;
	}
	// Step 4: request object for "path?query"; TLS when the scheme is https
	const std::wstring object = std::wstring (path) + query;
	request.h = WinHttpOpenRequest (connection.h, Wide (method).c_str (), object.c_str (), nullptr, WINHTTP_NO_REFERER,
									WINHTTP_DEFAULT_ACCEPT_TYPES,
									parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (request.h == nullptr) {
		r.error = WinHttpError ("WinHttpOpenRequest");
		return r;
	}
	// Step 5: headers joined with CRLF (length -1 = null-terminated string),
	// then send the body and wait for the response headers
	std::wstring allHeaders;
	for (const std::string& e : headers)
		allHeaders += Wide (e) + L"\r\n";
	if (!WinHttpSendRequest (request.h, allHeaders.empty () ? WINHTTP_NO_ADDITIONAL_HEADERS : allHeaders.c_str (),
							 allHeaders.empty () ? 0 : static_cast<DWORD> (-1L),
							 body.empty () ? WINHTTP_NO_REQUEST_DATA : const_cast<char*> (body.data ()),
							 static_cast<DWORD> (body.size ()), static_cast<DWORD> (body.size ()), 0) ||
		!WinHttpReceiveResponse (request.h, nullptr)) {
		r.error = WinHttpError ("envoi de la requete");
		return r;
	}
	// Step 6: HTTP status code, as a number
	DWORD code = 0;
	DWORD size = sizeof (code);
	WinHttpQueryHeaders (request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
						 &code, &size, WINHTTP_NO_HEADER_INDEX);
	// Step 7: read the body chunk by chunk until no data is left; a read
	// error keeps what was received so far; stop beyond 16 MB
	for (;;) {
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable (request.h, &available) || available == 0)
			break;
		const size_t before = r.body.size ();
		r.body.resize (before + available);
		DWORD read_ = 0;
		if (!WinHttpReadData (request.h, &r.body[before], available, &read_))
			break;
		r.body.resize (before + read_);
		if (r.body.size () > 16u * 1024u * 1024u)
			break;
	}
	r.code = static_cast<int> (code);
	return r;
}


// Percent-encodes a URL parameter (see Http.hpp). Each byte of the UTF-8
// text is handled separately, so a multi-byte character becomes several %XX.
std::string EncodeUrl (const std::string& text)
{
	std::string r;
	char buffer[4];
	for (const unsigned char c : text) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			r += static_cast<char> (c);
		else {
			std::snprintf (buffer, sizeof (buffer), "%%%02X", c);
			r += buffer;
		}
	}
	return r;
}

}	// namespace Http
