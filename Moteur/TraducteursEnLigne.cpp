// *****************************************************************************
// Tapir Translator - moteur : traducteurs en ligne
//
//   deepl          DeepL API (Free : cle en ":fx" -> api-free.deepl.com)
//   microsoft      Microsoft Translator (Azure AI Translator, API v3)
//   google         Google Cloud Translation (API v2 "Basic")
//   mymemory       MyMemory (sans cle ; e-mail facultatif pour un quota plus grand)
//   libretranslate LibreTranslate (adresse du serveur + cle eventuelle)
//
//   Les cles sont lues dans cles.json a chaque requete (modifiables a chaud).
//   Pour le banc : TAPIR_TRANSLATOR_URL_<NOM> remplace l'adresse du service.
// *****************************************************************************

#include "Traducteur.hpp"
#include "Http.hpp"
#include "Json.hpp"

#include <cstdlib>

namespace {

std::string Majuscules (std::string s)
{
	for (char& c : s)
		c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
	return s;
}

std::string UrlService (const char* nom, const std::string& defaut)
{
	const std::string variable = std::string ("TAPIR_TRANSLATOR_URL_") + Majuscules (nom);
	char* v = nullptr;
	size_t n = 0;
	if (_dupenv_s (&v, &n, variable.c_str ()) == 0 && v != nullptr) {
		std::string r = v;
		std::free (v);
		if (!r.empty ())
			return r;
	}
	return defaut;
}

std::string Champ (const Json::Valeur& cles, const char* service, const char* champ)
{
	const Json::Valeur* s = cles.Champ (service);
	return s != nullptr ? s->Texte (champ) : std::string ();
}

std::string TableauJson (const std::vector<std::string>& textes)
{
	std::string r = "[";
	for (size_t i = 0; i < textes.size (); ++i)
		r += (i == 0 ? "" : ", ") + Json::Chaine (textes[i]);
	return r + "]";
}

// Message d'erreur du service (formats DeepL, Microsoft, Google, LibreTranslate)
std::string MessageService (const Http::Reponse& r)
{
	if (r.code == 0)
		return r.erreur;
	std::string detail;
	Json::Valeur v;
	if (Json::Lecteur (r.corps).Lire (v)) {
		detail = v.Texte ("message");
		if (detail.empty ())
			detail = v.Texte ("error");
		if (const Json::Valeur* e = v.Champ ("error"))
			if (e->genre == Json::Valeur::Objet)
				detail = e->Texte ("message");
	}
	if (detail.empty ())
		detail = r.corps.substr (0, 200);
	std::string sens;
	if (r.code == 401 || r.code == 403)
		sens = "cle refusee : ";
	else if (r.code == 429)
		sens = "trop de requetes : ";
	else if (r.code == 456)
		sens = "quota mensuel atteint : ";
	return "HTTP " + std::to_string (r.code) + " " + sens + detail;
}

bool Lire (const Http::Reponse& r, Json::Valeur& v, std::string& erreur)
{
	if (r.code != 200) {
		erreur = MessageService (r);
		return false;
	}
	if (!Json::Lecteur (r.corps).Lire (v)) {
		erreur = "reponse illisible : " + r.corps.substr (0, 200);
		return false;
	}
	return true;
}

class TraducteurEnLigne : public Traducteur {
public:
	bool Prend (const std::string& langue) const override { return LangueArchicad (langue); }
};


// --- DeepL -----------------------------------------------------------------------------
class DeepL : public TraducteurEnLigne {
public:
	explicit DeepL (std::string cle) : cle_ (std::move (cle)) {}
	std::string Nom () const override { return "DeepL"; }

	static std::string Cible (const std::string& l)
	{
		if (l == "en") return "EN-GB";
		if (l == "pt") return "PT-PT";
		if (l == "zh") return "ZH-HANS";
		if (l == "no") return "NB";
		return Majuscules (l);
	}
	static std::string Source (const std::string& l) { return l == "no" ? "NB" : Majuscules (l); }

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle DeepL absente (Options > Cles des traducteurs)";
			return false;
		}
		const bool gratuite = cle_.size () > 3 && cle_.compare (cle_.size () - 3, 3, ":fx") == 0;
		const std::string base = UrlService ("deepl", gratuite ? "https://api-free.deepl.com" : "https://api.deepl.com");
		std::string corps = "{\"text\": " + TableauJson (textes) + ", \"target_lang\": " + Json::Chaine (Cible (cible));
		if (!source.empty ())
			corps += ", \"source_lang\": " + Json::Chaine (Source (source));
		corps += "}";
		const Http::Reponse r = Http::Requete ("POST", base + "/v2/translate",
			{ "Authorization: DeepL-Auth-Key " + cle_, "Content-Type: application/json" }, corps);
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* t = v.Champ ("translations");
		if (t == nullptr || t->genre != Json::Valeur::Tableau || t->tableau.size () != textes.size ()) {
			erreur = "reponse DeepL incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : t->tableau)
			resultats.push_back (e.Texte ("text"));
		return true;
	}

private:
	std::string cle_;
};


// --- Microsoft Translator ----------------------------------------------------------------
class Microsoft : public TraducteurEnLigne {
public:
	Microsoft (std::string cle, std::string region) : cle_ (std::move (cle)), region_ (std::move (region)) {}
	std::string Nom () const override { return "Microsoft Translator"; }

	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-Hans";
		if (l == "no") return "nb";
		if (l == "pt") return "pt-pt";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle Microsoft absente (Options > Cles des traducteurs)";
			return false;
		}
		std::string url = UrlService ("microsoft", "https://api.cognitive.microsofttranslator.com") +
						  "/translate?api-version=3.0&to=" + Code (cible);
		if (!source.empty ())
			url += "&from=" + Code (source);
		std::string corps = "[";
		for (size_t i = 0; i < textes.size (); ++i)
			corps += std::string (i == 0 ? "" : ", ") + "{\"Text\": " + Json::Chaine (textes[i]) + "}";
		corps += "]";
		std::vector<std::string> entetes = { "Ocp-Apim-Subscription-Key: " + cle_, "Content-Type: application/json; charset=UTF-8" };
		if (!region_.empty ())
			entetes.push_back ("Ocp-Apim-Subscription-Region: " + region_);
		const Http::Reponse r = Http::Requete ("POST", url, entetes, corps);
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		if (v.genre != Json::Valeur::Tableau || v.tableau.size () != textes.size ()) {
			erreur = "reponse Microsoft incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : v.tableau) {
			const Json::Valeur* t = e.Champ ("translations");
			resultats.push_back (t != nullptr && t->genre == Json::Valeur::Tableau && !t->tableau.empty () ? t->tableau[0].Texte ("text")
																												: std::string ());
		}
		return true;
	}

private:
	std::string cle_, region_;
};


// --- Google Cloud Translation (v2) -------------------------------------------------------
class Google : public TraducteurEnLigne {
public:
	explicit Google (std::string cle) : cle_ (std::move (cle)) {}
	std::string Nom () const override { return "Google Cloud Translation"; }

	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle Google absente (Options > Cles des traducteurs)";
			return false;
		}
		const std::string url = UrlService ("google", "https://translation.googleapis.com") + "/language/translate/v2?key=" +
								Http::EncoderUrl (cle_);
		std::string corps = "{\"q\": " + TableauJson (textes) + ", \"target\": " + Json::Chaine (Code (cible)) + ", \"format\": \"text\"";
		if (!source.empty ())
			corps += ", \"source\": " + Json::Chaine (Code (source));
		corps += "}";
		const Http::Reponse r = Http::Requete ("POST", url, { "Content-Type: application/json; charset=utf-8" }, corps);
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* d = v.Champ ("data");
		const Json::Valeur* t = d != nullptr ? d->Champ ("translations") : nullptr;
		if (t == nullptr || t->genre != Json::Valeur::Tableau || t->tableau.size () != textes.size ()) {
			erreur = "reponse Google incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : t->tableau)
			resultats.push_back (e.Texte ("translatedText"));
		return true;
	}

private:
	std::string cle_;
};


// --- MyMemory (une requete par texte) -----------------------------------------------------
class MyMemory : public TraducteurEnLigne {
public:
	explicit MyMemory (std::string email) : email_ (std::move (email)) {}
	std::string Nom () const override { return "MyMemory"; }

	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		resultats.clear ();
		const std::string base = UrlService ("mymemory", "https://api.mymemory.translated.net");
		for (const std::string& texte : textes) {
			std::string url = base + "/get?q=" + Http::EncoderUrl (texte) + "&langpair=" +
							  Http::EncoderUrl ((source.empty () ? std::string ("Autodetect") : Code (source)) + "|" + Code (cible));
			if (!email_.empty ())
				url += "&de=" + Http::EncoderUrl (email_);
			const Http::Reponse r = Http::Requete ("GET", url, {}, std::string ());
			Json::Valeur v;
			if (!Lire (r, v, erreur))
				return false;
			const Json::Valeur* statut = v.Champ ("responseStatus");
			const Json::Valeur* donnees = v.Champ ("responseData");
			const std::string traduit = donnees != nullptr ? donnees->Texte ("translatedText") : std::string ();
			const bool ok = statut != nullptr && ((statut->genre == Json::Valeur::Nombre && statut->nombre == 200.0) ||
												  (statut->genre == Json::Valeur::Chaine && statut->chaine == "200"));
			if (!ok || traduit.compare (0, 17, "MYMEMORY WARNING:") == 0) {
				erreur = "MyMemory : " + (traduit.empty () ? v.Texte ("responseDetails") : traduit.substr (0, 160));
				return false;
			}
			resultats.push_back (traduit);
		}
		return true;
	}

private:
	std::string email_;
};


// --- LibreTranslate ----------------------------------------------------------------------
class LibreTranslate : public TraducteurEnLigne {
public:
	LibreTranslate (std::string url, std::string cle) : url_ (std::move (url)), cle_ (std::move (cle)) {}
	std::string Nom () const override { return "LibreTranslate"; }

	static std::string Code (const std::string& l)
	{
		if (l == "no") return "nb";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		std::string base = UrlService ("libretranslate", url_);
		if (base.empty ()) {
			erreur = "adresse du serveur LibreTranslate absente (Options > Cles des traducteurs)";
			return false;
		}
		while (!base.empty () && base.back () == '/')
			base.pop_back ();
		std::string corps = "{\"q\": " + TableauJson (textes) + ", \"source\": " +
							Json::Chaine (source.empty () ? std::string ("auto") : Code (source)) + ", \"target\": " +
							Json::Chaine (Code (cible)) + ", \"format\": \"text\"";
		if (!cle_.empty ())
			corps += ", \"api_key\": " + Json::Chaine (cle_);
		corps += "}";
		const Http::Reponse r = Http::Requete ("POST", base + "/translate", { "Content-Type: application/json" }, corps);
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* t = v.Champ ("translatedText");
		resultats.clear ();
		if (t != nullptr && t->genre == Json::Valeur::Tableau && t->tableau.size () == textes.size ()) {
			for (const Json::Valeur& e : t->tableau)
				resultats.push_back (e.genre == Json::Valeur::Chaine ? e.chaine : std::string ());
			return true;
		}
		if (t != nullptr && t->genre == Json::Valeur::Chaine && textes.size () == 1) {
			resultats.push_back (t->chaine);
			return true;
		}
		erreur = "reponse LibreTranslate incomplete";
		return false;
	}

private:
	std::string url_, cle_;
};

}	// namespace


const char* const FournisseursEnLigne[] = { "deepl", "microsoft", "google", "mymemory", "libretranslate" };

std::unique_ptr<Traducteur> CreerTraducteurEnLigne (const std::string& nom, const Json::Valeur& cles)
{
	if (nom == "deepl")
		return std::make_unique<DeepL> (Champ (cles, "deepl", "cle"));
	if (nom == "microsoft")
		return std::make_unique<Microsoft> (Champ (cles, "microsoft", "cle"), Champ (cles, "microsoft", "region"));
	if (nom == "google")
		return std::make_unique<Google> (Champ (cles, "google", "cle"));
	if (nom == "mymemory")
		return std::make_unique<MyMemory> (Champ (cles, "mymemory", "email"));
	if (nom == "libretranslate")
		return std::make_unique<LibreTranslate> (Champ (cles, "libretranslate", "url"), Champ (cles, "libretranslate", "cle"));
	return nullptr;
}
