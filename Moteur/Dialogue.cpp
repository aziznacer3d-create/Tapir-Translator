// *****************************************************************************
// Tapir Translator - engine: "MyMemory quota reached" window (see Dialogue.hpp)
//
//   A plain Win32 window built by code (no resource file, the engine has
//   none), always on top, using the Windows message font and scaled to the
//   display DPI. Two variants:
//     - anonymous (no e-mail yet): explanation, e-mail input field, error
//       line, "Save" and "Later" buttons;
//     - with e-mail (the larger quota is reached too): explanation and a
//       single "OK" button.
//   IsDialogMessage provides Tab navigation, Enter (default button) and
//   Escape (IDCANCEL) as in a real dialog box.
// *****************************************************************************

#include "Dialogue.hpp"
#include "TT_Textes.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")

namespace {

// Control identifiers. The two buttons use IDOK / IDCANCEL so that Enter and
// Escape (translated by IsDialogMessage) trigger them.
enum { kTexte = 100, kEmail = 101, kEnregistrer = IDOK, kPlusTard = IDCANCEL, kErreur = 102 };

// Window state, attached to the window with GWLP_USERDATA and read by the
// window procedure.
struct Etat {
	ReponseQuota reponse = ReponseQuota::Fermee;	// result, Fermee until a button is pressed
	std::wstring email;			// address entered (valid when reponse == Enregistree)
	bool avecEmail = false;		// information-only variant (see Dialogue.hpp)
	HFONT police = nullptr;		// Windows message font, deleted at the end
	HWND champ = nullptr;		// e-mail input field (anonymous variant only)
	HWND erreur = nullptr;		// error line under the field (anonymous variant only)
	std::wstring invalide;		// "Invalid e-mail address." in the tool's language
};

// Converts a UTF-8 string to UTF-16 (Win32 W functions).
std::wstring Large (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

// Converts a UTF-16 string back to UTF-8.
std::string Etroit (const std::wstring& s)
{
	if (s.empty ())
		return std::string ();
	const int n = WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0, nullptr, nullptr);
	std::string r (static_cast<size_t> (n), '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n, nullptr, nullptr);
	return r;
}

// Minimal e-mail check (enough to catch typing mistakes, not RFC 5322):
// an "@" that is not the first character, a "." somewhere after it, no
// space, and no trailing ".". An empty string fails the first test, so
// e.back() is never reached on it.
bool EmailValide (const std::wstring& e)
{
	const size_t a = e.find (L'@');
	return a != std::wstring::npos && a > 0 && e.find (L'.', a) != std::wstring::npos && e.find (L' ') == std::wstring::npos &&
		   e.back () != L'.';
}

// Window procedure of the quota window.
//   WM_COMMAND "Save" (anonymous variant): validate the address, keep the
//              window open with an error message if invalid, otherwise store
//              it, set Enregistree and close.
//   WM_COMMAND "Later", or "OK" (information variant): set PlusTard (or
//              Fermee in the information variant) and close.
//   WM_CLOSE   : close; the answer stays Fermee.
//   WM_DESTROY : end the message loop of DemanderEmailMyMemory.
// The state pointer is null for the messages sent before SetWindowLongPtrW.
LRESULT CALLBACK Procedure (HWND fenetre, UINT message, WPARAM w, LPARAM l)
{
	Etat* etat = reinterpret_cast<Etat*> (GetWindowLongPtrW (fenetre, GWLP_USERDATA));
	switch (message) {
		case WM_COMMAND:
			if (etat != nullptr && LOWORD (w) == kEnregistrer && !etat->avecEmail) {
				// Step 1: read the field and trim leading / trailing spaces
				wchar_t tampon[256] = {};
				GetWindowTextW (etat->champ, tampon, 256);
				std::wstring e = tampon;
				while (!e.empty () && e.back () == L' ')
					e.pop_back ();
				while (!e.empty () && e.front () == L' ')
					e.erase (0, 1);
				// Step 2: invalid -> message under the field, focus back to it
				if (!EmailValide (e)) {
					SetWindowTextW (etat->erreur, etat->invalide.c_str ());
					SetFocus (etat->champ);
					return 0;
				}
				// Step 3: valid -> keep it and close the window
				etat->email = e;
				etat->reponse = ReponseQuota::Enregistree;
				DestroyWindow (fenetre);
				return 0;
			}
			if (etat != nullptr && (LOWORD (w) == kPlusTard || (LOWORD (w) == kEnregistrer && etat->avecEmail))) {
				etat->reponse = etat->avecEmail ? ReponseQuota::Fermee : ReponseQuota::PlusTard;
				DestroyWindow (fenetre);
				return 0;
			}
			break;
		case WM_CLOSE:
			DestroyWindow (fenetre);
			return 0;
		case WM_DESTROY:
			PostQuitMessage (0);
			return 0;
		default:
			break;
	}
	return DefWindowProcW (fenetre, message, w, l);
}

}	// namespace


// Builds and shows the quota window, runs its message loop and returns the
// answer (see Dialogue.hpp).
ReponseQuota DemanderEmailMyMemory (std::string& email, bool avecEmail, int langue)
{
	// Step 1: DPI awareness and scaling helper E(): sizes below are written
	// for 96 DPI and scaled to the screen's DPI
	SetProcessDPIAware ();
	HDC ecran = GetDC (nullptr);
	const int dpi = GetDeviceCaps (ecran, LOGPIXELSY);
	ReleaseDC (nullptr, ecran);
	const auto E = [dpi] (int v) { return MulDiv (v, dpi, 96); };

	// Step 2: window class (dialog-coloured background, information icon)
	HINSTANCE instance = GetModuleHandleW (nullptr);
	WNDCLASSW classe = {};
	classe.lpfnWndProc = Procedure;
	classe.hInstance = instance;
	classe.hCursor = LoadCursor (nullptr, IDC_ARROW);
	classe.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_BTNFACE + 1);
	classe.hIcon = LoadIcon (nullptr, IDI_INFORMATION);
	classe.lpszClassName = L"TapirTranslatorQuota";
	RegisterClassW (&classe);

	// Step 3: state, texts in the UI language, Windows message font
	Etat etat;
	etat.avecEmail = avecEmail;
	etat.invalide = Large (TT::Txt (TT::D_INVALIDE, langue));
	const std::wstring titre = L"Tapir Translator - " + Large (TT::Txt (TT::D_TITRE, langue));
	NONCLIENTMETRICSW mesures = {};
	mesures.cbSize = sizeof (mesures);
	SystemParametersInfoW (SPI_GETNONCLIENTMETRICS, sizeof (mesures), &mesures, 0);
	etat.police = CreateFontIndirectW (&mesures.lfMessageFont);

	// Step 4: top-level window centred on the primary screen; the client area
	// is 500 x 250 (anonymous) or 500 x 200 (information), AdjustWindowRectEx
	// adds the frame and title bar
	const int largeur = E (500), hauteur = E (avecEmail ? 200 : 250);
	RECT zone = { 0, 0, largeur, hauteur };
	AdjustWindowRectEx (&zone, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
	const int lf = zone.right - zone.left, hf = zone.bottom - zone.top;
	HWND fenetre = CreateWindowExW (WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, classe.lpszClassName, titre.c_str (),
									WS_CAPTION | WS_SYSMENU, (GetSystemMetrics (SM_CXSCREEN) - lf) / 2,
									(GetSystemMetrics (SM_CYSCREEN) - hf) / 2, lf, hf, nullptr, nullptr, instance, nullptr);
	if (fenetre == nullptr) {
		DeleteObject (etat.police);
		return ReponseQuota::Fermee;
	}
	// The window procedure reads the state from GWLP_USERDATA
	SetWindowLongPtrW (fenetre, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (&etat));

	// Step 5: explanation text; the information variant names the menu
	// "Tapir Translator > <online translator>" to choose another provider
	const std::wstring texte = avecEmail
		? Large (TT::TxtF (TT::D_AVEC_EMAIL, langue, { { "menu", std::string ("Tapir Translator > ") + TT::Txt (TT::M_TRADUCTEUR, langue) } }))
		: Large (TT::Txt (TT::D_ANONYME, langue));
	HWND statique = CreateWindowExW (0, L"STATIC", texte.c_str (), WS_CHILD | WS_VISIBLE, E (16), E (14), largeur - E (32), E (avecEmail ? 130 : 110),
									 fenetre, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kTexte)), instance, nullptr);
	SendMessageW (statique, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);

	// Step 6 (anonymous variant): label, input field pre-filled with the
	// address, and an initially empty error line below it
	int y = E (avecEmail ? 154 : 130);
	if (!avecEmail) {
		HWND libelle = CreateWindowExW (0, L"STATIC", Large (TT::Txt (TT::D_LIBELLE, langue)).c_str (), WS_CHILD | WS_VISIBLE, E (16), y + E (4),
										E (150), E (20), fenetre, nullptr, instance, nullptr);
		SendMessageW (libelle, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		etat.champ = CreateWindowExW (WS_EX_CLIENTEDGE, L"EDIT", Large (email).c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
									  E (170), y, largeur - E (186), E (26), fenetre,
									  reinterpret_cast<HMENU> (static_cast<INT_PTR> (kEmail)), instance, nullptr);
		SendMessageW (etat.champ, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		etat.erreur = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, E (170), y + E (30), largeur - E (186), E (20), fenetre,
									   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kErreur)), instance, nullptr);
		SendMessageW (etat.erreur, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		y += E (62);
	}
	// Step 7: buttons aligned on the right - default button "Save" (or "OK"),
	// then "Later" in the anonymous variant
	const int lb = E (110), hb = E (30);
	const std::wstring enregistrer = avecEmail ? std::wstring (L"OK") : Large (TT::Txt (TT::D_ENREGISTRER, langue));
	const std::wstring plusTardTexte = Large (TT::Txt (TT::D_PLUS_TARD, langue));
	HWND ok = CreateWindowExW (0, L"BUTTON", enregistrer.c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
							   largeur - E (16) - lb - (avecEmail ? 0 : lb + E (10)), y, lb, hb, fenetre,
							   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kEnregistrer)), instance, nullptr);
	SendMessageW (ok, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
	if (!avecEmail) {
		HWND plusTard = CreateWindowExW (0, L"BUTTON", plusTardTexte.c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP, largeur - E (16) - lb, y, lb, hb,
										 fenetre, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kPlusTard)), instance, nullptr);
		SendMessageW (plusTard, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
	}

	// Step 8: show it in front (the engine has no other window), flash the
	// taskbar button, select the field content so typing replaces it
	ShowWindow (fenetre, SW_SHOWNORMAL);
	SetForegroundWindow (fenetre);
	FlashWindow (fenetre, TRUE);
	if (etat.champ != nullptr) {
		SetFocus (etat.champ);
		SendMessageW (etat.champ, EM_SETSEL, 0, -1);
	}

	// Step 9: message loop until WM_DESTROY posts WM_QUIT; IsDialogMessage
	// handles Tab, Enter and Escape
	MSG msg;
	while (GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (fenetre, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	// Step 10: clean up (the class is unregistered so a later call can
	// register it again) and return the address entered, in UTF-8
	DeleteObject (etat.police);
	UnregisterClassW (classe.lpszClassName, instance);
	if (etat.reponse == ReponseQuota::Enregistree)
		email = Etroit (etat.email);
	return etat.reponse;
}
