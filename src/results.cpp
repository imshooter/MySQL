#include "mysql.hpp"

#include <yyjson.h>

#include <algorithm>
#include <cctype>

namespace mysqlcomponent {

namespace {

constexpr uint16_t Cp1252High[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

bool ascii(std::string_view text) {
	for (char character : text) {
		if ((unsigned char)character >= 0x80) {
			return false;
		}
	}
	return true;
}

void appendUtf8(std::string& output, uint32_t point) {
	if (point < 0x80) {
		output.push_back(char(point));
	} else if (point < 0x800) {
		output.push_back(char(0xC0 | (point >> 6)));
		output.push_back(char(0x80 | (point & 0x3F)));
	} else {
		output.push_back(char(0xE0 | (point >> 12)));
		output.push_back(char(0x80 | ((point >> 6) & 0x3F)));
		output.push_back(char(0x80 | (point & 0x3F)));
	}
}

char cp1252Of(uint32_t point) {
	if (point < 0x80 || (point >= 0xA0 && point <= 0xFF)) {
		return char(point);
	}
	for (int index = 0; index != 32; ++index) {
		if (Cp1252High[index] == point) {
			return char(0x80 + index);
		}
	}
	return '?';
}

}

std::string_view latin1ToUtf8(std::string_view text, std::string& buffer) {
	if (ascii(text)) {
		return text;
	}
	buffer.clear();
	buffer.reserve(text.size() + text.size() / 2);
	for (char character : text) {
		unsigned char byte = (unsigned char)character;
		appendUtf8(buffer, byte >= 0x80 && byte < 0xA0 ? Cp1252High[byte - 0x80] : byte);
	}
	return buffer;
}

std::string_view utf8ToLatin1(std::string_view text, std::string& buffer) {
	if (ascii(text)) {
		return text;
	}
	buffer.clear();
	buffer.reserve(text.size());
	size_t at = 0;
	while (at < text.size()) {
		unsigned char lead = (unsigned char)text[at];
		uint32_t point = lead;
		size_t extra = 0;
		if (lead >= 0x80) {
			if ((lead & 0xE0) == 0xC0) {
				point = lead & 0x1F;
				extra = 1;
			} else if ((lead & 0xF0) == 0xE0) {
				point = lead & 0x0F;
				extra = 2;
			} else if ((lead & 0xF8) == 0xF0) {
				point = lead & 0x07;
				extra = 3;
			} else {
				buffer.push_back('?');
				++at;
				continue;
			}
		}
		size_t end = at + 1 + extra;
		bool valid = end <= text.size();
		for (size_t next = at + 1; valid && next != end; ++next) {
			unsigned char continuation = (unsigned char)text[next];
			valid = (continuation & 0xC0) == 0x80;
			point = (point << 6) | (continuation & 0x3F);
		}
		if (!valid) {
			buffer.push_back('?');
			++at;
			continue;
		}
		buffer.push_back(cp1252Of(point));
		at = end;
	}
	return buffer;
}

JsonCell::~JsonCell() {
	if (doc != nullptr) {
		yyjson_doc_free(doc);
	}
}

int ResultSet::find(std::string_view name) const {
	std::string key(name);
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	auto found = names.find(key);
	return found == names.end() ? -1 : int(found->second);
}

bool ResultSet::cell(uint32_t row, uint32_t column, const char*& text, uint32_t& length) const {
	if (row >= rows || column >= columns.size()) {
		return false;
	}
	size_t index = size_t(row) * columns.size() + column;
	length = lengths[index];
	text = length == NullLength ? nullptr : data.data() + offsets[index];
	return true;
}

JsonCell* jsonOf(ResultSet& set, uint32_t row, uint32_t column) {
	const char* text = nullptr;
	uint32_t length = 0;
	if (!set.cell(row, column, text, length) || text == nullptr) {
		return nullptr;
	}
	if (set.json.empty()) {
		set.json.resize(size_t(set.rows) * set.columns.size());
	}
	std::unique_ptr<JsonCell>& slot = set.json[size_t(row) * set.columns.size() + column];
	if (!slot) {
		static std::string buffer;
		std::string_view source(text, length);
		if (set.latin1) {
			source = latin1ToUtf8(source, buffer);
		}
		slot = std::make_unique<JsonCell>();
		slot->doc = yyjson_read(source.data(), source.size(), YYJSON_READ_ALLOW_INVALID_UNICODE);
	}
	return slot->doc != nullptr ? slot.get() : nullptr;
}

}
