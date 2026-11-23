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
#include "TT_Texts.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")

namespace {

// Control identifiers. The two buttons use IDOK / IDCANCEL so that Enter and
// Escape (translated by IsDialogMessage) trigger them.
enum { kText = 100, kEmail = 101, kSave = IDOK, kLater = IDCANCEL, kError = 102 };

// Window state, attached to the window with GWLP_USERDATA and read by the
// window procedure.
struct Status {
	QuotaAnswer response = QuotaAnswer::Closed;	// result, Closed until a button is pressed
	std::wstring email;			// address entered (valid when response == Saved)
	bool withEmail = false;		// information-only variant (see Dialogue.hpp)
	HFONT font = nullptr;		// Windows message font, deleted at the end
	HWND field = nullptr;		// e-mail input field (anonymous variant only)
	HWND error = nullptr;		// error line under the field (anonymous variant only)
	std::wstring invalid;		// "Invalid e-mail address." in the tool's language
};

// Converts a UTF-8 string to UTF-16 (Win32 W functions).
std::wstring Wide (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

// Converts a UTF-16 string back to UTF-8.
std::string Narrow (const std::wstring& s)
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
bool ValidEmail (const std::wstring& e)
{
	const size_t a = e.find (L'@');
	return a != std::wstring::npos && a > 0 && e.find (L'.', a) != std::wstring::npos && e.find (L' ') == std::wstring::npos &&
		   e.back () != L'.';
}

// Window procedure of the quota window.
//   WM_COMMAND "Save" (anonymous variant): validate the address, keep the
//              window open with an error message if invalid, otherwise store
//              it, set Saved and close.
//   WM_COMMAND "Later", or "OK" (information variant): set Later (or
//              Closed in the information variant) and close.
//   WM_CLOSE   : close; the answer stays Closed.
//   WM_DESTROY : end the message loop of AskMyMemoryEmail.
// The state pointer is null for the messages sent before SetWindowLongPtrW.
LRESULT CALLBACK Procedure (HWND window, UINT message, WPARAM w, LPARAM l)
{
	Status* status = reinterpret_cast<Status*> (GetWindowLongPtrW (window, GWLP_USERDATA));
	switch (message) {
		case WM_COMMAND:
			if (status != nullptr && LOWORD (w) == kSave && !status->withEmail) {
				// Step 1: read the field and trim leading / trailing spaces
				wchar_t buffer[256] = {};
				GetWindowTextW (status->field, buffer, 256);
				std::wstring e = buffer;
				while (!e.empty () && e.back () == L' ')
					e.pop_back ();
				while (!e.empty () && e.front () == L' ')
					e.erase (0, 1);
				// Step 2: invalid -> message under the field, focus back to it
				if (!ValidEmail (e)) {
					SetWindowTextW (status->error, status->invalid.c_str ());
					SetFocus (status->field);
					return 0;
				}
				// Step 3: valid -> keep it and close the window
				status->email = e;
				status->response = QuotaAnswer::Saved;
				DestroyWindow (window);
				return 0;
			}
			if (status != nullptr && (LOWORD (w) == kLater || (LOWORD (w) == kSave && status->withEmail))) {
				status->response = status->withEmail ? QuotaAnswer::Closed : QuotaAnswer::Later;
				DestroyWindow (window);
				return 0;
			}
			break;
		case WM_CLOSE:
			DestroyWindow (window);
			return 0;
		case WM_DESTROY:
			PostQuitMessage (0);
			return 0;
		default:
			break;
	}
	return DefWindowProcW (window, message, w, l);
}

}	// namespace


// Builds and shows the quota window, runs its message loop and returns the
// answer (see Dialogue.hpp).
QuotaAnswer AskMyMemoryEmail (std::string& email, bool withEmail, int language)
{
	// Step 1: DPI awareness and scaling helper E(): sizes below are written
	// for 96 DPI and scaled to the screen's DPI
	SetProcessDPIAware ();
	HDC screen = GetDC (nullptr);
	const int dpi = GetDeviceCaps (screen, LOGPIXELSY);
	ReleaseDC (nullptr, screen);
	const auto E = [dpi] (int v) { return MulDiv (v, dpi, 96); };

	// Step 2: window class (dialog-coloured background, information icon)
	HINSTANCE instance = GetModuleHandleW (nullptr);
	WNDCLASSW class_ = {};
	class_.lpfnWndProc = Procedure;
	class_.hInstance = instance;
	class_.hCursor = LoadCursor (nullptr, IDC_ARROW);
	class_.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_BTNFACE + 1);
	class_.hIcon = LoadIcon (nullptr, IDI_INFORMATION);
	class_.lpszClassName = L"TapirTranslatorQuota";
	RegisterClassW (&class_);

	// Step 3: state, texts in the UI language, Windows message font
	Status status;
	status.withEmail = withEmail;
	status.invalid = Wide (TT::Txt (TT::D_INVALID, language));
	const std::wstring title = L"Tapir Translator - " + Wide (TT::Txt (TT::D_TITLE, language));
	NONCLIENTMETRICSW metrics = {};
	metrics.cbSize = sizeof (metrics);
	SystemParametersInfoW (SPI_GETNONCLIENTMETRICS, sizeof (metrics), &metrics, 0);
	status.font = CreateFontIndirectW (&metrics.lfMessageFont);

	// Step 4: top-level window centred on the primary screen; the client area
	// is 500 x 250 (anonymous) or 500 x 200 (information), AdjustWindowRectEx
	// adds the frame and title bar
	const int width = E (500), height = E (withEmail ? 200 : 250);
	RECT zone = { 0, 0, width, height };
	AdjustWindowRectEx (&zone, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
	const int lf = zone.right - zone.left, hf = zone.bottom - zone.top;
	HWND window = CreateWindowExW (WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, class_.lpszClassName, title.c_str (),
									WS_CAPTION | WS_SYSMENU, (GetSystemMetrics (SM_CXSCREEN) - lf) / 2,
									(GetSystemMetrics (SM_CYSCREEN) - hf) / 2, lf, hf, nullptr, nullptr, instance, nullptr);
	if (window == nullptr) {
		DeleteObject (status.font);
		return QuotaAnswer::Closed;
	}
	// The window procedure reads the state from GWLP_USERDATA
	SetWindowLongPtrW (window, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (&status));

	// Step 5: explanation text; the information variant names the menu
	// "Tapir Translator > <online translator>" to choose another provider
	const std::wstring text = withEmail
		? Wide (TT::TxtF (TT::D_WITH_EMAIL, language, { { "menu", std::string ("Tapir Translator > ") + TT::Txt (TT::M_TRANSLATOR, language) } }))
		: Wide (TT::Txt (TT::D_ANONYMOUS, language));
	HWND staticText = CreateWindowExW (0, L"STATIC", text.c_str (), WS_CHILD | WS_VISIBLE, E (16), E (14), width - E (32), E (withEmail ? 130 : 110),
									 window, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kText)), instance, nullptr);
	SendMessageW (staticText, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);

	// Step 6 (anonymous variant): label, input field pre-filled with the
	// address, and an initially empty error line below it
	int y = E (withEmail ? 154 : 130);
	if (!withEmail) {
		HWND label = CreateWindowExW (0, L"STATIC", Wide (TT::Txt (TT::D_LABEL, language)).c_str (), WS_CHILD | WS_VISIBLE, E (16), y + E (4),
										E (150), E (20), window, nullptr, instance, nullptr);
		SendMessageW (label, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);
		status.field = CreateWindowExW (WS_EX_CLIENTEDGE, L"EDIT", Wide (email).c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
									  E (170), y, width - E (186), E (26), window,
									  reinterpret_cast<HMENU> (static_cast<INT_PTR> (kEmail)), instance, nullptr);
		SendMessageW (status.field, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);
		status.error = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, E (170), y + E (30), width - E (186), E (20), window,
									   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kError)), instance, nullptr);
		SendMessageW (status.error, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);
		y += E (62);
	}
	// Step 7: buttons aligned on the right - default button "Save" (or "OK"),
	// then "Later" in the anonymous variant
	const int lb = E (110), hb = E (30);
	const std::wstring enregistrer = withEmail ? std::wstring (L"OK") : Wide (TT::Txt (TT::D_SAVE, language));
	const std::wstring laterText = Wide (TT::Txt (TT::D_LATER, language));
	HWND ok = CreateWindowExW (0, L"BUTTON", enregistrer.c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
							   width - E (16) - lb - (withEmail ? 0 : lb + E (10)), y, lb, hb, window,
							   reinterpret_cast<HMENU> (static_cast<INT_PTR> (kSave)), instance, nullptr);
	SendMessageW (ok, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);
	if (!withEmail) {
		HWND later = CreateWindowExW (0, L"BUTTON", laterText.c_str (), WS_CHILD | WS_VISIBLE | WS_TABSTOP, width - E (16) - lb, y, lb, hb,
										 window, reinterpret_cast<HMENU> (static_cast<INT_PTR> (kLater)), instance, nullptr);
		SendMessageW (later, WM_SETFONT, reinterpret_cast<WPARAM> (status.font), TRUE);
	}

	// Step 8: show it in front (the engine has no other window), flash the
	// taskbar button, select the field content so typing replaces it
	ShowWindow (window, SW_SHOWNORMAL);
	SetForegroundWindow (window);
	FlashWindow (window, TRUE);
	if (status.field != nullptr) {
		SetFocus (status.field);
		SendMessageW (status.field, EM_SETSEL, 0, -1);
	}

	// Step 9: message loop until WM_DESTROY posts WM_QUIT; IsDialogMessage
	// handles Tab, Enter and Escape
	MSG msg;
	while (GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (window, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	// Step 10: clean up (the class is unregistered so a later call can
	// register it again) and return the address entered, in UTF-8
	DeleteObject (status.font);
	UnregisterClassW (class_.lpszClassName, instance);
	if (status.response == QuotaAnswer::Saved)
		email = Narrow (status.email);
	return status.response;
}
