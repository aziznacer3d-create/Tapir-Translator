// *****************************************************************************
// Tapir Translator - engine: minimal JSON reader / writer (header only)
//
//   Just what the engine needs for its HTTP protocol, cles.json and the
//   online services' responses: objects, arrays, strings (escapes \" \\ \/
//   \b \f \n \r \t and \uXXXX, including UTF-16 surrogate pairs), numbers,
//   true / false / null. Strings are returned in UTF-8.
//
//   Json::Valeur   a parsed value (tagged union: genre + one member per kind)
//   Json::Lecteur  recursive-descent parser (depth limited to 64)
//   Json::Ecrire   indented writer (used to rewrite cles.json)
//   Json::Chaine   quotes and escapes one string (used to build JSON by hand)
// *****************************************************************************

#ifndef TT_MOTEUR_JSON_HPP
#define TT_MOTEUR_JSON_HPP

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace Json {

// One JSON value. Only the member matching "genre" is meaningful; a
// default-constructed value is null.
struct Valeur {
	enum Genre { Nul, Booleen, Nombre, Chaine, Tableau, Objet };
	Genre genre = Nul;
	bool booleen = false;					// Booleen
	double nombre = 0.0;					// Nombre
	std::string chaine;						// Chaine (UTF-8)
	std::vector<Valeur> tableau;			// Tableau
	std::map<std::string, Valeur> objet;	// Objet (keys kept sorted; a repeated key keeps its last value)

	// Member "nom" of an object, or nullptr if this is not an object or the
	// member does not exist.
	const Valeur* Champ (const std::string& nom) const
	{
		if (genre != Objet)
			return nullptr;
		auto it = objet.find (nom);
		return it == objet.end () ? nullptr : &it->second;
	}

	// String member "nom" of an object, or defaut if it is missing or not a
	// string.
	std::string Texte (const std::string& nom, const std::string& defaut = std::string ()) const
	{
		const Valeur* v = Champ (nom);
		return v != nullptr && v->genre == Chaine ? v->chaine : defaut;
	}
};


// Recursive-descent JSON parser. Usage: Json::Lecteur (texte).Lire (valeur).
// The reader keeps a reference to the text: the text must outlive it.
class Lecteur {
public:
	explicit Lecteur (const std::string& texte) : s_ (texte) {}

	// Parses the whole text into v. Returns false on a syntax error, on
	// nesting deeper than 64 levels, or if anything but whitespace follows
	// the value. On failure v may be partially filled.
	bool Lire (Valeur& v)
	{
		Blancs ();
		if (!Element (v))
			return false;
		Blancs ();
		return i_ == s_.size ();
	}

private:
	// Skips JSON whitespace (space, tab, CR, LF).
	void Blancs ()
	{
		while (i_ < s_.size () && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
			++i_;
	}

	// Parses one value at the current position, chosen by its first character:
	// '{' object, '[' array, '"' string, true / false / null, else a number.
	// The depth counter protects the stack against hostile deeply nested input.
	bool Element (Valeur& v)
	{
		if (i_ >= s_.size () || ++profondeur_ > 64)
			return false;
		bool ok = false;
		const char c = s_[i_];
		if (c == '{')
			ok = LireObjet (v);
		else if (c == '[')
			ok = LireTableau (v);
		else if (c == '"') {
			v.genre = Valeur::Chaine;
			ok = LireChaine (v.chaine);
		} else if (s_.compare (i_, 4, "true") == 0) {
			v.genre = Valeur::Booleen;
			v.booleen = true;
			i_ += 4;
			ok = true;
		} else if (s_.compare (i_, 5, "false") == 0) {
			v.genre = Valeur::Booleen;
			i_ += 5;
			ok = true;
		} else if (s_.compare (i_, 4, "null") == 0) {
			i_ += 4;
			ok = true;
		} else
			ok = LireNombre (v);
		--profondeur_;
		return ok;
	}

	// Parses an object; the current character is '{'.
	// Step 1: empty object "{}".
	// Step 2: loop on  "key" : value  followed by ',' (next member) or '}' (end).
	bool LireObjet (Valeur& v)
	{
		v.genre = Valeur::Objet;
		++i_;
		Blancs ();
		if (i_ < s_.size () && s_[i_] == '}') {
			++i_;
			return true;
		}
		for (;;) {
			Blancs ();
			std::string cle;
			if (i_ >= s_.size () || s_[i_] != '"' || !LireChaine (cle))
				return false;
			Blancs ();
			if (i_ >= s_.size () || s_[i_] != ':')
				return false;
			++i_;
			Blancs ();
			Valeur e;
			if (!Element (e))
				return false;
			v.objet[cle] = std::move (e);
			Blancs ();
			if (i_ < s_.size () && s_[i_] == ',') {
				++i_;
				continue;
			}
			if (i_ < s_.size () && s_[i_] == '}') {
				++i_;
				return true;
			}
			return false;
		}
	}

	// Parses an array; the current character is '['.
	// Step 1: empty array "[]".
	// Step 2: loop on  value  followed by ',' (next element) or ']' (end).
	bool LireTableau (Valeur& v)
	{
		v.genre = Valeur::Tableau;
		++i_;
		Blancs ();
		if (i_ < s_.size () && s_[i_] == ']') {
			++i_;
			return true;
		}
		for (;;) {
			Blancs ();
			Valeur e;
			if (!Element (e))
				return false;
			v.tableau.push_back (std::move (e));
			Blancs ();
			if (i_ < s_.size () && s_[i_] == ',') {
				++i_;
				continue;
			}
			if (i_ < s_.size () && s_[i_] == ']') {
				++i_;
				return true;
			}
			return false;
		}
	}

	// Parses a number: takes the longest run of the characters "+-0123456789.eE"
	// and converts it with atof (lenient: the exact JSON number grammar is not
	// checked). Fails if there is no such character (unknown token).
	bool LireNombre (Valeur& v)
	{
		const size_t debut = i_;
		while (i_ < s_.size () && (std::string ("+-0123456789.eE").find (s_[i_]) != std::string::npos))
			++i_;
		if (i_ == debut)
			return false;
		v.genre = Valeur::Nombre;
		v.nombre = std::atof (s_.substr (debut, i_ - debut).c_str ());
		return true;
	}

	// Appends the code point cp to out, encoded in UTF-8 (1 to 4 bytes).
	static void Utf8 (unsigned long cp, std::string& out)
	{
		if (cp < 0x80)
			out += static_cast<char> (cp);
		else if (cp < 0x800) {
			out += static_cast<char> (0xC0 | (cp >> 6));
			out += static_cast<char> (0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += static_cast<char> (0xE0 | (cp >> 12));
			out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char> (0x80 | (cp & 0x3F));
		} else {
			out += static_cast<char> (0xF0 | (cp >> 18));
			out += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char> (0x80 | (cp & 0x3F));
		}
	}

	// Reads the 4 hexadecimal digits of a \uXXXX escape into cp.
	// Returns false if fewer than 4 characters remain or one is not a hex digit.
	bool Hex4 (unsigned long& cp)
	{
		if (i_ + 4 > s_.size ())
			return false;
		cp = 0;
		for (int k = 0; k < 4; ++k) {
			const char c = s_[i_++];
			cp <<= 4;
			if (c >= '0' && c <= '9')
				cp |= static_cast<unsigned long> (c - '0');
			else if (c >= 'a' && c <= 'f')
				cp |= static_cast<unsigned long> (c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				cp |= static_cast<unsigned long> (c - 'A' + 10);
			else
				return false;
		}
		return true;
	}

	// Parses a string; the current character is the opening quote.
	// Ordinary bytes are copied as they are (the text is UTF-8 already);
	// escapes are decoded. Returns false on an unknown escape or a missing
	// closing quote.
	bool LireChaine (std::string& out)
	{
		++i_;	// skip the opening quote
		out.clear ();
		while (i_ < s_.size ()) {
			const char c = s_[i_++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i_ >= s_.size ())
				return false;
			const char e = s_[i_++];
			switch (e) {
				case '"':  out += '"';  break;
				case '\\': out += '\\'; break;
				case '/':  out += '/';  break;
				case 'b':  out += '\b'; break;
				case 'f':  out += '\f'; break;
				case 'n':  out += '\n'; break;
				case 'r':  out += '\r'; break;
				case 't':  out += '\t'; break;
				case 'u': {
					// \uXXXX; a high surrogate (D800-DBFF) followed by a second
					// \uXXXX is combined with it into one code point above U+FFFF
					unsigned long cp = 0;
					if (!Hex4 (cp))
						return false;
					if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size () && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
						i_ += 2;
						unsigned long bas = 0;
						if (!Hex4 (bas))
							return false;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (bas - 0xDC00);
					}
					Utf8 (cp, out);
					break;
				}
				default:
					return false;
			}
		}
		return false;
	}

	const std::string& s_;		// text being parsed (not copied)
	size_t i_ = 0;				// current position in s_
	int profondeur_ = 0;		// current nesting depth
};


// Forward declaration: Ecrire uses Chaine, defined below.
inline std::string Chaine (const std::string& texte);

// Appends v to sortie as indented JSON (2 spaces per level).
//   retrait : indentation of the current level (0 at the top)
// The keys of an object are written in alphabetical order (std::map order),
// except that keys starting with "_" (explanations meant for the user) come
// first. Numbers use "%.15g". Empty arrays / objects are written "[]" / "{}".
inline void Ecrire (const Valeur& v, std::string& sortie, int retrait = 0)
{
	const std::string marge (static_cast<size_t> (retrait + 2), ' ');
	const std::string fin (static_cast<size_t> (retrait), ' ');
	switch (v.genre) {
		case Valeur::Nul:		sortie += "null"; break;
		case Valeur::Booleen:	sortie += v.booleen ? "true" : "false"; break;
		case Valeur::Nombre: {
			char t[32];
			std::snprintf (t, sizeof (t), "%.15g", v.nombre);
			sortie += t;
			break;
		}
		case Valeur::Chaine:	sortie += Chaine (v.chaine); break;
		case Valeur::Tableau: {
			if (v.tableau.empty ()) {
				sortie += "[]";
				break;
			}
			sortie += "[\n";
			for (size_t i = 0; i < v.tableau.size (); ++i) {
				sortie += marge;
				Ecrire (v.tableau[i], sortie, retrait + 2);
				sortie += i + 1 < v.tableau.size () ? ",\n" : "\n";
			}
			sortie += fin + "]";
			break;
		}
		case Valeur::Objet: {
			if (v.objet.empty ()) {
				sortie += "{}";
				break;
			}
			// Order of the members: "_..." keys first, then the others
			std::vector<const std::pair<const std::string, Valeur>*> champs;
			for (const auto& c : v.objet) {
				if (!c.first.empty () && c.first[0] == '_')
					champs.push_back (&c);
			}
			for (const auto& c : v.objet) {
				if (c.first.empty () || c.first[0] != '_')
					champs.push_back (&c);
			}
			sortie += "{\n";
			for (size_t i = 0; i < champs.size (); ++i) {
				sortie += marge + Chaine (champs[i]->first) + ": ";
				Ecrire (champs[i]->second, sortie, retrait + 2);
				sortie += i + 1 < champs.size () ? ",\n" : "\n";
			}
			sortie += fin + "}";
			break;
		}
	}
}

// Returns texte as a quoted JSON string literal: quote, backslash, LF, CR and
// tab get their short escape, other control characters below 0x20 become
// \u00XX; every other byte (including UTF-8 sequences) is copied unchanged.
inline std::string Chaine (const std::string& texte)
{
	std::string r = "\"";
	for (const char c : texte) {
		switch (c) {
			case '"':  r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\n': r += "\\n";  break;
			case '\r': r += "\\r";  break;
			case '\t': r += "\\t";  break;
			default:
				if (static_cast<unsigned char> (c) < 0x20) {
					char tampon[8];
					std::snprintf (tampon, sizeof (tampon), "\\u%04x", static_cast<unsigned> (static_cast<unsigned char> (c)));
					r += tampon;
				} else
					r += c;
		}
	}
	return r + "\"";
}

}	// namespace Json

#endif
