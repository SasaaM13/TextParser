#include "JSONParser.h"
#include <algorithm>

bool JSONValue::isString() const { return owner->nodes()[idx].type == (uint32_t)JSONType::String; }
bool JSONValue::isNumber() const { return owner->nodes()[idx].type == (uint32_t)JSONType::Number; }
bool JSONValue::isBool()   const { return owner->nodes()[idx].type == (uint32_t)JSONType::Bool; }
bool JSONValue::isArray()  const { return owner->nodes()[idx].type == (uint32_t)JSONType::Array; }
bool JSONValue::isObject() const { return owner->nodes()[idx].type == (uint32_t)JSONType::Object; }
bool JSONValue::isNull()   const { return owner->nodes()[idx].type == (uint32_t)JSONType::Null; }

double JSONValue::asDouble() const
{
	auto& n = const_cast<JSONNode&>(owner->nodes()[idx]);
	if(n.type != (uint32_t)JSONType::Number)
		return 0.0;
	if(std::isnan(n.num))
	{
		double val;
		auto res = std::from_chars(n.raw.data(), n.raw.data() + n.raw.size(), val);
		if(res.ec == std::errc())
			n.num = val;
	}
	return n.num;
}

bool JSONValue::asBool() const
{
	return owner->nodes()[idx].boolVal;
}

std::string_view JSONValue::asStringView() const
{
	return owner->nodes()[idx].str;
}

size_t JSONValue::size() const
{
	const auto& n = owner->nodes()[idx];
	if(n.type == (uint32_t)JSONType::Array)
		return n.b;
	if(n.type == (uint32_t)JSONType::Object)
		return n.b;
	return 0;
}

JSONValue JSONValue::operator[](size_t i) const
{
	const auto& n = owner->nodes()[idx];
	if(n.type != (uint32_t)JSONType::Array || i >= n.b)
		return {};
	return JSONValue(owner, owner->childrenArena()[n.a + (uint32_t)i]);
}

JSONValue JSONValue::operator[](std::string_view key) const
{
	if(!valid())
		return {};
	const auto& n = owner->nodes()[idx];
	if(n.type != (uint32_t)JSONType::Object)
		return {};
	for(uint32_t k = 0; k < n.b; ++k)
	{
		const auto& kv = owner->membersArena()[n.a + k];
		if(kv.first.size() == key.size() && std::memcmp(kv.first.data(), key.data(), key.size()) == 0)
			return JSONValue(owner, kv.second);
	}
	return {};
}
size_t JSONParser::skip_ws(std::string_view sv, size_t i)
{
	while(i < sv.size() && is_ws((unsigned char)sv[i]))
		++i;
	return i;
}

size_t JSONParser::skip_bom_and_ws(std::string_view sv)
{
	size_t i = 0;
	if(sv.size() >= 3 && (unsigned char)sv[0] == 0xEF && (unsigned char)sv[1] == 0xBB && (unsigned char)sv[2] == 0xBF)
		i = 3;
	return skip_ws(sv, i);
}

bool JSONParser::expect_char(std::string_view sv, size_t& i, char ch)
{
	i = skip_ws(sv, i);
	if(i >= sv.size() || sv[i] != ch)
		return false;
	++i;
	return true;
}

bool JSONParser::parse_json_string_view(std::string_view sv, size_t& i, std::string_view& out)
{
	i = skip_ws(sv, i);
	if(i >= sv.size() || sv[i] != '"')
		return false;
	size_t start = ++i;
	bool hasEscape = false;

	while(i < sv.size())
	{
		char c = sv[i];
		if(c == '\\')
		{
			hasEscape = true;
			++i;
			if(i < sv.size())
				++i;
			continue;
		}
		if(c == '"')
			break;
		++i;
	}
	if(i >= sv.size() || sv[i] != '"')
		return false;

	size_t end = i;
	++i;

	if(!hasEscape)
	{
		out = sv.substr(start, end - start);
		return true;
	}
	out = sv.substr(start, end - start);
	return true;
}

bool JSONParser::parse_json_value_view(std::string_view sv, size_t& i, std::string_view& out)
{
	i = skip_ws(sv, i);
	if(i >= sv.size())
		return false;

	if(sv[i] == '"')
		return parse_json_string_view(sv, i, out);

	if(sv[i] == '{')
	{
		size_t start = i;
		int objDepth = 1;
		int arrDepth = 0;
		++i;

		while(i < sv.size() && (objDepth > 0 || arrDepth > 0))
		{
			char c = sv[i];
			if(c == '"')
			{
				++i;
				while(i < sv.size() && sv[i] != '"')
				{
					if(sv[i] == '\\') ++i;
					++i;
				}
			}
			else if(c == '{')
				objDepth++;
			else if(c == '}')
				objDepth--;
			else if(c == '[')
				arrDepth++;
			else if(c == ']')
				arrDepth--;
			++i;
		}
		out = sv.substr(start, i - start);
		return true;
	}
	if(sv[i] == '[')
	{
		size_t start = i;
		int depth = 1;
		++i;
		while(i < sv.size() && depth > 0)
		{
			if(sv[i] == '[')
				depth++;
			else if(sv[i] == ']')
				depth--;
			else if(sv[i] == '"')
			{
				++i;
				while(i < sv.size() && sv[i] != '"')
				{
					if(sv[i] == '\\')
						++i;
					++i;
				}
			}
			++i;
		}

		out = sv.substr(start, i - start);
		return true;
	}

	size_t start = i;
	while(i < sv.size())
	{
		char c = sv[i];
		if(is_ws((unsigned char)c) || c == ',' || c == '}' || c == ']')
			break;
		++i;
	}

	out = sv.substr(start, i - start);
	return true;
}
JSONParser::Mode JSONParser::detectMode(std::string_view sv) const
{
	size_t i = skip_bom_and_ws(sv);
	if(i >= sv.size()) 
		return Mode::Unknown;
	char c = sv[i];
	if(c == '[')
		return Mode::FlatArray;
	if(c == '{')
	{
		size_t j = i;
		int depth = 0;
		bool inStr = false;
		for(; j < sv.size(); ++j)
		{
			char x = sv[j];
			if(inStr)
			{
				if(x == '\\')
				{
					++j;
					continue;
				}
				if(x == '"')
					inStr = false;
				continue;
			}
			if(x == '"')
			{
				inStr = true;
				continue;
			}
			if(x == '{')
				++depth;
			else if(x == '}') 
			{
				--depth; if(depth == 0)
				{
					++j;
					break;
				}
			}
		}
		j = skip_ws(sv, j);
		if(j < sv.size() && sv[j] == '{')
			return Mode::NDJSON;
		return Mode::SingleObject;
	}
	return Mode::Unknown;
}

bool JSONParser::mapFile()
{
	namespace fs = std::filesystem;
	if(!fs::exists(filename_))
		return false;
	size_ = (size_t)fs::file_size(filename_);
	if(size_ == 0)
		return true;

	hFile_ = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(hFile_ == INVALID_HANDLE_VALUE)
		return false;

	hMap_ = CreateFileMappingA(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
	if(!hMap_)
		return false;

	base_ = (const char*)MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0);
	if(!base_)
		return false;
	return true;
}

void JSONParser::unmapFile()
{
	if(base_)
	{
		UnmapViewOfFile(base_);
		base_ = nullptr;
	}
	if(hMap_)
	{
		CloseHandle(hMap_);
		hMap_ = nullptr;
	}
	if(hFile_ != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile_);
		hFile_ = INVALID_HANDLE_VALUE;
	}
}

size_t JSONParser::parseValueDOM(std::string_view sv, size_t& i)
{
	i = skip_ws(sv, i);
	if(i >= sv.size())
		return SIZE_MAX;

	char c = sv[i];
	switch(c)
	{
	case '{':
		return parseObjectDOM(sv, i);
	case '[':
		return parseArrayDOM(sv, i);
	case '"':
		return parseStringDOM(sv, i);
	default:
		if((c >= '0' && c <= '9') || c == '-')
			return parseNumberDOM(sv, i);
		return parseLiteralDOM(sv, i);
	}
}

size_t JSONParser::parseStringDOM(std::string_view sv, size_t& i)
{
	std::string_view out;
	if(!parse_json_string_view(sv, i, out))
		return SIZE_MAX;

	JSONNode node{};
	node.type = static_cast<uint32_t>(JSONType::String);
	node.str = out;

	nodes_.push_back(node);
	return nodes_.size() - 1;
}

size_t JSONParser::parseNumberDOM(std::string_view sv, size_t& i)
{
	size_t start = i;
	while(i < sv.size())
	{
		char c = sv[i];
		if((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')
			++i;
		else break;
	}

	JSONNode node{};
	node.type = static_cast<uint32_t>(JSONType::Number);
	node.raw = std::string_view(sv.data() + start, i - start);
	node.num = std::numeric_limits<double>::quiet_NaN();

	nodes_.push_back(node);
	return nodes_.size() - 1;
}

size_t JSONParser::parseLiteralDOM(std::string_view sv, size_t& i)
{
	if(sv.substr(i, 4) == "true")
	{
		i += 4;
		JSONNode n{};
		n.type = static_cast<uint32_t>(JSONType::Bool);
		n.boolVal = true;
		nodes_.push_back(n);
		return nodes_.size() - 1;
	}

	if(sv.substr(i, 5) == "false")
	{
		i += 5;
		JSONNode n{};
		n.type = static_cast<uint32_t>(JSONType::Bool);
		n.boolVal = false;
		nodes_.push_back(n);
		return nodes_.size() - 1;
	}

	if(sv.substr(i, 4) == "null")
	{
		i += 4;
		JSONNode n{};
		n.type = static_cast<uint32_t>(JSONType::Null);
		nodes_.push_back(n);
		return nodes_.size() - 1;
	}

	return SIZE_MAX;
}

size_t JSONParser::parseArrayDOM(std::string_view sv, size_t& i)
{
	JSONNode node{};
	node.type = static_cast<uint32_t>(JSONType::Array);
	++i;

	std::vector<uint32_t> localChildren;
	localChildren.reserve(8);

	while(true)
	{
		i = skip_ws(sv, i);
		if(i >= sv.size())
			return SIZE_MAX;

		if(sv[i] == ']')
		{
			++i;
			break;
		}

		size_t child = parseValueDOM(sv, i);
		if(child == SIZE_MAX)
			return SIZE_MAX;

		localChildren.push_back((uint32_t)child);

		i = skip_ws(sv, i);
		if(i < sv.size() && sv[i] == ',')
		{
			++i;
			continue;
		}

		if(i < sv.size() && sv[i] == ']')
		{
			++i;
			break;
		}

		return SIZE_MAX;
	}
	node.a = (uint32_t)arenaChildren.size();
	node.b = (uint32_t)localChildren.size();

	arenaChildren.insert(arenaChildren.end(), localChildren.begin(), localChildren.end());

	nodes_.push_back(node);
	return nodes_.size() - 1;
}

size_t JSONParser::parseObjectDOM(std::string_view sv, size_t& i)
{
	JSONNode node{};
	node.type = static_cast<uint32_t>(JSONType::Object);
	++i;
	std::vector<std::pair<std::string_view, uint32_t>> localMembers;
	localMembers.reserve(8);
	while(true)
	{
		i = skip_ws(sv, i);
		if(i >= sv.size())
			return SIZE_MAX;
		if(sv[i] == '}')
		{
			++i;
			break;
		}

		std::string_view key;
		if(!parse_json_string_view(sv, i, key))
			return SIZE_MAX;
		if(!expect_char(sv, i, ':'))
			return SIZE_MAX;

		size_t val = parseValueDOM(sv, i);
		if(val == SIZE_MAX)
			return SIZE_MAX;

		localMembers.emplace_back(key, (uint32_t)val);
		i = skip_ws(sv, i);
		if(i < sv.size() && sv[i] == ',')
		{
			++i;
			continue;
		}
		if(i < sv.size() && sv[i] == '}')
		{
			++i;
			break;
		}
		return SIZE_MAX;
	}
	node.a = (uint32_t)arenaMembers.size();
	node.b = (uint32_t)localMembers.size();
	arenaMembers.insert(arenaMembers.end(), localMembers.begin(), localMembers.end());
	nodes_.push_back(node);
	return nodes_.size() - 1;
}

bool JSONParser::loadDOM()
{
	if(!mapFile())
		return false;

	nodes_.clear();
	nodes_.reserve(size_ / 16);
	std::string_view sv(base_, size_);
	size_t i = skip_bom_and_ws(sv);

	rootIndex_ = parseValueDOM(sv, i);
	return rootIndex_ != SIZE_MAX;
}

bool JSONParser::load()
{
	rows_ = cols_ = 0;
	totalFields_ = 0;
	dataViews_.clear();
	colNames_.clear();
	root_ = DataNode("root", "", 0);
	nodes_.clear();
	arenaChildren.clear();
	arenaMembers.clear();
	cacheString_.clear();
	cacheInt_.clear();
	cacheDouble_.clear();
	cacheBool_.clear();
	rootIndex_ = SIZE_MAX;
	namespace fs = std::filesystem;
	if(!fs::exists(filename_))
		return false;

	unmapFile();
	if(!mapFile())
		return false;

	std::string_view sv(base_, size_);
	size_t i = skip_bom_and_ws(sv);
	rootIndex_ = parseValueDOM(sv, i);
	if(rootIndex_ == SIZE_MAX)
	{
		unmapFile();
		return false;
	}

	notifyLoaded();
	return true;
}
bool JSONParser::parseFast()
{
	if(size_ == 0)
		return true;
	std::string_view sv(base_, size_);
	Mode m = detectMode(sv);
	switch(m)
	{
	case Mode::FlatArray:
		return parseFlatArray(sv);
	case Mode::NDJSON:
		return parseNDJSON(sv);
	case Mode::SingleObject:
		return parseSingleObjectToRoot(sv);
	default:
		return false;
	}
}

bool JSONParser::find_object_spans_in_array(std::string_view sv, size_t arrayBegin, size_t arrayEnd, std::vector<ObjSpan>& out)
{
	out.clear();
	size_t i = arrayBegin;
	i = skip_ws(sv, i);
	if(i >= arrayEnd || sv[i] != '[')
		return false;
	++i;

	int depth = 0;
	bool inStr = false;
	size_t objStart = 0;
	bool inObj = false;

	for(; i < arrayEnd; ++i)
	{
		char c = sv[i];
		if(inStr) {
			if(c == '\\')
			{
				++i;
				continue;
			}
			if(c == '"')
				inStr = false;
			continue;
		}
		else
		{
			if(c == '"')
			{
				inStr = true;
				continue;
			}
			if(c == '{')
			{
				if(!inObj)
				{
					inObj = true;
					objStart = i;
				}
				++depth;
			}
			else if(c == '}')
			{
				--depth;
				if(inObj && depth == 0)
				{
					out.push_back({ objStart, i + 1 });
					inObj = false;
				}
			}
			else if(c == ']')
				break;
		}
	}
	return !out.empty();
}

bool JSONParser::parseObjectRow(std::string_view obj, std::vector<std::string_view>& keysTmp, std::vector<std::string_view>& valsTmp, std::string_view* rowPtr)
{
	keysTmp.clear();
	valsTmp.clear();

	size_t i = 0;
	if(!expect_char(obj, i, '{'))
		return false;

	for(size_t c = 0; c < cols_; ++c)
		rowPtr[c] = {};

	while(true)
	{
		i = skip_ws(obj, i);
		if(i >= obj.size())
			return false;

		if(obj[i] == '}')
		{
			++i;
			break;
		}

		std::string_view key;
		if(!parse_json_string_view(obj, i, key))
			return false;

		if(!expect_char(obj, i, ':'))
			return false;

		std::string_view val;
		if(!parse_json_value_view(obj, i, val))
			return false;

		keysTmp.push_back(key);
		valsTmp.push_back(val);

		auto it = colIndex_.find(key);
		if(it != colIndex_.end())
			rowPtr[it->second] = val;
		i = skip_ws(obj, i);
		if(i < obj.size() && obj[i] == ',')
		{
			++i;
			continue;
		}
		if(i < obj.size() && obj[i] == '}')
		{
			++i;
			break;
		}
	}

	return true;
}

void JSONParser::buildColumnsFromFirstObject(const std::vector<std::string_view>& keysTmp)
{
	colNames_.clear();
	colIndex_.clear();
	colNames_.reserve(keysTmp.size());
	colIndex_.reserve(keysTmp.size());
	for(size_t i = 0; i < keysTmp.size(); ++i)
	{
		colNames_.emplace_back(keysTmp[i]);
		colIndex_[std::string_view(colNames_.back())] = i;
	}

	cols_ = colNames_.size();
}

bool JSONParser::parseFlatArray(std::string_view sv)
{
	size_t i0 = skip_bom_and_ws(sv);
	if(i0 >= sv.size() || sv[i0] != '[')
		return false;

	size_t iEnd = sv.size();
	std::vector<std::string_view> keysTmp;
	std::vector<std::string_view> valsTmp;
	std::vector<ObjSpan> spans;
	keysTmp.reserve(64);
	valsTmp.reserve(64);

	if(!find_object_spans_in_array(sv, i0, iEnd, spans))
		return false;
	if(spans.empty())
		return false;

	{
		std::string_view obj = sv.substr(spans[0].begin, spans[0].end - spans[0].begin);
		size_t j = 0;
		if(!expect_char(obj, j, '{'))
			return false;
		while(true)
		{
			j = skip_ws(obj, j);
			if(j >= obj.size())
				return false;
			if(obj[j] == '}')
			{
				++j;
				break;
			}

			std::string_view key;
			if(!parse_json_string_view(obj, j, key))
				return false;
			if(!expect_char(obj, j, ':'))
				return false;
			std::string_view val;
			if(!parse_json_value_view(obj, j, val))
				return false;

			keysTmp.push_back(key);
			valsTmp.push_back(val);

			j = skip_ws(obj, j);
			if(j < obj.size() && obj[j] == ',')
			{
				++j;
				continue;
			}
			if(j < obj.size() && obj[j] == '}')
			{
				++j;
				break;
			}
		}

		if(keysTmp.size() > opt_.maxColumnsHint)
			return false;
		buildColumnsFromFirstObject(keysTmp);
	}

	rows_ = spans.size();
	dataViews_.assign(rows_ * cols_, std::string_view{});

	unsigned hw = opt_.threadHint ? opt_.threadHint : std::max<unsigned int>(1u, std::thread::hardware_concurrency());
	bool usePar = opt_.parallel && rows_ >= 200 && hw >= 2;

	if(!usePar)
	{
		size_t totalFields = 0;
		for(size_t r = 0; r < rows_; ++r)
		{
			std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);
			auto* rowPtr = &dataViews_[r * cols_];
			if(!parseObjectRow(obj, keysTmp, valsTmp, rowPtr))
				return false;

			totalFields += keysTmp.size();
		}

		totalFields_ = totalFields;
		return true;
	}

	size_t workers = std::min<size_t>(hw, 8);
	size_t chunk = (rows_ + workers - 1) / workers;

	std::vector<std::future<size_t>> fut;
	fut.reserve(workers);

	for(size_t w = 0; w < workers; ++w)
	{
		size_t r0 = w * chunk;
		size_t r1 = std::min<size_t>(rows_, r0 + chunk);
		if(r0 >= r1)
			break;
		fut.push_back(std::async(std::launch::async, [&, r0, r1]() -> size_t
			{
				std::vector<std::string_view> keysTmpLocal;
				std::vector<std::string_view> valsTmpLocal;
				keysTmpLocal.reserve(32);
				valsTmpLocal.reserve(32);
				size_t localFields = 0;
				for(size_t r = r0; r < r1; ++r)
				{
					auto* rowPtr = &dataViews_[r * cols_];
					std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);

					if(!parseObjectRow(obj, keysTmpLocal, valsTmpLocal, rowPtr))
						return size_t(-1);
					localFields += keysTmpLocal.size();
				}

				return localFields;
			}));
	}

	totalFields_ = 0;
	for(auto& f : fut)
	{
		size_t v = f.get();
		if(v == size_t(-1))
			return false;
		totalFields_ += v;
	}

	return true;
}

bool JSONParser::parseNDJSON(std::string_view sv)
{
	size_t i = skip_bom_and_ws(sv);
	std::vector<ObjSpan> spans;
	spans.reserve(1024);
	size_t lineStart = i;
	while(lineStart < sv.size())
	{
		size_t lineEnd = lineStart;
		while(lineEnd < sv.size() && sv[lineEnd] != '\n')
			++lineEnd;

		size_t trimEnd = lineEnd;
		if(trimEnd > lineStart && sv[trimEnd - 1] == '\r')
			--trimEnd;

		size_t j = skip_ws(sv, lineStart);
		if(j < trimEnd && sv[j] == '{')
			spans.push_back({ lineStart, trimEnd });

		lineStart = (lineEnd < sv.size()) ? (lineEnd + 1) : sv.size();
	}
	if(spans.empty())
		return false;

	{
		std::vector<std::string_view> keysTmp, valsTmp;
		std::string_view obj = sv.substr(spans[0].begin, spans[0].end - spans[0].begin);

		size_t j = 0;
		if(!expect_char(obj, j, '{'))
			return false;
		while(true)
		{
			j = skip_ws(obj, j);
			if(j >= obj.size())
				return false;
			if(obj[j] == '}')
			{
				++j;
				break;
			}

			std::string_view key, val;
			if(!parse_json_string_view(obj, j, key))
				return false;
			if(!expect_char(obj, j, ':'))
				return false;
			if(!parse_json_value_view(obj, j, val))
				return false;

			keysTmp.push_back(key);
			valsTmp.push_back(val);

			j = skip_ws(obj, j);
			if(j < obj.size() && obj[j] == ',')
			{
				++j;
				continue;
			}
			if(j < obj.size() && obj[j] == '}')
			{
				++j;
				break;
			}
		}

		if(keysTmp.size() > opt_.maxColumnsHint)
			return false;
		buildColumnsFromFirstObject(keysTmp);
	}

	rows_ = spans.size();
	dataViews_.assign(rows_ * cols_, std::string_view{});

	unsigned hw = opt_.threadHint ? opt_.threadHint : std::max<unsigned int>(1u, std::thread::hardware_concurrency());
	bool usePar = opt_.parallel && rows_ >= 500 && hw >= 2;

	if(!usePar)
	{
		size_t totalFields = 0;
		std::vector<std::string_view> keysTmp, valsTmp;
		keysTmp.reserve(32);
		valsTmp.reserve(32);

		for(size_t r = 0; r < rows_; ++r)
		{
			auto* rowPtr = &dataViews_[r * cols_];
			std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);

			if(!parseObjectRow(obj, keysTmp, valsTmp, rowPtr))
				return false;

			totalFields += keysTmp.size();
		}

		totalFields_ = totalFields;
		return true;
	}

	size_t workers = std::min<size_t>(hw, 8);
	size_t chunk = (rows_ + workers - 1) / workers;

	std::vector<std::future<size_t>> fut;
	fut.reserve(workers);

	for(size_t w = 0; w < workers; ++w)
	{
		size_t r0 = w * chunk;
		size_t r1 = std::min<size_t>(rows_, r0 + chunk);
		if(r0 >= r1)
			break;

		fut.push_back(std::async(std::launch::async, [&, r0, r1]() -> size_t
			{
				std::vector<std::string_view> keysTmp, valsTmp;
				keysTmp.reserve(32);
				valsTmp.reserve(32);

				size_t localFields = 0;

				for(size_t r = r0; r < r1; ++r)
				{
					auto* rowPtr = &dataViews_[r * cols_];
					std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);

					if(!parseObjectRow(obj, keysTmp, valsTmp, rowPtr))
						return size_t(-1);

					localFields += keysTmp.size();
				}

				return localFields;
			}));
	}

	totalFields_ = 0;
	for(auto& f : fut)
	{
		size_t v = f.get();
		if(v == size_t(-1))
			return false;
		totalFields_ += v;
	}

	return true;
}
bool JSONParser::parseSingleObjectToRoot(std::string_view sv)
{
	size_t i = skip_bom_and_ws(sv);
	if(i >= sv.size() || sv[i] != '{')
		return false;
	std::vector<std::string_view> keysTmp, valsTmp;
	size_t j = 0;
	std::string_view obj = sv.substr(i);
	if(!expect_char(obj, j, '{'))
		return false;

	while(true)
	{
		j = skip_ws(obj, j);
		if(j >= obj.size())
			return false;
		if(obj[j] == '}')
		{
			++j;
			break;
		}

		std::string_view key;
		if(!parse_json_string_view(obj, j, key))
			return false;
		if(!expect_char(obj, j, ':'))
			return false;

		std::string_view val;
		if(!parse_json_value_view(obj, j, val))
			return false;

		keysTmp.push_back(key);
		valsTmp.push_back(val);

		j = skip_ws(obj, j);
		if(j < obj.size() && obj[j] == ',')
		{
			++j;
			continue;
		}
		if(j < obj.size() && obj[j] == '}')
		{
			++j;
			break;
		}
	}

	buildColumnsFromFirstObject(keysTmp);
	rows_ = 1;
	dataViews_.assign(cols_, std::string_view{});
	for(size_t k = 0; k < keysTmp.size(); ++k)
	{
		auto it = colIndex_.find(keysTmp[k]);
		if(it != colIndex_.end())
			dataViews_[it->second] = valsTmp[k];
	}

	totalFields_ = keysTmp.size();
	root_ = DataNode("root", "", 0);
	return true;
}

bool JSONParser::parseSlowFallback()
{
	return false;
}

const std::string& JSONParser::materialize(size_t r, size_t c) const
{
	const auto kOpt = keyChecked(r, c);
	if(!kOpt)
		return empty_;

	const size_t k = *kOpt;
	std::lock_guard<std::mutex> lk(cacheMutex_);

	if(k >= cacheString_.size())
		return empty_;

	if(cacheString_[k].has_value())
		return *cacheString_[k];

	auto v = valueView(r, c);
	cacheString_[k] = std::string(v);
	return *cacheString_[k];
}

const std::string& JSONParser::value(size_t r, size_t c) const
{
	if(r >= rows_ || c >= cols_)
		return empty_;
	return materialize(r, c);
}
