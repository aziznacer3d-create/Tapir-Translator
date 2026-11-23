// *****************************************************************************
// Tapir Translator - moteur : fenetre "quota MyMemory atteint" (voir Dialogue.hpp)
//
//   Fenetre Win32 construite a la main (pas de ressource), au premier plan,
//   police des messages de Windows, mise a l'echelle de l'affichage (DPI).
// *****************************************************************************

#include "Dialogue.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")

namespace {

enum { kTexte = 100, kEmail = 101, kEnregistrer = IDOK, kPlusTard = IDCANCEL, kErreur = 102 };

struct Etat {
	ReponseQuota reponse = ReponseQuota::Fermee;
	std::wstring email;
	bool avecEmail = false;
	HFONT police = nullptr;
	HWND champ = nullptr;
	HWND erreur = nullptr;
};

std::wstring Large (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

std::string Etroit (const std::wstring& s)
{
	if (s.empty ())
		return std::string ();
	const int n = WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0, nullptr, nullptr);
	std::string r (static_cast<size_t> (n), '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n, nullptr, nullptr);
	return r;
}

bool EmailValide (const std::wstring& e)
{
	const size_t a = e.find (L'@');
	return a != std::wstring::npos && a > 0 && e.find (L'.', a) != std::wstring::npos && e.find (L' ') == std::wstring::npos &&
		   e.back () != L'.';
}

LRESULT CALLBACK Procedure (HWND fenetre, UINT message, WPARAM w, LPARAM l)
{
	Etat* etat = reinterpret_cast<Etat*> (GetWindowLongPtrW (fenetre, GWLP_USERDATA));
	switch (message) {
		case WM_COMMAND:
			if (etat != nullptr && LOWORD (w) == kEnregistrer && !etat->avecEmail) {
				wchar_t tampon[256] = {};
				GetWindowTextW (etat->champ, tampon, 256);
				std::wstring e = tampon;
				while (!e.empty () && e.back () == L' ')
					e.pop_back ();
				while (!e.empty () && e.front () == L' ')
					e.erase (0, 1);
				if (!EmailValide (e)) {
					SetWindowTextW (etat->erreur, L"Adresse e-mail invalide.");
					SetFocus (etat->champ);
					return 0;
				}
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


ReponseQuota DemanderEmailMyMemory (std::string& email, bool avecEmail)
{
	SetProcessDPIAware ();
	HDC ecran = GetDC (nullptr);
	const int dpi = GetDeviceCaps (ecran, LOGPIXELSY);
	ReleaseDC (nullptr, ecran);
	const auto E = [dpi] (int v) { return MulDiv (v, dpi, 96); };

	HINSTANCE instance = GetModuleHandleW (nullptr);
	WNDCLASSW classe = {};
	classe.lpfnWndProc = Procedure;
	classe.hInstance = instance;
	classe.hCursor = LoadCursor (nullptr, IDC_ARROW);
	classe.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_BTNFACE + 1);
	classe.hIcon = LoadIcon (nullptr, IDI_INFORMATION);
	classe.lpszClassName = L"TapirTranslatorQuota";
	RegisterClassW (&classe);

	Etat etat;
	etat.avecEmail = avecEmail;
	NONCLIENTMETRICSW mesures = {};
	mesures.cbSize = sizeof (mesures);
	SystemParametersInfoW (SPI_GETNONCLIENTMETRICS, sizeof (mesures), &mesures, 0);
	etat.police = CreateFontIndirectW (&mesures.lfMessageFont);

	const int largeur = E (500), hauteur = E (avecEmail ? 200 : 250);
	RECT zone = { 0, 0, largeur, hauteur };
	AdjustWindowRectEx (&zone, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
	const int lf = zone.right - zone.left, hf = zone.bottom - zone.top;
	HWND fenetre = CreateWindowExW (WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, classe.lpszClassName, L"Tapir Translator - quota MyMemory atteint",
									WS_CAPTION | WS_SYSMENU, (GetSystemMetrics (SM_CXSCREEN) - lf) / 2,
									(GetSystemMetrics (SM_CYSCREEN) - hf) / 2, lf, hf, nullptr, nullptr, instance, nullptr);
	if (fenetre == nullptr) {
		DeleteObject (etat.police);
		return ReponseQuota::Fermee;
	}
	SetWindowLongPtrW (fenetre, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (&etat));

	const wchar_t* texte = avecEmail
		? L"MyMemory a atteint aujourd'hui son quota gratuit, même avec votre adresse e-mail "
		  L"(environ 50 000 caractères par jour).\r\n\r\n"
		  L"Les textes déjà traduits restent affichés depuis le cache. Pour le reste : attendez demain, "
		  L"ou choisissez un autre traducteur dans ArchiCAD, menu Tapir Translator > Traducteur en ligne "
		  L"(DeepL et Microsoft offrent un quota mensuel gratuit, avec une clé)."
		: L"MyMemory a atteint son quota gratuit anonyme (environ 5 000 caractères par jour).\r\n\r\n"
		  L"Avec une adresse e-mail, MyMemory accorde environ 50 000 caractères par jour. "
		  L"Aucun compte ni mot de passe n'est nécessaire. L'adresse est envoyée à MyMemory avec chaque "
		  L"demande (pour vous joindre en cas de problème) et enregistrée dans cles.json sur ce poste.";
	HWND statique = CreateWindowExW (0, L"STATIC", texte, WS_CHILD | WS_VISIBLE, E (16), E (14), largeur - E (32), E (avecEmail ? 130 : 110),
									 fenetre, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kTexte)), instance, nullptr);
	SendMessageW (statique, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);

	int y = E (avecEmail ? 154 : 130);
	if (!avecEmail) {
		HWND libelle = CreateWindowExW (0, L"STATIC", L"Adresse e-mail :", WS_CHILD | WS_VISIBLE, E (16), y + E (4), E (110), E (20), fenetre,
										nullptr, instance, nullptr);
		SendMessageW (libelle, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		etat.champ = CreateWindowExW (WS_EX_CLIENTEDGE, L"EDIT", Large (email).c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
									  E (130), y, largeur - E (146), E (26), fenetre,
									  reinterpret_cast<HMENU> (static_cast<INT_PTR> (kEmail)), instance, nullptr);
		SendMessageW (etat.champ, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		etat.erreur = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, E (130), y + E (30), largeur - E (146), E (20), fenetre,
									   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kErreur)), instance, nullptr);
		SendMessageW (etat.erreur, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
		y += E (62);
	}
	const int lb = E (110), hb = E (30);
	HWND ok = CreateWindowExW (0, L"BUTTON", avecEmail ? L"OK" : L"Enregistrer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
							   largeur - E (16) - lb - (avecEmail ? 0 : lb + E (10)), y, lb, hb, fenetre,
							   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kEnregistrer)), instance, nullptr);
	SendMessageW (ok, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
	if (!avecEmail) {
		HWND plusTard = CreateWindowExW (0, L"BUTTON", L"Plus tard", WS_CHILD | WS_VISIBLE | WS_TABSTOP, largeur - E (16) - lb, y, lb, hb,
										 fenetre, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kPlusTard)), instance, nullptr);
		SendMessageW (plusTard, WM_SETFONT, reinterpret_cast<WPARAM> (etat.police), TRUE);
	}

	ShowWindow (fenetre, SW_SHOWNORMAL);
	SetForegroundWindow (fenetre);
	FlashWindow (fenetre, TRUE);
	if (etat.champ != nullptr) {
		SetFocus (etat.champ);
		SendMessageW (etat.champ, EM_SETSEL, 0, -1);
	}

	MSG msg;
	while (GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (fenetre, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	DeleteObject (etat.police);
	UnregisterClassW (classe.lpszClassName, instance);
	if (etat.reponse == ReponseQuota::Enregistree)
		email = Etroit (etat.email);
	return etat.reponse;
}
