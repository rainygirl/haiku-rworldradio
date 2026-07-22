#include "JsonValue.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

class Parser {
public:
	explicit Parser(const std::string& text) : fText(text), fPos(0) {}

	JsonValue ParseDocument()
	{
		SkipWhitespace();
		JsonValue value = ParseValue();
		SkipWhitespace();
		if (fPos != fText.size())
			Fail("trailing data after JSON document");
		return value;
	}

private:
	const std::string& fText;
	size_t fPos;

	void Fail(const std::string& message)
	{
		char offset[32];
		snprintf(offset, sizeof(offset), "%lu", static_cast<unsigned long>(fPos));
		throw std::runtime_error("JSON parse error at offset "
			+ std::string(offset) + ": " + message);
	}

	char Peek()
	{
		if (fPos >= fText.size())
			Fail("unexpected end of input");
		return fText[fPos];
	}

	char Next()
	{
		char c = Peek();
		fPos++;
		return c;
	}

	void Expect(char expected)
	{
		if (Next() != expected)
			Fail(std::string("expected '") + expected + "'");
	}

	void SkipWhitespace()
	{
		while (fPos < fText.size()) {
			char c = fText[fPos];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
				fPos++;
			else
				break;
		}
	}

	bool Consume(const char* literal)
	{
		size_t len = strlen(literal);
		if (fText.compare(fPos, len, literal) == 0) {
			fPos += len;
			return true;
		}
		return false;
	}

	JsonValue ParseValue()
	{
		SkipWhitespace();
		char c = Peek();
		if (c == '{')
			return ParseObject();
		if (c == '[')
			return ParseArray();
		if (c == '"')
			return ParseString();
		if (c == 't' || c == 'f')
			return ParseBoolean();
		if (c == 'n')
			return ParseNull();
		if (c == '-' || (c >= '0' && c <= '9'))
			return ParseNumber();
		Fail("unexpected character");
		return JsonValue();
	}

	JsonValue ParseObject()
	{
		Expect('{');
		JsonValue value;
		value.type = JsonValue::Object;
		SkipWhitespace();
		if (Peek() == '}') {
			Next();
			return value;
		}
		while (true) {
			SkipWhitespace();
			JsonValue key = ParseString();
			SkipWhitespace();
			Expect(':');
			JsonValue member = ParseValue();
			value.objectValue.push_back(
				std::make_pair(key.stringValue, member));
			SkipWhitespace();
			char c = Next();
			if (c == ',')
				continue;
			if (c == '}')
				break;
			Fail("expected ',' or '}' in object");
		}
		return value;
	}

	JsonValue ParseArray()
	{
		Expect('[');
		JsonValue value;
		value.type = JsonValue::Array;
		SkipWhitespace();
		if (Peek() == ']') {
			Next();
			return value;
		}
		while (true) {
			value.arrayValue.push_back(ParseValue());
			SkipWhitespace();
			char c = Next();
			if (c == ',')
				continue;
			if (c == ']')
				break;
			Fail("expected ',' or ']' in array");
		}
		return value;
	}

	static void AppendUtf8(std::string& out, unsigned int codepoint)
	{
		if (codepoint <= 0x7F) {
			out += static_cast<char>(codepoint);
		} else if (codepoint <= 0x7FF) {
			out += static_cast<char>(0xC0 | (codepoint >> 6));
			out += static_cast<char>(0x80 | (codepoint & 0x3F));
		} else if (codepoint <= 0xFFFF) {
			out += static_cast<char>(0xE0 | (codepoint >> 12));
			out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (codepoint & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (codepoint >> 18));
			out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (codepoint & 0x3F));
		}
	}

	unsigned int ParseHex4()
	{
		unsigned int result = 0;
		for (int i = 0; i < 4; i++) {
			char c = Next();
			result <<= 4;
			if (c >= '0' && c <= '9')
				result |= static_cast<unsigned int>(c - '0');
			else if (c >= 'a' && c <= 'f')
				result |= static_cast<unsigned int>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				result |= static_cast<unsigned int>(c - 'A' + 10);
			else
				Fail("invalid \\u escape");
		}
		return result;
	}

	JsonValue ParseString()
	{
		Expect('"');
		JsonValue value;
		value.type = JsonValue::String;
		std::string& out = value.stringValue;
		while (true) {
			char c = Next();
			if (c == '"')
				break;
			if (c == '\\') {
				char escape = Next();
				switch (escape) {
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u': {
						unsigned int codepoint = ParseHex4();
						if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
							if (Next() != '\\' || Next() != 'u')
								Fail("expected low surrogate");
							unsigned int low = ParseHex4();
							codepoint = 0x10000
								+ ((codepoint - 0xD800) << 10)
								+ (low - 0xDC00);
						}
						AppendUtf8(out, codepoint);
						break;
					}
					default:
						Fail("invalid escape sequence");
				}
			} else {
				out += c;
			}
		}
		return value;
	}

	JsonValue ParseNumber()
	{
		size_t start = fPos;
		if (Peek() == '-')
			fPos++;
		while (fPos < fText.size() && isdigit(static_cast<unsigned char>(fText[fPos])))
			fPos++;
		if (fPos < fText.size() && fText[fPos] == '.') {
			fPos++;
			while (fPos < fText.size() && isdigit(static_cast<unsigned char>(fText[fPos])))
				fPos++;
		}
		if (fPos < fText.size() && (fText[fPos] == 'e' || fText[fPos] == 'E')) {
			fPos++;
			if (fPos < fText.size() && (fText[fPos] == '+' || fText[fPos] == '-'))
				fPos++;
			while (fPos < fText.size() && isdigit(static_cast<unsigned char>(fText[fPos])))
				fPos++;
		}
		JsonValue value;
		value.type = JsonValue::Number;
		value.numberValue = strtod(fText.substr(start, fPos - start).c_str(), NULL);
		return value;
	}

	JsonValue ParseBoolean()
	{
		JsonValue value;
		value.type = JsonValue::Boolean;
		if (Consume("true"))
			value.boolValue = true;
		else if (Consume("false"))
			value.boolValue = false;
		else
			Fail("invalid literal");
		return value;
	}

	JsonValue ParseNull()
	{
		if (!Consume("null"))
			Fail("invalid literal");
		return JsonValue();
	}

	static size_t strlen(const char* s)
	{
		size_t n = 0;
		while (s[n] != '\0')
			n++;
		return n;
	}
};

} // namespace

const JsonValue*
JsonValue::Find(const std::string& key) const
{
	if (type != Object)
		return NULL;
	for (size_t i = 0; i < objectValue.size(); i++) {
		if (objectValue[i].first == key)
			return &objectValue[i].second;
	}
	return NULL;
}

std::string
JsonValue::AsString(const std::string& fallback) const
{
	if (type == String)
		return stringValue;
	if (type == Number) {
		// radio-browser occasionally sends numeric-looking fields as
		// strings and vice versa; normalize to a plain string.
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%g", numberValue);
		return buffer;
	}
	return fallback;
}

double
JsonValue::AsDouble(double fallback) const
{
	if (type == Number)
		return numberValue;
	if (type == String)
		return strtod(stringValue.c_str(), NULL);
	return fallback;
}

int
JsonValue::AsInt(int fallback) const
{
	return static_cast<int>(AsDouble(fallback));
}

JsonValue
JsonValue::Parse(const std::string& text)
{
	Parser parser(text);
	return parser.ParseDocument();
}
