// *****************************************************************************
// Tapir Translator - engine: online translators
//
//   The five providers the user can choose from (menu "Translator en line"),
//   each one implementing the Translator interface (Translator.hpp). Every
//   call is a synchronous HTTPS request made with Http::Request (WinHTTP).
//
//   deepl           DeepL API v2 - a key ending in ":fx" is a Free key
//                   (api-free.deepl.com), any other key a Pro key (api.deepl.com)
//                   POST /v2/translate, header "Authorization: DeepL-Auth-Key <key>"
//                   body     {"text": [...], "target_lang": "EN-GB", "source_lang"?: "FR"}
//                   response {"translations": [{"text": "..."}, ...]}
//   microsoft       Microsoft Translator (Azure AI Translator, API v3)
//                   POST /translate?api-version=3.0&to=<l>[&from=<l>]
//                   headers "Ocp-Apim-Subscription-Key" (+ "-Region" if given)
//                   body     [{"Text": "..."}, ...]
//                   response [{"translations": [{"text": "..."}]}, ...]
//   google          Google Cloud Translation (API v2 "Basic")
//                   POST /language/translate/v2?key=<key>
//                   body     {"q": [...], "target": "en", "format": "text", "source"?: "fr"}
//                   response {"data": {"translations": [{"translatedText": "..."}, ...]}}
//   mymemory        MyMemory - no key; an optional e-mail ("de=") raises the
//                   free daily quota. One GET per text:
//                   GET /get?q=<text>&langpair=<src>|<dst>[&de=<email>]
//                   response {"responseData": {"translatedText": "..."}, "responseStatus": 200}
//                   A translatedText starting with "MYMEMORY WARNING:" means the
//                   quota is reached (EngineMain.cpp then offers to enter an e-mail).
//   libretranslate  LibreTranslate - server URL (+ optional API key)
//                   POST <url>/translate
//                   body     {"q": [...], "source": "auto" | "fr", "target": "en", "format": "text", "api_key"?: "..."}
//                   response {"translatedText": [...]}  (or a single string for one text)
//
//   The keys come from keys.json, read again for every request (EngineMain.cpp)
//   so they can be changed while the engine runs. Every provider must return
//   exactly one translation per input text, in the same order.
//   Test bench: the environment variable TAPIR_TRANSLATOR_URL_<NAME> (e.g.
//   TAPIR_TRANSLATOR_URL_DEEPL) replaces the service base URL, so a fake local
//   server can answer in each service's format.
// *****************************************************************************

#include "Translator.hpp"
#include "Http.hpp"
#include "Json.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

// Returns an ASCII upper-case copy of the string (used for DeepL language
// codes and environment variable names).
std::string Uppercase (std::string s)
{
	for (char& c : s)
		c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
	return s;
}

// Base URL of a service.
//   name    : provider name ("deepl"...), upper-cased into TAPIR_TRANSLATOR_URL_<NAME>
//   defaultValue : official address of the service
// Returns the value of that environment variable when it is set and not
// empty (test bench), otherwise defaultValue.
std::string ServiceUrl (const char* name, const std::string& defaultValue)
{
	const std::string variable = std::string ("TAPIR_TRANSLATOR_URL_") + Uppercase (name);
	char* v = nullptr;
	size_t n = 0;
	if (_dupenv_s (&v, &n, variable.c_str ()) == 0 && v != nullptr) {
		std::string r = v;
		std::free (v);
		if (!r.empty ())
			return r;
	}
	return defaultValue;
}

// Reads keys.json["<service>"]["<field>"] as a string; "" when missing.
std::string Field (const Json::Value& keys, const char* service, const char* field)
{
	const Json::Value* s = keys.Field (service);
	return s != nullptr ? s->Text (field) : std::string ();
}

// Serialises a list of texts as a JSON array of strings: ["a", "b"].
std::string JsonArray (const std::vector<std::string>& texts)
{
	std::string r = "[";
	for (size_t i = 0; i < texts.size (); ++i)
		r += (i == 0 ? "" : ", ") + Json::String (texts[i]);
	return r + "]";
}

// Builds the error message of a failed call, written to log.txt.
//   r : the HTTP response
// No response at all -> the network error from Http::Request. Otherwise
// "HTTP <code> <meaning>: <detail>", where the detail is taken from the error
// formats of the services: {"message": ...} (DeepL), {"error": "..."}
// (LibreTranslate), {"error": {"message": ...}} (Microsoft, Google), or else
// the first 200 bytes of the body.
std::string ServiceMessage (const Http::Response& r)
{
	if (r.code == 0)
		return r.error;
	// Step 1: detail extracted from the JSON body, if any
	std::string detail;
	Json::Value v;
	if (Json::Reader (r.body).Read (v)) {
		detail = v.Text ("message");
		if (detail.empty ())
			detail = v.Text ("error");
		if (const Json::Value* e = v.Field ("error"))
			if (e->kind == Json::Value::Object)
				detail = e->Text ("message");
	}
	if (detail.empty ())
		detail = r.body.substr (0, 200);
	// Step 2: plain meaning of the usual codes (456 = DeepL quota exceeded)
	std::string meaning;
	if (r.code == 401 || r.code == 403)
		meaning = "key refused: ";
	else if (r.code == 429)
		meaning = "too many requests: ";
	else if (r.code == 456)
		meaning = "monthly quota reached: ";
	return "HTTP " + std::to_string (r.code) + " " + meaning + detail;
}

// Checks a service response and parses its JSON body.
//   r      : HTTP response
//   v      : output, the parsed body
//   error : output, filled when false is returned
// Returns true only for HTTP 200 with a valid JSON body.
bool Read (const Http::Response& r, Json::Value& v, std::string& error)
{
	if (r.code != 200) {
		error = ServiceMessage (r);
		return false;
	}
	if (!Json::Reader (r.body).Read (v)) {
		error = "unreadable response: " + r.body.substr (0, 200);
		return false;
	}
	return true;
}

// Common base of the online translators: they all accept the 20 ArchiCAD
// languages (each subclass maps the codes to the service's own codes).
class OnlineTranslator : public Translator {
public:
	bool Supports (const std::string& language) const override { return ArchicadLanguage (language); }
};


// --- DeepL -----------------------------------------------------------------------------
// DeepL API v2: the whole batch in one POST request.
class DeepL : public OnlineTranslator {
public:
	// key : DeepL authentication key from keys.json ("deepl": {"key": ...})
	explicit DeepL (std::string key) : key_ (std::move (key)) {}
	std::string Name () const override { return "DeepL"; }

	// DeepL target language code: DeepL requires a variant for English,
	// Portuguese and Chinese (British English, European Portuguese,
	// simplified Chinese); Norwegian is "NB" (Bokmal); other codes upper-cased.
	static std::string Target (const std::string& l)
	{
		if (l == "en") return "EN-GB";
		if (l == "pt") return "PT-PT";
		if (l == "zh") return "ZH-HANS";
		if (l == "no") return "NB";
		return Uppercase (l);
	}
	// DeepL source language code: no variants for sources, only "NB" for Norwegian.
	static std::string Source (const std::string& l) { return l == "no" ? "NB" : Uppercase (l); }

	// Translates the batch (see Translator::Translate).
	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		if (key_.empty ()) {
			error = "DeepL key missing (Online translator > Translator keys)";
			return false;
		}
		// Step 1: Free keys end with ":fx" and use a different host
		const bool freeTier = key_.size () > 3 && key_.compare (key_.size () - 3, 3, ":fx") == 0;
		const std::string base = ServiceUrl ("deepl", freeTier ? "https://api-free.deepl.com" : "https://api.deepl.com");
		// Step 2: JSON body; no "source_lang" = DeepL detects the language
		std::string body = "{\"text\": " + JsonArray (texts) + ", \"target_lang\": " + Json::String (Target (target));
		if (!source.empty ())
			body += ", \"source_lang\": " + Json::String (Source (source));
		body += "}";
		// Step 3: send the request
		const Http::Response r = Http::Request ("POST", base + "/v2/translate",
			{ "Authorization: DeepL-Auth-Key " + key_, "Content-Type: application/json" }, body);
		// Step 4: {"translations": [{"text": ...}]}, one entry per text
		Json::Value v;
		if (!Read (r, v, error))
			return false;
		const Json::Value* t = v.Field ("translations");
		if (t == nullptr || t->kind != Json::Value::Array || t->array.size () != texts.size ()) {
			error = "incomplete DeepL response";
			return false;
		}
		results.clear ();
		for (const Json::Value& e : t->array)
			results.push_back (e.Text ("text"));
		return true;
	}

private:
	std::string key_;
};


// --- Microsoft Translator ----------------------------------------------------------------
// Azure AI Translator, API v3: the whole batch in one POST request.
class Microsoft : public OnlineTranslator {
public:
	// key    : subscription key ("microsoft": {"key": ...})
	// region : Azure resource region ("microsoft": {"region": ...}), required
	//          by regional / multi-service resources, may be empty otherwise
	Microsoft (std::string key, std::string region) : key_ (std::move (key)), region_ (std::move (region)) {}
	std::string Name () const override { return "Microsoft Translator"; }

	// Microsoft language code: simplified Chinese, Norwegian Bokmal,
	// European Portuguese; other codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-Hans";
		if (l == "no") return "nb";
		if (l == "pt") return "pt-pt";
		return l;
	}

	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		if (key_.empty ()) {
			error = "Microsoft key missing (Online translator > Translator keys)";
			return false;
		}
		// Step 1: languages go in the query string; no "from" = auto-detection
		std::string url = ServiceUrl ("microsoft", "https://api.cognitive.microsofttranslator.com") +
						  "/translate?api-version=3.0&to=" + Code (target);
		if (!source.empty ())
			url += "&from=" + Code (source);
		// Step 2: body [{"Text": ...}, ...]
		std::string body = "[";
		for (size_t i = 0; i < texts.size (); ++i)
			body += std::string (i == 0 ? "" : ", ") + "{\"Text\": " + Json::String (texts[i]) + "}";
		body += "]";
		// Step 3: key header, plus the region header when a region is configured
		std::vector<std::string> headers = { "Ocp-Apim-Subscription-Key: " + key_, "Content-Type: application/json; charset=UTF-8" };
		if (!region_.empty ())
			headers.push_back ("Ocp-Apim-Subscription-Region: " + region_);
		const Http::Response r = Http::Request ("POST", url, headers, body);
		// Step 4: the response is an array with one element per text, each
		// holding "translations": [{"text": ...}] (an entry without it gives "")
		Json::Value v;
		if (!Read (r, v, error))
			return false;
		if (v.kind != Json::Value::Array || v.array.size () != texts.size ()) {
			error = "incomplete Microsoft response";
			return false;
		}
		results.clear ();
		for (const Json::Value& e : v.array) {
			const Json::Value* t = e.Field ("translations");
			results.push_back (t != nullptr && t->kind == Json::Value::Array && !t->array.empty () ? t->array[0].Text ("text")
																												: std::string ());
		}
		return true;
	}

private:
	std::string key_, region_;
};


// --- Google Cloud Translation (v2) -------------------------------------------------------
// Google Cloud Translation v2 ("Basic") with an API key: one POST request.
class Google : public OnlineTranslator {
public:
	// key : API key ("google": {"key": ...})
	explicit Google (std::string key) : key_ (std::move (key)) {}
	std::string Name () const override { return "Google Cloud Translation"; }

	// Google language code: simplified Chinese, European Portuguese; other
	// codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		if (key_.empty ()) {
			error = "Google key missing (Online translator > Translator keys)";
			return false;
		}
		// Step 1: the key goes in the query string (URL-encoded)
		const std::string url = ServiceUrl ("google", "https://translation.googleapis.com") + "/language/translate/v2?key=" +
								Http::EncodeUrl (key_);
		// Step 2: body; "format": "text" so the texts are not treated as HTML
		// (no entity escaping); no "source" = auto-detection
		std::string body = "{\"q\": " + JsonArray (texts) + ", \"target\": " + Json::String (Code (target)) + ", \"format\": \"text\"";
		if (!source.empty ())
			body += ", \"source\": " + Json::String (Code (source));
		body += "}";
		const Http::Response r = Http::Request ("POST", url, { "Content-Type: application/json; charset=utf-8" }, body);
		// Step 3: {"data": {"translations": [{"translatedText": ...}]}}, one per text
		Json::Value v;
		if (!Read (r, v, error))
			return false;
		const Json::Value* d = v.Field ("data");
		const Json::Value* t = d != nullptr ? d->Field ("translations") : nullptr;
		if (t == nullptr || t->kind != Json::Value::Array || t->array.size () != texts.size ()) {
			error = "incomplete Google response";
			return false;
		}
		results.clear ();
		for (const Json::Value& e : t->array)
			results.push_back (e.Text ("translatedText"));
		return true;
	}

private:
	std::string key_;
};


// --- MyMemory (one request per text) ------------------------------------------------------
// MyMemory free API: no key, one GET request per text, kParallel requests at a time.
class MyMemory : public OnlineTranslator {
public:
	// email : optional address ("mymemory": {"email": ...}) sent as "de=",
	//         which raises the free daily quota
	explicit MyMemory (std::string email) : email_ (std::move (email)) {}
	std::string Name () const override { return "MyMemory"; }

	// MyMemory language code: simplified Chinese, European Portuguese; other
	// codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	// Requests sent at the same time: one text per request and about 0.7 s each (new TLS
	// connection every time), so a window of 300 texts took minutes one by one.
	static constexpr size_t kParallel = 6;

	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		results.assign (texts.size (), std::string ());
		error.clear ();
		const std::string base = ServiceUrl ("mymemory", "https://api.mymemory.translated.net");
		const std::string pair = Http::EncodeUrl ((source.empty () ? std::string ("Autodetect") : Code (source)) + "|" + Code (target));
		// The whole batch fails as soon as one text fails (the layer retries later).
		// Each worker takes the next text; results[i] is written by one worker only.
		std::atomic<size_t> next {0};
		std::atomic<bool> failed {false};
		std::mutex errorLock;
		auto fail = [&] (const std::string& e) {
			std::lock_guard<std::mutex> guard (errorLock);
			// The quota warning wins: EngineMain.cpp looks for "MYMEMORY WARNING" to offer the e-mail
			if (error.empty () || e.find ("MYMEMORY WARNING") != std::string::npos)
				error = e;
			failed = true;
		};
		auto work = [&] () {
			for (size_t i = next++; i < texts.size () && !failed; i = next++) {
				// Step 1: GET /get?q=<text>&langpair=<src>|<dst> ("Autodetect" when
				// the source is unknown), plus "&de=<email>" when configured
				std::string url = base + "/get?q=" + Http::EncodeUrl (texts[i]) + "&langpair=" + pair;
				if (!email_.empty ())
					url += "&de=" + Http::EncodeUrl (email_);
				const Http::Response r = Http::Request ("GET", url, {}, std::string ());
				// Step 2: {"responseData": {"translatedText": ...}, "responseStatus": 200}
				Json::Value v;
				std::string readError;
				if (!Read (r, v, readError)) {
					fail (readError);
					return;
				}
				const Json::Value* status_code = v.Field ("responseStatus");
				const Json::Value* data = v.Field ("responseData");
				const std::string translated = data != nullptr ? data->Text ("translatedText") : std::string ();
				// Step 3: responseStatus may be a number or a string; a quota
				// warning arrives with HTTP 200 as the "translation" itself
				// (17 = length of "MYMEMORY WARNING:"). The error text keeps
				// "MYMEMORY WARNING" so EngineMain.cpp can recognise it.
				const bool ok = status_code != nullptr && ((status_code->kind == Json::Value::Number && status_code->number == 200.0) ||
													  (status_code->kind == Json::Value::String && status_code->str == "200"));
				if (!ok || translated.compare (0, 17, "MYMEMORY WARNING:") == 0) {
					fail ("MyMemory : " + (translated.empty () ? v.Text ("responseDetails") : translated.substr (0, 160)));
					return;
				}
				results[i] = translated;
			}
		};
		// Workers: kParallel - 1 threads plus this one
		std::vector<std::thread> workers;
		for (size_t k = 1; k < kParallel && k < texts.size (); ++k)
			workers.emplace_back (work);
		work ();
		for (std::thread& t : workers)
			t.join ();
		if (failed) {
			results.clear ();
			return false;
		}
		return true;
	}

private:
	std::string email_;
};


// --- LibreTranslate ----------------------------------------------------------------------
// LibreTranslate (self-hosted or public server): one POST request for the batch.
class LibreTranslate : public OnlineTranslator {
public:
	// url : server address ("libretranslate": {"url": ...})
	// key : API key if the server requires one ("libretranslate": {"key": ...})
	LibreTranslate (std::string url, std::string key) : url_ (std::move (url)), key_ (std::move (key)) {}
	std::string Name () const override { return "LibreTranslate"; }

	// LibreTranslate language code: Norwegian Bokmal is "nb"; others unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "no") return "nb";
		return l;
	}

	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		// Step 1: server address (no default: the user must configure one);
		// trailing slashes are removed before appending "/translate"
		std::string base = ServiceUrl ("libretranslate", url_);
		if (base.empty ()) {
			error = "LibreTranslate server address missing (Online translator > Translator keys)";
			return false;
		}
		while (!base.empty () && base.back () == '/')
			base.pop_back ();
		// Step 2: body; source "auto" = detection by the server
		std::string body = "{\"q\": " + JsonArray (texts) + ", \"source\": " +
							Json::String (source.empty () ? std::string ("auto") : Code (source)) + ", \"target\": " +
							Json::String (Code (target)) + ", \"format\": \"text\"";
		if (!key_.empty ())
			body += ", \"api_key\": " + Json::String (key_);
		body += "}";
		const Http::Response r = Http::Request ("POST", base + "/translate", { "Content-Type: application/json" }, body);
		// Step 3: "translatedText" is an array for an array "q"; some servers
		// answer a single string, accepted when there is only one text
		Json::Value v;
		if (!Read (r, v, error))
			return false;
		const Json::Value* t = v.Field ("translatedText");
		results.clear ();
		if (t != nullptr && t->kind == Json::Value::Array && t->array.size () == texts.size ()) {
			for (const Json::Value& e : t->array)
				results.push_back (e.kind == Json::Value::String ? e.str : std::string ());
			return true;
		}
		if (t != nullptr && t->kind == Json::Value::String && texts.size () == 1) {
			results.push_back (t->str);
			return true;
		}
		error = "incomplete LibreTranslate response";
		return false;
	}

private:
	std::string url_, key_;
};

}	// namespace


// Names of the online providers, as sent in "provider" and listed by GET /status.
const char* const OnlineProviders[] = { "deepl", "microsoft", "google", "mymemory", "libretranslate" };

// Creates the online translator called name, configured from keys.json.
//   name  : one of OnlineProviders
//   keys : parsed content of keys.json (may be null / empty: missing keys are
//          reported by Translate, so the error reaches the log)
// Returns nullptr for an unknown name.
std::unique_ptr<Translator> CreateOnlineTranslator (const std::string& name, const Json::Value& keys)
{
	if (name == "deepl")
		return std::make_unique<DeepL> (Field (keys, "deepl", "key"));
	if (name == "microsoft")
		return std::make_unique<Microsoft> (Field (keys, "microsoft", "key"), Field (keys, "microsoft", "region"));
	if (name == "google")
		return std::make_unique<Google> (Field (keys, "google", "key"));
	if (name == "mymemory")
		return std::make_unique<MyMemory> (Field (keys, "mymemory", "email"));
	if (name == "libretranslate")
		return std::make_unique<LibreTranslate> (Field (keys, "libretranslate", "url"), Field (keys, "libretranslate", "key"));
	return nullptr;
}
