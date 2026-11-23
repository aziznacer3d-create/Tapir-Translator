// *****************************************************************************
// Tapir Translator - engine: minimal JSON reader / writer (header only)
//
//   Just what the engine needs for its HTTP protocol, keys.json and the
//   online services' responses: objects, arrays, strings (escapes \" \\ \/
//   \b \f \n \r \t and \uXXXX, including UTF-16 surrogate pairs), numbers,
//   true / false / null. Strings are returned in UTF-8.
//
//   Json::Value   a parsed value (tagged union: kind + one member per kind)
//   Json::Reader  recursive-descent parser (depth limited to 64)
//   Json::Write   indented writer (used to rewrite keys.json)
//   Json::String   quotes and escapes one string (used to build JSON by hand)
// *****************************************************************************

#ifndef TT_ENGINE_JSON_HPP
#define TT_ENGINE_JSON_HPP

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace Json {

// One JSON value. Only the member matching "kind" is meaningful; a
// default-constructed value is null.
struct Value {
	enum Kind { Null, Bool, Number, String, Array, Object };
	Kind kind = Null;
	bool boolean = false;					// Bool
	double number = 0.0;					// Number
	std::string str;						// String (UTF-8)
	std::vector<Value> array;			// Array
	std::map<std::string, Value> object;	// Object (keys kept sorted; a repeated key keeps its last value)

	// Member "name" of an object, or nullptr if this is not an object or the
	// member does not exist.
	const Value* Field (const std::string& name) const
	{
		if (kind != Object)
			return nullptr;
		auto it = object.find (name);
		return it == object.end () ? nullptr : &it->second;
	}

	// String member "name" of an object, or defaultValue if it is missing or not a
	// string.
	std::string Text (const std::string& name, const std::string& defaultValue = std::string ()) const
	{
		const Value* v = Field (name);
		return v != nullptr && v->kind == String ? v->str : defaultValue;
	}
};


// Recursive-descent JSON parser. Usage: Json::Reader (text).Read (value).
// The reader keeps a reference to the text: the text must outlive it.
class Reader {
public:
	explicit Reader (const std::string& text) : s_ (text) {}

	// Parses the whole text into v. Returns false on a syntax error, on
	// nesting deeper than 64 levels, or if anything but whitespace follows
	// the value. On failure v may be partially filled.
	bool Read (Value& v)
	{
		SkipSpaces ();
		if (!Element (v))
			return false;
		SkipSpaces ();
		return i_ == s_.size ();
	}

private:
	// Skips JSON whitespace (space, tab, CR, LF).
	void SkipSpaces ()
	{
		while (i_ < s_.size () && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
			++i_;
	}

	// Parses one value at the current position, chosen by its first character:
	// '{' object, '[' array, '"' string, true / false / null, else a number.
	// The depth counter protects the stack against hostile deeply nested input.
	bool Element (Value& v)
	{
		if (i_ >= s_.size () || ++depth_ > 64)
			return false;
		bool ok = false;
		const char c = s_[i_];
		if (c == '{')
			ok = ReadObject (v);
		else if (c == '[')
			ok = ReadArray (v);
		else if (c == '"') {
			v.kind = Value::String;
			ok = ReadString (v.str);
		} else if (s_.compare (i_, 4, "true") == 0) {
			v.kind = Value::Bool;
			v.boolean = true;
			i_ += 4;
			ok = true;
		} else if (s_.compare (i_, 5, "false") == 0) {
			v.kind = Value::Bool;
			i_ += 5;
			ok = true;
		} else if (s_.compare (i_, 4, "null") == 0) {
			i_ += 4;
			ok = true;
		} else
			ok = ReadNumber (v);
		--depth_;
		return ok;
	}

	// Parses an object; the current character is '{'.
	// Step 1: empty object "{}".
	// Step 2: loop on  "key" : value  followed by ',' (next member) or '}' (end).
	bool ReadObject (Value& v)
	{
		v.kind = Value::Object;
		++i_;
		SkipSpaces ();
		if (i_ < s_.size () && s_[i_] == '}') {
			++i_;
			return true;
		}
		for (;;) {
			SkipSpaces ();
			std::string key;
			if (i_ >= s_.size () || s_[i_] != '"' || !ReadString (key))
				return false;
			SkipSpaces ();
			if (i_ >= s_.size () || s_[i_] != ':')
				return false;
			++i_;
			SkipSpaces ();
			Value e;
			if (!Element (e))
				return false;
			v.object[key] = std::move (e);
			SkipSpaces ();
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
	bool ReadArray (Value& v)
	{
		v.kind = Value::Array;
		++i_;
		SkipSpaces ();
		if (i_ < s_.size () && s_[i_] == ']') {
			++i_;
			return true;
		}
		for (;;) {
			SkipSpaces ();
			Value e;
			if (!Element (e))
				return false;
			v.array.push_back (std::move (e));
			SkipSpaces ();
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
	bool ReadNumber (Value& v)
	{
		const size_t start = i_;
		while (i_ < s_.size () && (std::string ("+-0123456789.eE").find (s_[i_]) != std::string::npos))
			++i_;
		if (i_ == start)
			return false;
		v.kind = Value::Number;
		v.number = std::atof (s_.substr (start, i_ - start).c_str ());
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
	bool ReadString (std::string& out)
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
						unsigned long low = 0;
						if (!Hex4 (low))
							return false;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
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
	int depth_ = 0;		// current nesting depth
};


// Forward declaration: Write uses String, defined below.
inline std::string String (const std::string& text);

// Appends v to output as indented JSON (2 spaces per level).
//   indent : indentation of the current level (0 at the top)
// The keys of an object are written in alphabetical order (std::map order),
// except that keys starting with "_" (explanations meant for the user) come
// first. Numbers use "%.15g". Empty arrays / objects are written "[]" / "{}".
inline void Write (const Value& v, std::string& output, int indent = 0)
{
	const std::string margin (static_cast<size_t> (indent + 2), ' ');
	const std::string end_ (static_cast<size_t> (indent), ' ');
	switch (v.kind) {
		case Value::Null:		output += "null"; break;
		case Value::Bool:	output += v.boolean ? "true" : "false"; break;
		case Value::Number: {
			char t[32];
			std::snprintf (t, sizeof (t), "%.15g", v.number);
			output += t;
			break;
		}
		case Value::String:	output += String (v.str); break;
		case Value::Array: {
			if (v.array.empty ()) {
				output += "[]";
				break;
			}
			output += "[\n";
			for (size_t i = 0; i < v.array.size (); ++i) {
				output += margin;
				Write (v.array[i], output, indent + 2);
				output += i + 1 < v.array.size () ? ",\n" : "\n";
			}
			output += end_ + "]";
			break;
		}
		case Value::Object: {
			if (v.object.empty ()) {
				output += "{}";
				break;
			}
			// Order of the members: "_..." keys first, then the others
			std::vector<const std::pair<const std::string, Value>*> fields;
			for (const auto& c : v.object) {
				if (!c.first.empty () && c.first[0] == '_')
					fields.push_back (&c);
			}
			for (const auto& c : v.object) {
				if (c.first.empty () || c.first[0] != '_')
					fields.push_back (&c);
			}
			output += "{\n";
			for (size_t i = 0; i < fields.size (); ++i) {
				output += margin + String (fields[i]->first) + ": ";
				Write (fields[i]->second, output, indent + 2);
				output += i + 1 < fields.size () ? ",\n" : "\n";
			}
			output += end_ + "}";
			break;
		}
	}
}

// Returns text as a quoted JSON string literal: quote, backslash, LF, CR and
// tab get their short escape, other control characters below 0x20 become
// \u00XX; every other byte (including UTF-8 sequences) is copied unchanged.
inline std::string String (const std::string& text)
{
	std::string r = "\"";
	for (const char c : text) {
		switch (c) {
			case '"':  r += "\\\""; break;
			case '\\': r += "\\\\"; break;
			case '\n': r += "\\n";  break;
			case '\r': r += "\\r";  break;
			case '\t': r += "\\t";  break;
			default:
				if (static_cast<unsigned char> (c) < 0x20) {
					char buffer[8];
					std::snprintf (buffer, sizeof (buffer), "\\u%04x", static_cast<unsigned> (static_cast<unsigned char> (c)));
					r += buffer;
				} else
					r += c;
		}
	}
	return r + "\"";
}

}	// namespace Json

#endif
