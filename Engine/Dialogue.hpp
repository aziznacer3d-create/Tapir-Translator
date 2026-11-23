// *****************************************************************************
// Tapir Translator - engine: "MyMemory quota reached" window
//
//   Opened by the engine (a process separate from ArchiCAD) when MyMemory
//   answers "MYMEMORY WARNING" (or HTTP 429): it offers to enter an e-mail
//   address, which raises the free quota from about 5 000 to about 50 000
//   characters per day according to the MyMemory documentation. No password
//   is asked: a MyMemory account does not raise the quota (the account key is
//   only used for private translation memories).
//
//   The texts of the window come from TT_Texts (the tool's 20 UI languages).
// *****************************************************************************

#ifndef TT_ENGINE_DIALOGUE_HPP
#define TT_ENGINE_DIALOGUE_HPP

#include <string>

// How the window was closed.
//   Closed      : closed without a choice (title-bar close button), or "OK" /
//                 Escape in the information-only variant
//   Saved : a valid address was entered and "Save" pressed
//   Later    : "Later" pressed (or Escape) in the input variant
// The values are also used as the engine's exit code with --show-quota.
enum class QuotaAnswer { Closed, Saved, Later };

// Shows the window modally (own message loop) and returns the user's answer.
//   email     : pre-filled address; receives the entered address when the
//               result is Saved
//   withEmail : true when an e-mail is already configured, i.e. the
//               50 000 characters quota is reached too -> information only
//               (no input field, a single "OK" button)
//   language    : UI language index (TT_Texts.hpp)
QuotaAnswer AskMyMemoryEmail (std::string& email, bool withEmail, int language);

#endif
