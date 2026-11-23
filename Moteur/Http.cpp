// *****************************************************************************
// Tapir Translator - engine: HTTP(S) client based on WinHTTP (see Http.hpp)
//
//   Implementation of Http::Requete (one synchronous WinHTTP request with the
//   automatic system proxy) and Http::EncoderUrl (RFC 3986 percent-encoding).
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
std::wstring Large (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

// Builds the error message for a failed WinHTTP call, from GetLastError():
//   "<etape> : erreur WinHTTP <code>" followed, for the common codes, by a
//   short explanation in parentheses (timeout, unknown host, refused
//   certificate...). Must be called right after the failing call.
//   etape : name of the step that failed
std::string ErreurWinHttp (const char* etape)
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
	return std::string (etape) + " : erreur WinHTTP " + std::to_string (code) + sens;
}

// RAII wrapper for a WinHTTP handle: closed automatically on every return path.
struct Poignee {
	HINTERNET h = nullptr;
	~Poignee () { if (h != nullptr) WinHttpCloseHandle (h); }
};

}	// namespace


// Sends one HTTP(S) request and reads the whole response (see Http.hpp).
Reponse Requete (const std::string& methode, const std::string& url, const std::vector<std::string>& entetes,
				 const std::string& corps, int delaiMs)
{
	// Step 1: split the URL into host, port, path and query string
	// (fixed-size buffers: host 256, path 4096, query 8192 characters)
	Reponse r;
	const std::wstring urlW = Large (url);
	URL_COMPONENTS parties = {};
	parties.dwStructSize = sizeof (parties);
	wchar_t hote[256] = {};
	wchar_t chemin[4096] = {};
	wchar_t requete[8192] = {};
	parties.lpszHostName = hote;
	parties.dwHostNameLength = 256;
	parties.lpszUrlPath = chemin;
	parties.dwUrlPathLength = 4096;
	parties.lpszExtraInfo = requete;
	parties.dwExtraInfoLength = 8192;
	if (!WinHttpCrackUrl (urlW.c_str (), static_cast<DWORD> (urlW.size ()), 0, &parties)) {
		r.erreur = ErreurWinHttp ("adresse illisible");
		return r;
	}

	// Step 2: WinHTTP session with the automatic system proxy (WPAD, Windows
	// settings); WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1 or later
	Poignee session, connexion, demande;
	session.h = WinHttpOpen (L"TapirTranslator/0.5", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
							 WINHTTP_NO_PROXY_BYPASS, 0);
	if (session.h == nullptr) {
		r.erreur = ErreurWinHttp ("WinHttpOpen");
		return r;
	}
	// Same timeout for name resolution, connection, sending and receiving
	WinHttpSetTimeouts (session.h, delaiMs, delaiMs, delaiMs, delaiMs);
	// Step 3: connection to the host (no network traffic yet)
	connexion.h = WinHttpConnect (session.h, hote, parties.nPort, 0);
	if (connexion.h == nullptr) {
		r.erreur = ErreurWinHttp ("WinHttpConnect");
		return r;
	}
	// Step 4: request object for "path?query"; TLS when the scheme is https
	const std::wstring objet = std::wstring (chemin) + requete;
	demande.h = WinHttpOpenRequest (connexion.h, Large (methode).c_str (), objet.c_str (), nullptr, WINHTTP_NO_REFERER,
									WINHTTP_DEFAULT_ACCEPT_TYPES,
									parties.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (demande.h == nullptr) {
		r.erreur = ErreurWinHttp ("WinHttpOpenRequest");
		return r;
	}
	// Step 5: headers joined with CRLF (length -1 = null-terminated string),
	// then send the body and wait for the response headers
	std::wstring tousEntetes;
	for (const std::string& e : entetes)
		tousEntetes += Large (e) + L"\r\n";
	if (!WinHttpSendRequest (demande.h, tousEntetes.empty () ? WINHTTP_NO_ADDITIONAL_HEADERS : tousEntetes.c_str (),
							 tousEntetes.empty () ? 0 : static_cast<DWORD> (-1L),
							 corps.empty () ? WINHTTP_NO_REQUEST_DATA : const_cast<char*> (corps.data ()),
							 static_cast<DWORD> (corps.size ()), static_cast<DWORD> (corps.size ()), 0) ||
		!WinHttpReceiveResponse (demande.h, nullptr)) {
		r.erreur = ErreurWinHttp ("envoi de la requete");
		return r;
	}
	// Step 6: HTTP status code, as a number
	DWORD code = 0;
	DWORD taille = sizeof (code);
	WinHttpQueryHeaders (demande.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
						 &code, &taille, WINHTTP_NO_HEADER_INDEX);
	// Step 7: read the body chunk by chunk until no data is left; a read
	// error keeps what was received so far; stop beyond 16 MB
	for (;;) {
		DWORD disponible = 0;
		if (!WinHttpQueryDataAvailable (demande.h, &disponible) || disponible == 0)
			break;
		const size_t avant = r.corps.size ();
		r.corps.resize (avant + disponible);
		DWORD lu = 0;
		if (!WinHttpReadData (demande.h, &r.corps[avant], disponible, &lu))
			break;
		r.corps.resize (avant + lu);
		if (r.corps.size () > 16u * 1024u * 1024u)
			break;
	}
	r.code = static_cast<int> (code);
	return r;
}


// Percent-encodes a URL parameter (see Http.hpp). Each byte of the UTF-8
// text is handled separately, so a multi-byte character becomes several %XX.
std::string EncoderUrl (const std::string& texte)
{
	std::string r;
	char tampon[4];
	for (const unsigned char c : texte) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			r += static_cast<char> (c);
		else {
			std::snprintf (tampon, sizeof (tampon), "%%%02X", c);
			r += tampon;
		}
	}
	return r;
}

}	// namespace Http
