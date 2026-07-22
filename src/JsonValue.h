#ifndef HAIKU_RADIO_JSON_VALUE_H
#define HAIKU_RADIO_JSON_VALUE_H

#include <map>
#include <string>
#include <utility>
#include <vector>

// Minimal recursive-descent JSON parser. No third-party dependency so the
// project builds with nothing beyond a C++11 compiler and the Haiku SDK.
// Only covers what radio-browser's /json/stations response needs: objects,
// arrays, strings, numbers, booleans and null.
class JsonValue {
public:
	enum Type { Null, Boolean, Number, String, Array, Object };

	Type type;
	bool boolValue;
	double numberValue;
	std::string stringValue;
	std::vector<JsonValue> arrayValue;
	std::vector<std::pair<std::string, JsonValue> > objectValue;

	JsonValue() : type(Null), boolValue(false), numberValue(0) {}

	bool IsObject() const { return type == Object; }
	bool IsArray() const { return type == Array; }
	bool IsString() const { return type == String; }

	// Object member lookup. Returns NULL if this isn't an object or the
	// key is absent.
	const JsonValue* Find(const std::string& key) const;

	std::string AsString(const std::string& fallback = "") const;
	double AsDouble(double fallback = 0) const;
	int AsInt(int fallback = 0) const;

	// Throws std::runtime_error with a position on malformed input.
	static JsonValue Parse(const std::string& text);
};

#endif
