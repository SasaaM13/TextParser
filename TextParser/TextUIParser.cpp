#include "TextUIParser.h"
#include "Timer.h"
#include "JSONParser.h"
#include "XMLParser.h"
#include "XLSXParser.h"
#include "CSVParser.h"

#include <commdlg.h>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>
#include <iomanip>
#include <windowsx.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "Msimg32.lib")  
#include <vssym32.h>
#pragma execution_character_set("utf-8")
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/error/en.h>
#include <nlohmann/json.hpp>
#include <tinyxml2.h>
#include <pugixml.hpp>
#include <vincentlaucsb-csv-parser/csv.hpp>
#include <rapidcsv.h>
#include <OpenXLSX/OpenXLSX.hpp>
#include <xlnt/xlnt.hpp>
#include <Richedit.h>

using namespace std;

// ---------------- UI globals ----------------
HWND TextParserUI::hEditOutput = nullptr;
static HWND hButtonParse = nullptr;
static HFONT hMonoFont = nullptr;
static HBRUSH hBgBrush = nullptr;

// ---------------- Utility ----------------
static std::string GetLowerExt(const std::string& path) {
    size_t pos = path.find_last_of('.');
    std::string ext = (pos != std::string::npos) ? path.substr(pos + 1) : "";
    for(auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

// =======================================================
// JSON helpers
// =======================================================
static bool RapidJSON_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    using namespace rapidjson;
    std::ifstream ifs(path, std::ios::binary);
    if(!ifs) { if(err) *err = "Cannot open file"; return false; }
    IStreamWrapper isw(ifs);
    Document d;
    d.ParseStream(isw);
    if(d.HasParseError()) {
        if(err) *err = GetParseError_En(d.GetParseError());
        return false;
    }

    rows = cols = 0;
    const Value* arr = nullptr;
    if(d.IsArray()) arr = &d;
    else if(d.IsObject()) {
        for(auto it = d.MemberBegin(); it != d.MemberEnd(); ++it)
            if(it->value.IsArray()) { arr = &it->value; break; }
        if(!arr) { rows = 1; cols = (size_t)d.MemberCount(); return true; }
    }
    if(!arr) return false;

    rows = arr->Size();
    for(auto& v : arr->GetArray()) {
        if(v.IsArray()) cols = max(cols, (size_t)v.Size());
        else if(v.IsObject()) cols = max(cols, (size_t)v.MemberCount());
        else cols = max(cols, (size_t)1);
    }
    return true;
}

static bool NlohmannJSON_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    std::ifstream ifs(path);
    if(!ifs) { if(err) *err = "Cannot open file"; return false; }

    rows = cols = 0;
    std::string line;
    std::vector<std::string> colNames;

    while(std::getline(ifs, line)) {
        if(line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            if(j.is_object()) {
                rows++;
                cols = std::max(cols, j.size());
            }
            else if(j.is_array()) {
                rows += j.size();
                if(!j.empty() && j[0].is_object())
                    cols = std::max(cols, j[0].size());
            }
        }
        catch(...) { continue; }
    }
    return rows > 0;
}


// =======================================================
// XML helpers
// =======================================================
// --- rekurzivni brojač ---
static void CountAllAttrs_Tiny(const tinyxml2::XMLElement* e, size_t& attrCount) {
    for(auto a = e->FirstAttribute(); a; a = a->Next()) attrCount++;
    for(auto c = e->FirstChildElement(); c; c = c->NextSiblingElement())
        CountAllAttrs_Tiny(c, attrCount);
}
static void CountAllAttrs_Pugi(const pugi::xml_node& n, size_t& attrCount) {
    for(auto a : n.attributes()) attrCount++;
    for(auto c : n.children()) if(c.type() == pugi::node_element) CountAllAttrs_Pugi(c, attrCount);
}

// TinyXML2
static void Tiny_CountAll(const tinyxml2::XMLElement* e, size_t& elemCount, size_t& attrCount) {
    if(!e) return;
    elemCount++;

    for(auto a = e->FirstAttribute(); a; a = a->Next())
        attrCount++;

    for(auto c = e->FirstChildElement(); c; c = c->NextSiblingElement())
        Tiny_CountAll(c, elemCount, attrCount);
}

static void Tiny_CountAll_Text(const tinyxml2::XMLNode* n, size_t& elem, size_t& text)
{
    if(!n) return;

    if(n->ToElement())
        elem++;

    if(auto t = n->ToText())
    {
        const char* s = t->Value();
        if(s && *s) text++;
    }

    for(auto c = n->FirstChild(); c; c = c->NextSibling())
        Tiny_CountAll_Text(c, elem, text);
}

static bool TinyXML2_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    tinyxml2::XMLDocument doc;
    if(doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        if(err) *err = "TinyXML2: cannot load file";
        return false;
    }

    size_t elem = 0, text = 0;
    Tiny_CountAll_Text(&doc, elem, text);

    rows = elem;
    cols = text;
    return elem > 0;
}

// PugiXML
static void Pugi_CountAll(const pugi::xml_node& n, size_t& elemCount, size_t& attrCount) {
    if(!n || n.type() != pugi::node_element) return;

    elemCount++;

    for(auto a : n.attributes())
        attrCount++;

    for(auto c : n.children())
        if(c.type() == pugi::node_element)
            Pugi_CountAll(c, elemCount, attrCount);
}

static void Pugi_CountAll_Text(pugi::xml_node n, size_t& elem, size_t& text)
{
    if(n.type() == pugi::node_element)
        elem++;

    if(n.type() == pugi::node_pcdata)
        if(n.value() && *n.value())
            text++;

    for(auto c : n.children())
        Pugi_CountAll_Text(c, elem, text);
}

static bool PugiXML_Count(const std::string& filepath, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    pugi::xml_document doc;
    if(!doc.load_file(filepath.c_str()))
    {
        if(err) *err = "PugiXML: cannot load file";
        return false;
    }

    size_t elem = 0;
    size_t text = 0;

    Pugi_CountAll_Text(doc.document_element(), elem, text);

    rows = elem;   // elements
    cols = text;   // text nodes
    return elem > 0;
}




// =======================================================
// CSV helpers
// =======================================================
static bool CSV_Vincent_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    rows = cols = 0;
    try {
        csv::CSVFormat fmt; fmt.variable_columns(true).header_row(-1);
        csv::CSVReader reader(path, fmt);
        for(auto& r : reader) { rows++; cols = max(cols, (size_t)r.size()); }
        return true;
    }
    catch(...) { return false; }
}

static bool CSV_RapidCSV_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    try
    {
        rapidcsv::Document doc(path, rapidcsv::LabelParams(-1, -1));
        rows = doc.GetRowCount(); cols = doc.GetColumnCount(); return true;
    }
    catch(...) { return false; }
}

// =======================================================
// XLSX helpers
// =======================================================
static bool XLSX_OpenXLSX_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    rows = cols = 0;
    try {
        OpenXLSX::XLDocument doc; doc.open(path);
        auto ws = doc.workbook().worksheet(1);
        auto range = ws.range();
        auto tl = range.topLeft();
        auto br = range.bottomRight();
        rows = br.row() - tl.row() + 1;
        cols = br.column() - tl.column() + 1;
        doc.close();
        return true;
    }
    catch(...) { return false; }
}

static bool XLSX_xlnt_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
    try {
        xlnt::workbook wb; wb.load(path);
        auto ws = wb.active_sheet();
        rows = ws.highest_row(); cols = ws.highest_column().index;
        return true;
    }
    catch(...) { return false; }
}

// =======================================================
// RichEdit helpers (UTF-8 safe + emoji-aware)
// =======================================================
static bool HasEmoji(const std::string& s)
{
    // Prosti heuristički check — traži UTF-8 bajtove iznad U+1F000
    for(unsigned char c : s)
        if((c & 0xF0) == 0xF0) return true; // 4-bajtni UTF-8 (emoji, simboli)
    return false;
}

static void RE_AppendColored(HWND hRE, COLORREF color, bool bold, const std::string& text)
{
    CHARRANGE endSel{ -1, -1 };
    SendMessageW(hRE, EM_EXSETSEL, 0, (LPARAM)&endSel);

    bool hasEmoji = false;
    for(unsigned char c : text)
        if((c & 0xF0) == 0xF0) { hasEmoji = true; break; }

    const wchar_t* fontFace = hasEmoji ? L"Segoe UI Emoji" : L"Consolas";

    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_FACE;
    cf.crTextColor = color;
    cf.wWeight = bold ? FW_BOLD : FW_NORMAL;
    wcscpy_s(cf.szFaceName, fontFace);
    SendMessageW(hRE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if(len <= 1) return;

    std::wstring wtext(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext.data(), len);

    SendMessageW(hRE, EM_REPLACESEL, FALSE, (LPARAM)wtext.c_str());
}

static void RE_AppendLine(HWND hRE, COLORREF color, bool bold, const std::string& line)
{
    RE_AppendColored(hRE, color, bold, line + "\r\n");
}

static void PrettyPrintHeader(HWND hRE, const std::string& filepath)
{
    std::string icon = "📄";
    std::string ext = GetLowerExt(filepath);
    if(ext == "csv") icon = "📄 CSV";
    else if(ext == "json") icon = "🧩 JSON";
    else if(ext == "xml") icon = "🗂️ XML";
    else if(ext == "xlsx") icon = "📊 XLSX";

    RE_AppendLine(hRE, RGB(30, 144, 255), true,
        "Benchmarking parsers for: " + icon + "  →  " + filepath);
    RE_AppendLine(hRE, RGB(120, 120, 120), false,
        "---------------------------------------------");
}


// =======================================================
// Window Procedure with Gradient + Custom Button
// =======================================================
LRESULT CALLBACK TextParserUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));

    switch(msg)
    {
    case WM_CREATE:
        hBgBrush = CreateSolidBrush(RGB(248, 248, 255));
        break;

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        HDC hdc = (HDC)wParam;

        TRIVERTEX vertex[2] = {
            {0, 0, 20000, 26000, 65535, 0x0000},
            {rc.right, rc.bottom, 65535, 65535, 65535, 0x0000}
        };
        GRADIENT_RECT gRect = { 0, 1 };
        GradientFill(hdc, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
        return 1;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if(dis->CtlID == 1001) {
            RECT rc = dis->rcItem;
            HDC hdc = dis->hDC;
            bool hovered = (dis->itemState & ODS_HOTLIGHT);
            COLORREF start = hovered ? RGB(0, 180, 255) : RGB(0, 120, 215);
            COLORREF end = RGB(0, 80, 180);

            TRIVERTEX v[2] = {
                {rc.left, rc.top, GetRValue(start) << 8, GetGValue(start) << 8, GetBValue(start) << 8, 0},
                {rc.right, rc.bottom, GetRValue(end) << 8, GetGValue(end) << 8, GetBValue(end) << 8, 0}
            };
            GRADIENT_RECT gr = { 0, 1 };
            GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, L"📂  Open File & Benchmark", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
    }
    break;

    case WM_COMMAND:
        if(LOWORD(wParam) == 1001) {
            std::string filepath = OpenFileDialog(hwnd);
            if(!filepath.empty()) {
                SetWindowTextW(hEditOutput, L"");
                PrettyPrintHeader(hEditOutput, filepath);
                ParseFile(filepath);
            }
        }
        break;

    case WM_DESTROY:
        if(hBgBrush) DeleteObject(hBgBrush);
        if(hMonoFont) DeleteObject(hMonoFont);
        if(hWhiteBrush) DeleteObject(hWhiteBrush);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// =======================================================
// Run()
// =======================================================
int TextParserUI::Run(HINSTANCE hInst, int nCmdShow)
{
    LoadLibraryW(L"Msftedit.dll");

    const wchar_t CLASS_NAME[] = L"TextParserUI";
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(WS_EX_APPWINDOW, CLASS_NAME, L"📊 Text File Parser Benchmark",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
        nullptr, nullptr, hInst, nullptr);

    hButtonParse = CreateWindowEx(0, L"BUTTON", L"📂  Open File & Benchmark",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
        370, 60, 260, 45, hwnd, (HMENU)1001, hInst, nullptr);

    hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        40, 120, 900, 540, hwnd, nullptr, hInst, nullptr);

    LOGFONTW lf{}; lf.lfHeight = -18; wcscpy_s(lf.lfFaceName, L"Consolas");
    hMonoFont = CreateFontIndirectW(&lf);
    SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hMonoFont, TRUE);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while(GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
// ======================================================================
// PRINT FIRST 50 XML ELEMENTS (Custom / TinyXML2 / PugiXML)
// ======================================================================

// ----------------------------------------------------------------------------------
// Custom ultra-fast XML (skimming only, zero overhead, no recursion, raw buffer scan)
// ----------------------------------------------------------------------------------
static void PrintFirst50_CustomXML(const std::string& path, int n)
{
    XMLParser xml(path);
    if(!xml.load()) return;

    RE_AppendLine(TextParserUI::hEditOutput, RGB(0, 180, 120), true,
        "Custom XML (first 50 elements):");

    Timer T;
    T.start();

    size_t total = xml.elementCount();
    size_t limit = std::min<size_t>(n, total);

    for(size_t i = 0; i < limit; i++)
    {
        auto opt = xml.getByIndex(i);
        if(!opt.has_value()) continue;    // <-- fixed

        const auto& node = opt.value();   // reference

        // ==== Trim value ====
        std::string val(node.text);       // convert view → string

        auto trim = [&](std::string& s) {
            size_t start = 0;
            while(start < s.size() && isspace((unsigned char)s[start])) start++;

            size_t end = s.size();
            while(end > start && isspace((unsigned char)s[end - 1])) end--;

            s = s.substr(start, end - start);
            };
        trim(val);

        // Skip empty values
        if(val.empty()) continue;

        std::string line =
            "[" + std::to_string(i) + "] <" + std::string(opt.value().tag) + "> = \"" + val + "\"";

        RE_AppendLine(TextParserUI::hEditOutput, RGB(0, 140, 120), false, line);
    }

    double secs = T.seconds();
    RE_AppendLine(TextParserUI::hEditOutput, RGB(80, 80, 80), false,
        "⏱ CustomXML extract time = " + std::to_string(secs) + " s\n");
}




// --------------------------------------------
// TinyXML2 – first 50 text-bearing elements
// --------------------------------------------
static void PrintFirst50_Tiny(const std::string& path, int ns)
{
    tinyxml2::XMLDocument doc;
    if(doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
        return;

    RE_AppendLine(TextParserUI::hEditOutput, RGB(180, 120, 0), true,
        "TinyXML2 (first n elements):");

    Timer T;
    T.start();

    size_t printed = 0;

    std::function<void(const tinyxml2::XMLNode*)> dfs =
        [&](const tinyxml2::XMLNode* n)
        {
            if(printed >= ns || !n) return;

            if(auto e = n->ToElement())
            {
                const char* text = e->GetText();
                if(text && *text)
                {
                    std::string line = "[" + std::to_string(printed) + "] <"
                        + e->Name() + "> = \"" + text + "\"";
                    RE_AppendLine(TextParserUI::hEditOutput, RGB(180, 140, 0), false, line);
                    printed++;
                }
            }
            for(auto c = n->FirstChild(); c && printed < ns; c = c->NextSibling())
                dfs(c);
        };

    dfs(doc.RootElement());

    double secs = T.seconds();
    RE_AppendLine(TextParserUI::hEditOutput, RGB(80, 80, 80), false,
        "⏱ TinyXML2 extract time = " + std::to_string(secs) + " s\n");
}



// --------------------------------------------
// PugiXML – first 50 text nodes
// --------------------------------------------
static void PrintFirst50_Pugi(const std::string& path, int ns)
{
    pugi::xml_document doc;
    if(!doc.load_file(path.c_str()))
        return;

    RE_AppendLine(TextParserUI::hEditOutput, RGB(0, 120, 200), true,
        "PugiXML (first n elements):");

    Timer T;
    T.start();

    size_t printed = 0;

    std::function<void(pugi::xml_node)> dfs =
        [&](pugi::xml_node n)
        {
            if(printed >= ns) return;
            if(n.type() == pugi::node_element)
            {
                auto txt = n.first_child();
                if(txt && txt.type() == pugi::node_pcdata)
                {
                    std::string line = "[" + std::to_string(printed) + "] <" +
                        std::string(n.name()) + "> = \"" + txt.value() + "\"";
                    RE_AppendLine(TextParserUI::hEditOutput, RGB(0, 150, 200), false, line);
                    printed++;
                }
            }
            for(pugi::xml_node c : n.children())
                if(printed < ns) dfs(c);
        };

    dfs(doc.document_element());

    double secs = T.seconds();
    RE_AppendLine(TextParserUI::hEditOutput, RGB(80, 80, 80), false,
        "⏱ PugiXML extract time = " + std::to_string(secs) + " s\n");
}


// =======================================================
// File Dialog
// =======================================================
std::string TextParserUI::OpenFileDialog(HWND hwnd)
{
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "All Supported\0*.csv;*.json;*.xml;*.xlsx\0CSV\0*.csv\0JSON\0*.json\0XML\0*.xml\0XLSX\0*.xlsx\0";
    ofn.lpstrFile = filename; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? std::string(filename) : "";
}

// =======================================================
// Benchmark framework + output
// =======================================================
struct BenchLine {
    std::string name; bool ok; size_t rows, cols; double secs; std::string err;
};

template<typename F>
static BenchLine RunOne(const std::string& name, F&& f)
{
    BenchLine b{ name,false,0,0,0.0,{} };
    Timer t; t.start();
    try { b.ok = f(b.rows, b.cols, b.err); }
    catch(...) { b.ok = false; }
    t.end(); b.secs = t.seconds();
    return b;
}

static void PrintLine(const BenchLine& b)
{
    std::ostringstream ln;
    if(b.ok) {
        ln << std::left << std::setw(22) << b.name << "  ";

        // detektuj tip benchmarka iz imena
        std::string lower = b.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if(lower.find("csv") != std::string::npos ||
            lower.find("xlsx") != std::string::npos)
        {
            ln << "rows=" << b.rows << ", cols=" << b.cols;
        }
        else if(lower.find("json") != std::string::npos)
        {
            ln << "objects=" << b.rows << ", fields=" << b.cols;
        }
        else if(lower.find("xml") != std::string::npos)
        {
            ln << "elements=" << b.rows << ", textNodes=" << b.cols;
        }
        else {
            ln << "entries=" << b.rows << ", values=" << b.cols;
        }

        ln << ", t=" << std::fixed << std::setprecision(4) << b.secs << "s";
        RE_AppendLine(TextParserUI::hEditOutput, RGB(34, 139, 34), false, ln.str());
    }
    else {
        ln << std::left << std::setw(22) << b.name << "  FAILED";
        if(!b.err.empty()) ln << "  (" << b.err << ")";
        RE_AppendLine(TextParserUI::hEditOutput, RGB(178, 34, 34), true, ln.str());
    }
}


// =======================================================
// ParseFile + comparison section
// =======================================================
void TextParserUI::ParseFile(const std::string& filepath)
{
    const std::string ext = GetLowerExt(filepath);
    std::vector<BenchLine> lines;

    if(ext == "csv") {
        lines.push_back(RunOne("Custom CSV", [&](size_t& r, size_t& c, std::string& e) {
            CSVParser::Options opt;
            opt.delimiter = ',';
            opt.hasHeader = true;
            opt.maxRows = -1;
            opt.maxCols = -1;

            CSVParser csv(filepath, opt);
            bool ok = csv.load();
            if(ok) { r = csv.totalrowCount(); c = csv.colCount(); }
            return ok;
            }));

        lines.push_back(RunOne("vincent csv-parser", [&](size_t& r, size_t& c, std::string& e) {
            return CSV_Vincent_Count(filepath, r, c, &e);
            }));

        lines.push_back(RunOne("rapidcsv", [&](size_t& r, size_t& c, std::string& e) {
            return CSV_RapidCSV_Count(filepath, r, c, &e);
            }));
    }
    else if(ext == "json") {
        lines.push_back(RunOne("Custom JSON", [&](size_t& r, size_t& c, std::string& e) {
            JSONParser j(filepath);
            bool ok = j.load();
            if(ok) { r = j.rowCount(); c = j.colCount(); }
            return ok;
            }));
        lines.push_back(RunOne("RapidJSON", [&](size_t& r, size_t& c, std::string& e) {
            return RapidJSON_Count(filepath, r, c, &e);
            }));
        lines.push_back(RunOne("nlohmann/json", [&](size_t& r, size_t& c, std::string& e) {
            return NlohmannJSON_Count(filepath, r, c, &e);
            }));
    }
    else if(ext == "xml") {
        lines.push_back(RunOne("Custom XML", [&](size_t& r, size_t& c, std::string& e) {
            XMLParser x(filepath);
            bool ok = x.load();
            if(ok) { r = x.elementCount(); c = x.textNodeCount(); }
            return ok;
            }));
        lines.push_back(RunOne("tinyxml2", [&](size_t& r, size_t& c, std::string& e) {
            return TinyXML2_Count(filepath, r, c, &e);
            }));
        lines.push_back(RunOne("pugixml", [&](size_t& r, size_t& c, std::string& e) {
            return PugiXML_Count(filepath, r, c, &e);
            }));
    }
    else if(ext == "xlsx") {
        lines.push_back(RunOne("Custom XLSX", [&](size_t& r, size_t& c, std::string& e) {
            XLSXParser x(filepath);
            bool ok = x.load();
            if(ok) { r = x.rowCount(); c = x.colCount(); }
            return ok;
            }));
        lines.push_back(RunOne("OpenXLSX", [&](size_t& r, size_t& c, std::string& e) {
            return XLSX_OpenXLSX_Count(filepath, r, c, &e);
            }));
        lines.push_back(RunOne("xlnt", [&](size_t& r, size_t& c, std::string& e) {
            return XLSX_xlnt_Count(filepath, r, c, &e);
            }));
    }
    else {
        RE_AppendLine(hEditOutput, RGB(178, 34, 34), true, "Unsupported file type: " + ext);
        return;
    }

    if(ext == "xml")
    {
        int n = 5000;
        RE_AppendLine(hEditOutput, RGB(100, 100, 100), true,
            "\n🔎 Extracting first nth XML elements...\n");

        PrintFirst50_CustomXML(filepath,n);
        PrintFirst50_Tiny(filepath,n);
        PrintFirst50_Pugi(filepath,n);
    }

    // Print individual results
    for(auto& b : lines) PrintLine(b);

    // ===== Enhanced Performance Comparison Section =====
    std::vector<BenchLine> okLines;
    for(auto& b : lines)
        if(b.ok)
            okLines.push_back(b);

    if(okLines.size() < 2) return;

    std::sort(okLines.begin(), okLines.end(),
        [](const BenchLine& a, const BenchLine& b) { return a.secs < b.secs; });

    double fastest = okLines.front().secs;
    RE_AppendLine(hEditOutput, RGB(100, 100, 100), true, "\r\n📈 Performance Comparison");
    RE_AppendLine(hEditOutput, RGB(150, 150, 150), false, "------------------------------------------");

    for(size_t i = 0; i < okLines.size(); ++i) {
        const auto& b = okLines[i];
        double ratio = b.secs / fastest;
        double slower = (ratio - 1.0) * 100.0;

        std::string medal;
        COLORREF color = RGB(50, 50, 50);
        if(i == 0) { medal = "[#1]"; color = RGB(255, 215, 0); }
        else if(i == 1) { medal = "[#2]"; color = RGB(192, 192, 192); }
        else if(i == 2) { medal = "[#3]"; color = RGB(205, 127, 50); }

        // ASCII progress bar
        int barLen = 30;
        int fill = (int)(barLen / ratio);
        if(fill < 1) fill = 1;
        if(fill > barLen) fill = barLen;
        std::string bar(fill, '█');
        bar += std::string(barLen - fill, '░');

        std::ostringstream line;
        line << std::left << std::setw(2) << medal << " "
            << std::left << std::setw(20) << b.name
            << " | " << std::right << std::setw(7)
            << std::fixed << std::setprecision(4) << b.secs << " s"
            << " | " << std::left << std::setw(12)
            << (i == 0 ? "FASTEST" : (std::to_string((int)slower) + "% slower"));

        RE_AppendLine(hEditOutput, color, false, line.str());
    }

}
