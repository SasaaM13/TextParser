#include "JSONParser.h"
#include <algorithm>

size_t JSONParser::skip_ws(std::string_view sv, size_t i) {
    while(i < sv.size() && is_ws((unsigned char)sv[i])) ++i;
    return i;
}

size_t JSONParser::skip_bom_and_ws(std::string_view sv) {
    size_t i = 0;
    // UTF-8 BOM
    if(sv.size() >= 3 &&
        (unsigned char)sv[0] == 0xEF &&
        (unsigned char)sv[1] == 0xBB &&
        (unsigned char)sv[2] == 0xBF) {
        i = 3;
    }
    return skip_ws(sv, i);
}

bool JSONParser::expect_char(std::string_view sv, size_t& i, char ch) {
    i = skip_ws(sv, i);
    if(i >= sv.size() || sv[i] != ch) return false;
    ++i;
    return true;
}

// Minimal string parser:
// - out je view na ORIGINAL buffer (bez unescape) ako nema backslash
// - ako ima escape, možemo (1) sporo materijalizovati ili (2) ostaviti raw.
// Za speed: ovde radimo VIEW samo kada nema '\'. Ako ima, fallback na lenju materializaciju u value().
bool JSONParser::parse_json_string_view(std::string_view sv, size_t& i, std::string_view& out) {
    i = skip_ws(sv, i);
    if(i >= sv.size() || sv[i] != '"') return false;
    size_t start = ++i;
    bool hasEscape = false;

    while(i < sv.size()) {
        char c = sv[i];
        if(c == '\\') { hasEscape = true; ++i; if(i < sv.size()) ++i; continue; }
        if(c == '"') break;
        ++i;
    }
    if(i >= sv.size() || sv[i] != '"') return false;

    size_t end = i;
    ++i;

    if(!hasEscape) {
        out = sv.substr(start, end - start);
        return true;
    }

    // escape present: store a sentinel view to the raw segment including quotes?
    // Here we store the inside segment view anyway (raw). materialize() can unescape if you want later.
    out = sv.substr(start, end - start);
    return true;
}

bool JSONParser::parse_json_value_view(std::string_view sv, size_t& i, std::string_view& out) {
    i = skip_ws(sv, i);
    if(i >= sv.size()) return false;

    if(sv[i] == '"') {
        return parse_json_string_view(sv, i, out);
    }

    // bare token: number, true, false, null
    size_t start = i;
    while(i < sv.size()) {
        char c = sv[i];
        if(is_ws((unsigned char)c) || c == ',' || c == '}' || c == ']') break;
        ++i;
    }
    out = sv.substr(start, i - start);
    return true;
}

JSONParser::Mode JSONParser::detectMode(std::string_view sv) const {
    size_t i = skip_bom_and_ws(sv);
    if(i >= sv.size()) return Mode::Unknown;
    char c = sv[i];
    if(c == '[') return Mode::FlatArray;
    if(c == '{') {
        // Heuristika NDJSON: ako ima '\n' pre nego što se zatvori prvi objekat, često je NDJSON (ali ne uvek).
        // Brže: proveri da li posle prvog '}' ima '\n' i opet '{'.
        size_t j = i;
        int depth = 0;
        bool inStr = false;
        for(; j < sv.size(); ++j) {
            char x = sv[j];
            if(inStr) {
                if(x == '\\') { ++j; continue; }
                if(x == '"') inStr = false;
                continue;
            }
            if(x == '"') { inStr = true; continue; }
            if(x == '{') ++depth;
            else if(x == '}') { --depth; if(depth == 0) { ++j; break; } }
        }
        // skip ws
        j = skip_ws(sv, j);
        if(j < sv.size() && sv[j] == '{') return Mode::NDJSON; // multiple objects back-to-back (common in ndjson without newline too)
        // If next non-ws is something else, treat single object
        return Mode::SingleObject;
    }
    return Mode::Unknown;
}

bool JSONParser::mapFile() {
    namespace fs = std::filesystem;
    if(!fs::exists(filename_)) return false;
    size_ = (size_t)fs::file_size(filename_);
    if(size_ == 0) return true;

#ifdef _WIN32
    hFile_ = CreateFileA(filename_.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hFile_ == INVALID_HANDLE_VALUE) return false;

    hMap_ = CreateFileMappingA(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
    if(!hMap_) return false;

    base_ = (const char*)MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0);
    if(!base_) return false;
#else
    fd_ = open(filename_.c_str(), O_RDONLY);
    if(fd_ < 0) return false;
    void* mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if(mapped == MAP_FAILED) return false;
    base_ = (const char*)mapped;
#endif
    return true;
}

void JSONParser::unmapFile() {
#ifdef _WIN32
    if(base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    if(hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
    if(hFile_ != INVALID_HANDLE_VALUE) { CloseHandle(hFile_); hFile_ = INVALID_HANDLE_VALUE; }
#else
    if(base_ && size_) { munmap((void*)base_, size_); base_ = nullptr; }
    if(fd_ >= 0) { close(fd_); fd_ = -1; }
#endif
}

bool JSONParser::load() {
    // reset stanja
    rows_ = cols_ = 0;
    dataViews_.clear();
    colNames_.clear();
    root_ = DataNode("root", "", 0);

    // VAŽNO: očisti stare cache-ove
    cacheString_.clear();
    cacheInt_.clear();
    cacheDouble_.clear();
    cacheBool_.clear();

    if(!opt_.preferFastPath)
        return parseSlowFallback();

    namespace fs = std::filesystem;
    if(!fs::exists(filename_))
        return false;

    // ===== FAST PATH =====
    if(opt_.useMMap) {
        // ❗ unmap starog fajla (ako load zoveš više puta)
        unmapFile();

        if(!mapFile())
            return parseSlowFallback();

        if(parseFast()) {
            notifyLoaded();
            return true;   // ❗ mmap OSTAJЕ ŽIV
        }

        // fallback → moraš unmap
        unmapFile();
        return parseSlowFallback();
    }

    return parseSlowFallback();
}

bool JSONParser::parseFast() {
    if(size_ == 0) return true;
    std::string_view sv(base_, size_);

    Mode m = detectMode(sv);
    switch(m) {
    case Mode::FlatArray: return parseFlatArray(sv);
    case Mode::NDJSON:    return parseNDJSON(sv);
    case Mode::SingleObject:
        // za tvoje merenje (objects/fields) to nije bitno, ali može
        return parseSingleObjectToRoot(sv);
    default:
        return false;
    }
}

// ======= array span extraction =======
// Nalazi opseg svakog top-level objekta u [ ... ] bez parsiranja polja.
// Radi skalarno, ali vrlo brzo (bez alokacija po karakteru).
bool JSONParser::find_object_spans_in_array(std::string_view sv, size_t arrayBegin, size_t arrayEnd,
    std::vector<ObjSpan>& out, bool /*parallelHint*/) {
    out.clear();
    size_t i = arrayBegin;
    i = skip_ws(sv, i);
    if(i >= arrayEnd || sv[i] != '[') return false;
    ++i;

    int depth = 0;
    bool inStr = false;
    size_t objStart = 0;
    bool inObj = false;

    for(; i < arrayEnd; ++i) {
        char c = sv[i];
        if(inStr) {
            if(c == '\\') { ++i; continue; }
            if(c == '"') inStr = false;
            continue;
        }
        else {
            if(c == '"') { inStr = true; continue; }
            if(c == '{') {
                if(!inObj) { inObj = true; objStart = i; }
                ++depth;
            }
            else if(c == '}') {
                --depth;
                if(inObj && depth == 0) {
                    out.push_back({ objStart, i + 1 });
                    inObj = false;
                }
            }
            else if(c == ']') {
                break;
            }
        }
    }
    return !out.empty();
}

// Parse one object to rowOut (aligned to cols_)
bool JSONParser::parseObjectRow(std::string_view obj,
    std::vector<std::string_view>& keysTmp,
    std::vector<std::string_view>& valsTmp,
    std::vector<std::string_view>& rowOut) {
    keysTmp.clear();
    valsTmp.clear();

    size_t i = 0;
    if(!expect_char(obj, i, '{')) return false;

    while(true) {
        i = skip_ws(obj, i);
        if(i >= obj.size()) return false;
        if(obj[i] == '}') { ++i; break; }

        std::string_view key;
        if(!parse_json_string_view(obj, i, key)) return false;
        if(!expect_char(obj, i, ':')) return false;

        std::string_view val;
        if(!parse_json_value_view(obj, i, val)) return false;

        keysTmp.push_back(key);
        valsTmp.push_back(val);

        i = skip_ws(obj, i);
        if(i < obj.size() && obj[i] == ',') { ++i; continue; }
        if(i < obj.size() && obj[i] == '}') { ++i; break; }
    }

    // init rowOut with empties
    rowOut.assign(cols_, std::string_view{});

    // Map keys to columns (linear search is ok for 5 fields; if bigger, we can add hash)
    for(size_t k = 0; k < keysTmp.size(); ++k) {
        auto it = std::find_if(colNames_.begin(), colNames_.end(),
            [&](const std::string& s) { return std::string_view(s) == keysTmp[k]; });
        if(it == colNames_.end()) continue;
        size_t col = (size_t)std::distance(colNames_.begin(), it);
        rowOut[col] = valsTmp[k];
    }
    return true;
}

void JSONParser::buildColumnsFromFirstObject(const std::vector<std::string_view>& keysTmp) {
    colNames_.clear();
    colNames_.reserve(keysTmp.size());
    for(auto k : keysTmp) colNames_.emplace_back(k); // one-time copy
    cols_ = colNames_.size();
}

// ======= parse flat array =======
bool JSONParser::parseFlatArray(std::string_view sv) {
    size_t i0 = skip_bom_and_ws(sv);
    if(i0 >= sv.size() || sv[i0] != '[') return false;

    // find closing ']' quickly (we still need object spans)
    size_t iEnd = sv.size();

    std::vector<ObjSpan> spans;
    if(!find_object_spans_in_array(sv, i0, iEnd, spans, opt_.parallel)) return false;

    // parse first object to get columns
    {
        std::vector<std::string_view> keysTmp, valsTmp, rowTmp;
        std::string_view obj = sv.substr(spans[0].begin, spans[0].end - spans[0].begin);

        // temporary cols_ = number of keys from first object (after build)
        // First parse just keys/vals without mapping
        size_t j = 0;
        if(!expect_char(obj, j, '{')) return false;
        while(true) {
            j = skip_ws(obj, j);
            if(j >= obj.size()) return false;
            if(obj[j] == '}') { ++j; break; }

            std::string_view key;
            if(!parse_json_string_view(obj, j, key)) return false;
            if(!expect_char(obj, j, ':')) return false;
            std::string_view val;
            if(!parse_json_value_view(obj, j, val)) return false;

            keysTmp.push_back(key);
            valsTmp.push_back(val);

            j = skip_ws(obj, j);
            if(j < obj.size() && obj[j] == ',') { ++j; continue; }
            if(j < obj.size() && obj[j] == '}') { ++j; break; }
        }

        if(keysTmp.size() > opt_.maxColumnsHint) return false;
        buildColumnsFromFirstObject(keysTmp);
    }

    rows_ = spans.size();
    dataViews_.assign(rows_ * cols_, std::string_view{});

    // Prepare lazy caches sized for base key(r,c)
    cacheString_.assign(rows_ * cols_, std::nullopt);
    cacheInt_.assign(rows_ * cols_, std::nullopt);
    cacheDouble_.assign(rows_ * cols_, std::nullopt);
    cacheBool_.assign(rows_ * cols_, std::nullopt);

    // parse each object into its row (parallel optional)
    unsigned hw = opt_.threadHint ? opt_.threadHint : std::max<unsigned int>(1u, std::thread::hardware_concurrency());
    bool usePar = opt_.parallel && rows_ >= 200 && hw >= 2; // heuristika

    if(!usePar) {
        std::vector<std::string_view> keysTmp, valsTmp, rowTmp;
        for(size_t r = 0; r < rows_; ++r) {
            std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);
            if(!parseObjectRow(obj, keysTmp, valsTmp, rowTmp)) return false;
            std::memcpy(&dataViews_[r * cols_], rowTmp.data(), cols_ * sizeof(std::string_view));
        }
        return true;
    }

    // parallel: split spans into chunks
    size_t workers = std::min<size_t>(hw, 8); // cap
    size_t chunk = (rows_ + workers - 1) / workers;

    std::vector<std::future<bool>> fut;
    fut.reserve(workers);

    for(size_t w = 0; w < workers; ++w) {
        size_t r0 = w * chunk;
        size_t r1 = std::min<size_t>(rows_, r0 + chunk);
        if(r0 >= r1) break;

        fut.push_back(std::async(std::launch::async, [&, r0, r1]() -> bool {
            std::vector<std::string_view> keysTmp, valsTmp, rowTmp;
            for(size_t r = r0; r < r1; ++r) {
                std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);
                if(!parseObjectRow(obj, keysTmp, valsTmp, rowTmp)) return false;
                std::memcpy(&dataViews_[r * cols_], rowTmp.data(), cols_ * sizeof(std::string_view));
            }
            return true;
            }));
    }

    for(auto& f : fut) if(!f.get()) return false;
    return true;
}

// ======= parse NDJSON =======
bool JSONParser::parseNDJSON(std::string_view sv) {
    size_t i = skip_bom_and_ws(sv);

    // Split by lines (views) — ultra cheap
    std::vector<ObjSpan> spans;
    spans.reserve(1024);

    size_t lineStart = i;
    while(lineStart < sv.size()) {
        size_t lineEnd = lineStart;
        while(lineEnd < sv.size() && sv[lineEnd] != '\n') ++lineEnd;

        // trim \r
        size_t trimEnd = lineEnd;
        if(trimEnd > lineStart && sv[trimEnd - 1] == '\r') --trimEnd;

        // skip empty lines
        size_t j = skip_ws(sv, lineStart);
        if(j < trimEnd && sv[j] == '{') {
            spans.push_back({ lineStart, trimEnd });
        }

        lineStart = (lineEnd < sv.size()) ? (lineEnd + 1) : sv.size();
    }
    if(spans.empty()) return false;

    // parse first line to get columns
    {
        std::vector<std::string_view> keysTmp, valsTmp;
        std::string_view obj = sv.substr(spans[0].begin, spans[0].end - spans[0].begin);

        size_t j = 0;
        if(!expect_char(obj, j, '{')) return false;
        while(true) {
            j = skip_ws(obj, j);
            if(j >= obj.size()) return false;
            if(obj[j] == '}') { ++j; break; }

            std::string_view key, val;
            if(!parse_json_string_view(obj, j, key)) return false;
            if(!expect_char(obj, j, ':')) return false;
            if(!parse_json_value_view(obj, j, val)) return false;

            keysTmp.push_back(key);
            valsTmp.push_back(val);

            j = skip_ws(obj, j);
            if(j < obj.size() && obj[j] == ',') { ++j; continue; }
            if(j < obj.size() && obj[j] == '}') { ++j; break; }
        }

        if(keysTmp.size() > opt_.maxColumnsHint) return false;
        buildColumnsFromFirstObject(keysTmp);
    }

    rows_ = spans.size();
    dataViews_.assign(rows_ * cols_, std::string_view{});
    cacheString_.assign(rows_ * cols_, std::nullopt);
    cacheInt_.assign(rows_ * cols_, std::nullopt);
    cacheDouble_.assign(rows_ * cols_, std::nullopt);
    cacheBool_.assign(rows_ * cols_, std::nullopt);

    unsigned hw = opt_.threadHint ? opt_.threadHint : std::max<unsigned int>(1u, std::thread::hardware_concurrency());
    bool usePar = opt_.parallel && rows_ >= 500 && hw >= 2;

    if(!usePar) {
        std::vector<std::string_view> keysTmp, valsTmp, rowTmp;
        for(size_t r = 0; r < rows_; ++r) {
            std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);
            if(!parseObjectRow(obj, keysTmp, valsTmp, rowTmp)) return false;
            std::memcpy(&dataViews_[r * cols_], rowTmp.data(), cols_ * sizeof(std::string_view));
        }
        return true;
    }

    size_t workers = std::min<size_t>(hw, 8);
    size_t chunk = (rows_ + workers - 1) / workers;

    std::vector<std::future<bool>> fut;
    fut.reserve(workers);

    for(size_t w = 0; w < workers; ++w) {
        size_t r0 = w * chunk;
        size_t r1 = std::min<size_t>(rows_, r0 + chunk);
        if(r0 >= r1) break;

        fut.push_back(std::async(std::launch::async, [&, r0, r1]() -> bool {
            std::vector<std::string_view> keysTmp, valsTmp, rowTmp;
            for(size_t r = r0; r < r1; ++r) {
                std::string_view obj = sv.substr(spans[r].begin, spans[r].end - spans[r].begin);
                if(!parseObjectRow(obj, keysTmp, valsTmp, rowTmp)) return false;
                std::memcpy(&dataViews_[r * cols_], rowTmp.data(), cols_ * sizeof(std::string_view));
            }
            return true;
            }));
    }

    for(auto& f : fut) if(!f.get()) return false;
    return true;
}

// Optional: single object -> root_ (nije fokus performance)
bool JSONParser::parseSingleObjectToRoot(std::string_view sv) {
    // Najbrže: preskoči i vrati prazno root_ ili napravi minimalno.
    // Ako ti treba hijerarhija, onda pravimo pravi parser (ali to je drugi režim).
    root_ = DataNode("root", "", 0);
    return true;
}

bool JSONParser::parseSlowFallback() {
    // Ako hoćeš: ubaci tvoj postojeći load_slow() kod ovde (kopiranje).
    // Za sada: minimalno
    return false;
}

const std::string& JSONParser::materialize(size_t r, size_t c) const {
    size_t k = key(r, c);

    { // quick check without lock (benign data race avoided by always locking for write)
      // can't read optional safely without lock in general; keep it simple:
    }

    std::lock_guard<std::mutex> lk(cacheMutex_);
    if(k >= cacheString_.size()) return empty_;
    if(cacheString_[k].has_value()) return *cacheString_[k];

    auto v = valueView(r, c);
    cacheString_[k] = std::string(v); // NOTE: za escaped string možeš ovde da dodaš unescape
    return *cacheString_[k];
}

const std::string& JSONParser::value(size_t r, size_t c) const {
    if(r >= rows_ || c >= cols_) return empty_;
    return materialize(r, c);
}
