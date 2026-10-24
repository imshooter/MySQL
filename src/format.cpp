#include "mysql.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace mysqlcomponent {

namespace {

size_t room(AMX* amx, cell address) {
	if (address < amx->hea) {
		return size_t(amx->hea - address) / sizeof(cell);
	}
	return size_t(amx->stp - address) / sizeof(cell);
}

void appendInteger(std::string& output, long long value) {
	char buffer[24];
	auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
	output.append(buffer, written.ptr);
}

void appendHex(std::string& output, ucell value) {
	char buffer[16];
	auto written = std::to_chars(buffer, buffer + sizeof buffer, value, 16);
	output.append(buffer, written.ptr);
}

void appendFloat(std::string& output, float value, int precision) {
	if (!std::isfinite(value)) {
		output.append("NULL");
		return;
	}
	char buffer[64];
	auto written = precision < 0
		? std::to_chars(buffer, buffer + sizeof buffer, value)
		: std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, std::min(precision, 30));
	output.append(buffer, written.ptr);
}

std::string lower(std::string_view text) {
	std::string result(text);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return result;
}

std::string_view trim(std::string_view text) {
	while (!text.empty() && std::isspace((unsigned char)text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && std::isspace((unsigned char)text.back())) {
		text.remove_suffix(1);
	}
	return text;
}

bool truth(std::string_view value) {
	std::string text = lower(value);
	return text == "1" || text == "true" || text == "yes" || text == "on";
}

bool number(std::string_view value, double& result) {
	std::string text(value);
	char* end = nullptr;
	result = std::strtod(text.c_str(), &end);
	return end != text.c_str() && *end == '\0';
}

bool setting(Settings& settings, std::string_view name, std::string_view value, std::string& error) {
	std::string key = lower(trim(name));
	value = trim(value);
	double amount = 0.0;
	if (key == "hostname" || key == "host") {
		settings.host = std::string(value);
	} else if (key == "username" || key == "user") {
		settings.user = std::string(value);
	} else if (key == "password") {
		settings.password = std::string(value);
	} else if (key == "database") {
		settings.database = std::string(value);
	} else if (key == "server_port" || key == "port") {
		if (!number(value, amount) || amount < 1.0 || amount > 65535.0) {
			error = "the port is not a number from 1 to 65535";
			return false;
		}
		settings.port = unsigned(amount);
	} else if (key == "charset") {
		settings.charset = lower(value);
	} else if (key == "multi_statements") {
		settings.multiStatements = truth(value);
	} else if (key == "pool_size" || key == "pool") {
		if (!number(value, amount) || amount < 0.0 || amount > 32.0) {
			error = "the pool is not a number from 0 to 32";
			return false;
		}
		settings.pool = int(amount);
	} else if (key == "connect_timeout" || key == "timeout") {
		if (!number(value, amount) || amount < 1.0) {
			error = "the timeout is not a number of seconds";
			return false;
		}
		settings.timeout = unsigned(amount);
	} else if (key == "read_timeout") {
		if (!number(value, amount) || amount < 0.0) {
			error = "read_timeout is not a number of seconds";
			return false;
		}
		settings.readTimeout = unsigned(amount);
	} else if (key == "write_timeout") {
		if (!number(value, amount) || amount < 0.0) {
			error = "write_timeout is not a number of seconds";
			return false;
		}
		settings.writeTimeout = unsigned(amount);
	} else if (key == "slow_query") {
		if (!number(value, amount) || amount < 0.0) {
			error = "slow_query is not a number of milliseconds";
			return false;
		}
		settings.slowQuery = amount;
	} else if (key == "log_errors") {
		settings.logErrors = truth(value);
	} else if (key == "max_query_length") {
		if (!number(value, amount) || amount < 0.0) {
			error = "max_query_length is not a number of bytes";
			return false;
		}
		settings.maxQueryLength = size_t(amount);
	} else if (key == "max_pending") {
		if (!number(value, amount) || amount < 0.0) {
			error = "max_pending is not a number of queries";
			return false;
		}
		settings.maxPending = size_t(amount);
	} else if (key == "max_queries_per_second") {
		if (!number(value, amount) || amount < 0.0) {
			error = "max_queries_per_second is not a number of queries";
			return false;
		}
		settings.maxPerSecond = size_t(amount);
	} else if (key == "ssl" || key == "ssl_enable") {
		settings.ssl = truth(value);
	} else if (key == "ssl_verify") {
		settings.sslVerify = truth(value);
	} else if (key == "ssl_ca" || key == "ssl_ca_file") {
		settings.sslCa = std::string(value);
	} else if (key == "ssl_capath" || key == "ssl_ca_path") {
		settings.sslCaPath = std::string(value);
	} else if (key == "ssl_cert" || key == "ssl_cert_file") {
		settings.sslCert = std::string(value);
	} else if (key == "ssl_key" || key == "ssl_key_file") {
		settings.sslKey = std::string(value);
	} else if (key == "ssl_crl") {
		settings.sslCrl = std::string(value);
	} else if (key == "ssl_crlpath") {
		settings.sslCrlPath = std::string(value);
	} else if (key == "ssl_cipher") {
		settings.sslCipher = std::string(value);
	} else if (key == "tls_version") {
		settings.tlsVersion = std::string(value);
	} else if (key == "ssl_fingerprint") {
		settings.sslFingerprint = std::string(value);
	} else if (key == "auto_reconnect") {
	} else {
		error = "unknown setting " + std::string(trim(name));
		return false;
	}
	return true;
}

bool safeCharset(const std::string& charset) {
	return charset != "big5" && charset != "cp932" && charset != "gbk" && charset != "gb18030" && charset != "sjis";
}

bool expand(std::string_view value, std::string& output, std::string& error) {
	output.clear();
	size_t at = 0;
	while (at < value.size()) {
		size_t start = value.find("${", at);
		if (start == std::string_view::npos) {
			output.append(value.substr(at));
			break;
		}
		size_t end = value.find('}', start + 2);
		if (end == std::string_view::npos) {
			error = "a ${ has no closing }";
			return false;
		}
		output.append(value.substr(at, start - at));
		std::string name(value.substr(start + 2, end - start - 2));
		const char* found = std::getenv(name.c_str());
		if (found == nullptr) {
			error = "the environment variable " + name + " is not set";
			return false;
		}
		output.append(found);
		at = end + 1;
	}
	return true;
}

}

cell* addressOf(AMX* amx, cell address) {
	Script* script = scriptOf(amx);
	cell* target = nullptr;
	if (script == nullptr || script->api->GetAddr(address, &target) != AMX_ERR_NONE) {
		return nullptr;
	}
	return target;
}

bool readString(AMX* amx, cell address, std::string& output) {
	output.clear();
	cell* text = addressOf(amx, address);
	if (text == nullptr) {
		return false;
	}
	size_t limit = room(amx, address);
	if (limit == 0) {
		return true;
	}
	if (ucell(text[0]) > UNPACKEDMAX) {
		for (size_t index = 0; index != limit; ++index) {
			ucell value = ucell(text[index]);
			for (int shift = 24; shift >= 0; shift -= 8) {
				char character = char((value >> shift) & 0xFF);
				if (character == '\0') {
					return true;
				}
				output.push_back(character);
			}
		}
		return true;
	}
	size_t length = 0;
	while (length != limit && text[length] != 0) {
		++length;
	}
	output.resize(length);
	for (size_t index = 0; index != length; ++index) {
		output[index] = char(text[index]);
	}
	return true;
}

int writeString(AMX* amx, cell address, std::string_view text, cell size) {
	if (size <= 0) {
		return -1;
	}
	cell* target = addressOf(amx, address);
	if (target == nullptr) {
		return -1;
	}
	size_t limit = std::min(size_t(size), room(amx, address));
	if (limit == 0) {
		return -1;
	}
	size_t count = std::min(text.size(), limit - 1);
	for (size_t index = 0; index != count; ++index) {
		target[index] = cell((unsigned char)text[index]);
	}
	target[count] = 0;
	return int(count);
}

void appendEscaped(std::string& output, std::string_view text, bool backslash) {
	output.reserve(output.size() + text.size() + 8);
	for (char character : text) {
		if (!backslash) {
			if (character == '\'') {
				output.push_back('\'');
			}
			output.push_back(character);
			continue;
		}
		switch (character) {
			case '\0':
				output.append("\\0");
				break;
			case '\n':
				output.append("\\n");
				break;
			case '\r':
				output.append("\\r");
				break;
			case '\\':
				output.append("\\\\");
				break;
			case '\'':
				output.append("\\'");
				break;
			case '"':
				output.append("\\\"");
				break;
			case '\032':
				output.append("\\Z");
				break;
			default:
				output.push_back(character);
				break;
		}
	}
}

void appendJson(std::string& output, std::string_view text) {
	static const char hex[] = "0123456789abcdef";
	output.reserve(output.size() + text.size() + 2);
	output.push_back('"');
	for (char character : text) {
		switch (character) {
			case '"':
				output.append("\\\"");
				break;
			case '\\':
				output.append("\\\\");
				break;
			case '\n':
				output.append("\\n");
				break;
			case '\r':
				output.append("\\r");
				break;
			case '\t':
				output.append("\\t");
				break;
			case '\b':
				output.append("\\b");
				break;
			case '\f':
				output.append("\\f");
				break;
			default:
				if ((unsigned char)character < 0x20) {
					output.append("\\u00");
					output.push_back(hex[(unsigned char)character >> 4]);
					output.push_back(hex[(unsigned char)character & 15]);
				} else {
					output.push_back(character);
				}
				break;
		}
	}
	output.push_back('"');
}

bool parseStatement(std::string_view pattern, std::string& sql, std::string& types, std::string& error) {
	sql.clear();
	types.clear();
	sql.reserve(pattern.size());
	for (size_t at = 0; at < pattern.size(); ++at) {
		char character = pattern[at];
		if (character != '%') {
			sql.push_back(character);
			continue;
		}
		if (++at == pattern.size()) {
			error = "the statement ends with a lone %";
			return false;
		}
		if (pattern[at] == '.') {
			while (++at < pattern.size() && std::isdigit((unsigned char)pattern[at])) {
			}
			if (at == pattern.size()) {
				error = "the statement ends inside a specifier";
				return false;
			}
		}
		switch (pattern[at]) {
			case '%':
				sql.push_back('%');
				continue;
			case 'i':
			case 'd':
			case 'x':
				types.push_back('i');
				break;
			case 'u':
				types.push_back('u');
				break;
			case 'f':
				types.push_back('f');
				break;
			case 's':
			case 'e':
			case 'q':
			case 'j':
				types.push_back('s');
				break;
			case 'Q':
				types.push_back('Q');
				break;
			default:
				error = std::string("unknown specifier %") + pattern[at];
				return false;
		}
		sql.push_back('?');
	}
	return true;
}

bool formatQuery(AMX* amx, const cell* params, int format, int first, bool backslash, std::string& output, std::string& error) {
	static std::string pattern;
	static std::string text;
	if (!readString(amx, params[format], pattern)) {
		error = "the query is not a string";
		return false;
	}
	int count = int(params[0] / sizeof(cell));
	int argument = first;
	output.reserve(output.size() + pattern.size() + 32);
	for (size_t at = 0; at < pattern.size(); ++at) {
		char character = pattern[at];
		if (character != '%') {
			output.push_back(character);
			continue;
		}
		if (++at == pattern.size()) {
			error = "the query ends with a lone %";
			return false;
		}
		int precision = -1;
		if (pattern[at] == '.') {
			precision = 0;
			while (++at < pattern.size() && std::isdigit((unsigned char)pattern[at])) {
				precision = std::min(precision * 10 + (pattern[at] - '0'), 30);
			}
			if (at == pattern.size()) {
				error = "the query ends inside a specifier";
				return false;
			}
		}
		char specifier = pattern[at];
		if (specifier == '%') {
			output.push_back('%');
			continue;
		}
		if (argument > count) {
			error = "the query has more specifiers than values";
			return false;
		}
		cell address = params[argument++];
		switch (specifier) {
			case 'i':
			case 'd':
			case 'u':
			case 'x':
			case 'f': {
				cell* value = addressOf(amx, address);
				if (value == nullptr) {
					error = "a value of the query cannot be read";
					return false;
				}
				if (specifier == 'u') {
					appendInteger(output, (long long)ucell(*value));
				} else if (specifier == 'x') {
					appendHex(output, ucell(*value));
				} else if (specifier == 'f') {
					appendFloat(output, amx_ctof(*value), precision);
				} else {
					appendInteger(output, (long long)*value);
				}
				break;
			}
			case 'Q': {
				cell* value = addressOf(amx, address);
				Script* script = scriptOf(amx);
				if (script == nullptr || value == nullptr) {
					error = "%Q is not given a query being built";
					return false;
				}
				auto found = script->builders.find(int(*value));
				if (found == script->builders.end()) {
					error = "%Q is not given a query being built";
					return false;
				}
				output.append(found->second.text);
				break;
			}
			case 's':
			case 'e':
			case 'j':
			case 'q': {
				if (!readString(amx, address, text)) {
					error = "a string of the query cannot be read";
					return false;
				}
				if (specifier == 's') {
					output.append(text);
				} else if (specifier == 'e') {
					appendEscaped(output, text, backslash);
				} else if (specifier == 'j') {
					appendJson(output, text);
				} else {
					output.push_back('\'');
					appendEscaped(output, text, backslash);
					output.push_back('\'');
				}
				break;
			}
			default:
				error = std::string("unknown specifier %") + specifier;
				return false;
		}
	}
	return true;
}

bool readOptions(std::string_view options, Settings& settings, std::string& error) {
	while (!options.empty()) {
		size_t end = options.find_first_of(";\n");
		std::string_view entry = options.substr(0, end);
		options = end == std::string_view::npos ? std::string_view() : options.substr(end + 1);
		if (trim(entry).empty()) {
			continue;
		}
		size_t equals = entry.find('=');
		if (equals == std::string_view::npos) {
			error = "a setting has no value: " + std::string(trim(entry));
			return false;
		}
		if (!setting(settings, entry.substr(0, equals), entry.substr(equals + 1), error)) {
			return false;
		}
	}
	if (!safeCharset(settings.charset)) {
		error = "the character set " + settings.charset + " cannot be escaped safely";
		return false;
	}
	return true;
}

bool readSettingsFile(const std::string& path, Settings& settings, std::string& error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		error = "cannot open " + path;
		return false;
	}
	std::string line;
	std::string value;
	while (std::getline(file, line)) {
		std::string_view entry = trim(line);
		if (entry.empty() || entry.front() == '#' || entry.front() == ';' || entry.front() == '[') {
			continue;
		}
		size_t equals = entry.find('=');
		if (equals == std::string_view::npos) {
			error = "a line of " + path + " has no value: " + std::string(entry);
			return false;
		}
		if (!expand(entry.substr(equals + 1), value, error)) {
			error += " (" + path + ")";
			return false;
		}
		if (!setting(settings, entry.substr(0, equals), value, error)) {
			return false;
		}
	}
	if (!safeCharset(settings.charset)) {
		error = "the character set " + settings.charset + " cannot be escaped safely";
		return false;
	}
	return true;
}

}
