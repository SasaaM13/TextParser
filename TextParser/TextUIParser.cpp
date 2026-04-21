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
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
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
#ifdef GetObject
#undef GetObject
#endif
#define TEST 1
using namespace std;

// ---------------- UI globals ----------------
HWND TextParserUI::hEditOutput = nullptr;
static HWND hButtonParse = nullptr;
static HFONT hMonoFont = nullptr;
static HBRUSH hBgBrush = nullptr;
static HWND hListView = nullptr;


struct ListSortCtx
{
	HWND hList = nullptr;
	int col = 0;
	bool asc = true;
};

static ListSortCtx g_listSortCtx{};
static double g_bestTotal = -1.0;

static int CALLBACK CompareListItems(LPARAM l1, LPARAM l2, LPARAM)
{
	wchar_t t1[128]{}, t2[128]{};

	ListView_GetItemText(g_listSortCtx.hList, (int)l1, g_listSortCtx.col, t1, 128);
	ListView_GetItemText(g_listSortCtx.hList, (int)l2, g_listSortCtx.col, t2, 128);

	int result = 0;

	if(g_listSortCtx.col == 0)
	{
		result = _wcsicmp(t1, t2);
	}
	else
	{
		double v1 = _wtof(t1);
		double v2 = _wtof(t2);

		if(v1 < v2) result = -1;
		else if(v1 > v2) result = 1;
		else result = 0;
	}

	return g_listSortCtx.asc ? result : -result;
}
// ---------------- Utility ----------------
static std::string GetLowerExt(const std::string& path) {
	size_t pos = path.find_last_of('.');
	std::string ext = (pos != std::string::npos) ? path.substr(pos + 1) : "";
	for(auto& c : ext) c = (char)tolower((unsigned char)c);
	return ext;
}

// =======================================================
// JSON helpers
static bool RapidJSON_Count(
	const std::string& path,
	size_t& rows,
	size_t& fields,
	std::string* err = nullptr)
{
	using namespace rapidjson;

	std::ifstream ifs(path);
	if(!ifs) {
		if(err) *err = "Cannot open file";
		return false;
	}

	// === KLJUČNO: parse cijelog fajla, NE line-by-line ===
	IStreamWrapper isw(ifs);
	Document d;
	d.ParseStream(isw);

	if(d.HasParseError()) {
		if(err) {
			*err = std::string(GetParseError_En(d.GetParseError())) +
				" (offset=" + std::to_string(d.GetErrorOffset()) + ")";
		}
		return false;
	}

	rows = 0;
	fields = 0;

	// === Obrada strukture ===
	if(d.IsArray()) {
		rows = d.Size();

		for(const auto& v : d.GetArray()) {
			if(v.IsObject())
				fields += v.MemberCount();
			else
				fields += 1;
		}
	}
	else if(d.IsObject()) {
		rows = 1;
		fields = d.MemberCount();
	}
	else {
		rows = 1;
		fields = 1;
	}

	return true;
}
static bool NlohmannJSON_Count(
	const std::string& path,
	size_t& rows,
	size_t& fields,
	std::string* err = nullptr)
{
	std::ifstream ifs(path);
	if(!ifs) {
		if(err) *err = "Cannot open file";
		return false;
	}

	rows = 0;
	fields = 0;

	try
	{
		nlohmann::json j;
		ifs >> j;

		if(j.is_array()) {
			rows = j.size();

			for(const auto& v : j) {
				if(v.is_object())
					fields += v.size();
				else
					fields += 1;
			}
		}
		else if(j.is_object()) {
			rows = 1;
			fields = j.size();
		}
		else {
			rows = 1;
			fields = 1;
		}
	}
	catch(const std::exception& ex)
	{
		if(err) *err = ex.what();
		return false;
	}

	return true;
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
	{
		hBgBrush = CreateSolidBrush(RGB(248, 248, 255));

		INITCOMMONCONTROLSEX icc{};
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_LISTVIEW_CLASSES;
		InitCommonControlsEx(&icc);
		hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
			40, 180, 900, 480,
			hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

		ListView_SetExtendedListViewStyle(hListView,
			LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
		auto AddCol = [&](int i, const wchar_t* name, int w)
			{
				LVCOLUMNW col{};
				col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
				col.pszText = const_cast<LPWSTR>(name);
				col.cx = w;
				col.iSubItem = i;
				ListView_InsertColumn(hListView, i, &col);
			};

		AddCol(0, L"Parser", 180);
		AddCol(1, L"Load (s)", 100);
		AddCol(2, L"Scan (s)", 100);
		AddCol(3, L"Total (s)", 100);
		AddCol(4, L"MB/s", 80);
		AddCol(5, L"Values/s", 140);
		break;
	}

	case WM_ERASEBKGND:
	{
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

	case WM_NOTIFY:
	{
		LPNMHDR hdr = (LPNMHDR)lParam;

		if(hdr->hwndFrom == hListView && hdr->code == LVN_COLUMNCLICK)
		{
			static int sortCol = 0;
			static bool asc = true;

			auto* p = (NMLISTVIEW*)lParam;

			if(sortCol == p->iSubItem) asc = !asc;
			else { sortCol = p->iSubItem; asc = true; }

			g_listSortCtx.hList = hListView;
			g_listSortCtx.col = sortCol;
			g_listSortCtx.asc = asc;

			ListView_SortItems(hListView, CompareListItems, 0);

			g_bestTotal = -1.0;
			InvalidateRect(hListView, nullptr, TRUE);
			return 0;
		}

		if(hdr->hwndFrom == hListView && hdr->code == NM_CUSTOMDRAW)
		{
			LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;

			if(cd->nmcd.dwDrawStage == CDDS_PREPAINT)
				return CDRF_NOTIFYITEMDRAW;

			if(cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
			{
				int row = (int)cd->nmcd.dwItemSpec;

				if(g_bestTotal < 0.0)
				{
					int count = ListView_GetItemCount(hListView);
					g_bestTotal = 1e100;

					for(int i = 0; i < count; ++i)
					{
						wchar_t buf[64]{};
						ListView_GetItemText(hListView, i, 3, buf, 64); // Total kolona
						double v = _wtof(buf);
						if(v < g_bestTotal) g_bestTotal = v;
					}
				}

				wchar_t buf[64]{};
				ListView_GetItemText(hListView, row, 3, buf, 64);
				double val = _wtof(buf);

				if(fabs(val - g_bestTotal) < 1e-9)
				{
					cd->clrText = RGB(0, 120, 0);
					cd->clrTextBk = RGB(220, 255, 220);
				}

				return CDRF_NEWFONT;
			}
		}

		break;
	}

	case WM_DRAWITEM:
	{
		LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;

		if(dis->CtlID == 1001)
		{
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

			DrawTextW(hdc, L"Open File", -1, &rc,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			return TRUE;
		}
		break;
	}

	case WM_COMMAND:
	{
		if(LOWORD(wParam) == 1001)
		{
			std::string filepath = OpenFileDialog(hwnd);

			if(!filepath.empty())
			{
				// clear listview
				ListView_DeleteAllItems(hListView);

				// reset fastest cache
				g_bestTotal = -1.0;

				// clear rich edit header/output
				SetWindowTextW(hEditOutput, L"");
				UpdateWindow(hEditOutput);

				PrettyPrintHeader(hEditOutput, filepath);
				ParseFile(filepath);
			}
			break;
		}
		break;
	}

	case WM_DESTROY:
	{
		if(hBgBrush) DeleteObject(hBgBrush);
		if(hMonoFont) DeleteObject(hMonoFont);
		if(hWhiteBrush) DeleteObject(hWhiteBrush);
		PostQuitMessage(0);
		break;
	}
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
		WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		40, 120, 900, 50, hwnd, nullptr, hInst, nullptr);

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

// ======================= BENCH STRUCT =======================
struct BenchLine
{
	std::string name;
	bool ok = false;

	size_t rows = 0;
	size_t cols = 0;

	double loadSecs = 0.0;
	double scanSecs = 0.0;
	double parseSecs = 0.0;
	double mbPerSec = 0.0;
	uint64_t valuesPerSec = 0;

	std::string err;

	double total() const
	{
		return loadSecs + scanSecs + parseSecs;
	}
};

// ======================= MEASURE =======================
static double Measure(std::function<void()> fn)
{
	Timer t; t.start();
	fn();
	t.end();
	return t.seconds();
}

// ======================= FULL SCAN =======================
template<typename GetValueFn>
double RunFullScan(size_t rows, size_t cols, GetValueFn get)
{
	return Measure([&]()
		{
			volatile size_t sink = 0;

			for(size_t i = 0; i < rows; ++i)
				for(size_t j = 0; j < cols; ++j)
					sink += get(i, j).size();
		});
}

// ======================= PARSE ALL =======================
template<typename GetValueFn>
double RunParseAll(size_t rows, size_t cols, GetValueFn get)
{
	return Measure([&]()
		{
			volatile double sum = 0.0;

			for(size_t i = 0; i < rows; ++i)
				for(size_t j = 0; j < cols; ++j)
				{
					auto v = get(i, j);
					sum += std::strtod(v.data(), nullptr);
				}
		});
}

// ================= HELPERS =================
inline double safeTime(double t)
{
	return (t < 1e-6) ? 1e-6 : t;
}

inline std::string fmtTime(double v)
{
	std::ostringstream ss;
	if(v < 0.001) ss << std::fixed << std::setprecision(6) << v;
	else if(v < 0.1) ss << std::fixed << std::setprecision(4) << v;
	else ss << std::fixed << std::setprecision(3) << v;
	return ss.str();
}

inline int getRepeat(double fileMB)
{
	if(fileMB < 0.1) return 200;
	if(fileMB < 1.0) return 50;
	if(fileMB < 5.0) return 10;
	return 1;
}


// ================= MAIN =================
void TextParserUI::ParseFile(const std::string& filepath)
{
	const std::string ext = GetLowerExt(filepath);
	std::vector<BenchLine> lines;

	double fileMB = 0.0;
	try {
		fileMB = std::filesystem::file_size(filepath) / (1024.0 * 1024.0);
	}
	catch(...) {}

	// preload
	std::string fileContent;
	{
		std::ifstream f(filepath, std::ios::binary);
		fileContent.assign((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	}

	int repeat = getRepeat(fileMB);

	// ================= CSV =================
	if(ext == "csv")
	{
		// ===== Custom CSV =====
		lines.push_back([&]()
			{
				BenchLine b; b.name = "Custom CSV";

				CSVParser::Options opt;
				opt.delimiter = ',';
				opt.hasHeader = true;
				opt.allowQuotes = false;
				opt.useMMap = true;

				CSVParser csv(filepath, opt);

				Timer t; t.start();
				b.ok = csv.load();
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;
				size_t r = csv.rowCount();
				size_t c = csv.colCount();
				size_t values = r * c;

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							for(size_t i = 0; i < r; ++i)
								for(size_t j = 0; j < c; ++j)
									sink += csv.valueView(i, j).size();
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// ===== Vincent CSV =====
		lines.push_back([&]()
			{
				BenchLine b; b.name = "Vincent CSV";

				Timer t; t.start();
				csv::CSVReader reader(filepath);
				t.end(); b.loadSecs = t.seconds();

				size_t values = 0;
				for(auto& row : reader)
					values += row.size();

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
						{
							csv::CSVReader r(filepath);
							for(auto& row : r)
								for(auto& f : row)
									sink += f.get<std::string_view>().size();
						}
					}) / repeat;

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// ===== rapidcsv =====
		lines.push_back([&]()
			{
				BenchLine b; b.name = "rapidcsv";

				std::istringstream ss(fileContent);

				Timer t; t.start();
				rapidcsv::Document doc(ss);
				size_t r = doc.GetRowCount();
				size_t c = doc.GetColumnCount();
				t.end(); b.loadSecs = t.seconds();

				size_t values = r * c;

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							for(size_t i = 0; i < r; ++i)
								for(size_t j = 0; j < c; ++j)
									sink += doc.GetCell<std::string>(j, i).size();
					}) / repeat;

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());
	}

	// ================= JSON =================
	else if(ext == "json")
	{
		lines.push_back([&]()
			{
				BenchLine b; b.name = "Custom JSON";

				JSONParser j(filepath);

				Timer t; t.start();
				b.ok = j.load();
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;

				size_t values = j.rowCount() * j.totalFields();

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							for(size_t i = 0; i < j.rowCount(); ++i)
								for(size_t k = 0; k < j.colCount(); ++k)
									sink += j.valueView(i, k).size();
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		lines.push_back([&]()
			{
				BenchLine b; b.name = "RapidJSON";

				rapidjson::Document doc;

				Timer t; t.start();
				doc.Parse(fileContent.c_str());
				t.end(); b.loadSecs = t.seconds();
				if(doc.HasParseError()) return b;

				size_t values = 0;
				std::function<void(const rapidjson::Value&)> walk;

				walk = [&](const rapidjson::Value& v)
					{
						if(v.IsString()) { values++; }
						else if(v.IsArray()) for(auto& x : v.GetArray()) walk(x);
						else if(v.IsObject()) for(auto& m : v.GetObject()) walk(m.value);
					};

				walk(doc);

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							walk(doc);
					}) / repeat;

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());
		lines.push_back([&]()
			{
				BenchLine b; b.name = "nlohmann/json";

				Timer t; t.start();
				nlohmann::json j = nlohmann::json::parse(fileContent);
				t.end(); b.loadSecs = t.seconds();

				size_t values = 0;

				std::function<void(const nlohmann::json&)> walk;
				walk = [&](const nlohmann::json& v)
					{
						if(v.is_string()) values++;
						else if(v.is_array()) for(auto& x : v) walk(x);
						else if(v.is_object()) for(auto& x : v.items()) walk(x.value());
					};
				walk(j);

				b.scanSecs = Measure([&]()
					{
						for(int rep = 0; rep < repeat; ++rep)
							walk(j);
					}) / repeat;

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());
	}

	// ================= XML =================
	else if(ext == "xml")
	{
		// Custom
		lines.push_back([&]()
			{
				BenchLine b; b.name = "Custom XML";

				XMLParser x(filepath);

				Timer t; t.start();
				b.ok = x.load();
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;

				size_t values = x.textNodeCount();

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							for(size_t i = 0; i < values; ++i)
							{
								auto v = x.getByIndex(i);
								if(v) sink += v->text.size();
							}
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// TinyXML2
		lines.push_back([&]()
			{
				BenchLine b; b.name = "tinyxml2";

				tinyxml2::XMLDocument doc;

				Timer t; t.start();
				b.ok = doc.Parse(fileContent.c_str()) == tinyxml2::XML_SUCCESS;
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;

				// ===== COUNT (SAMO JEDNOM) =====
				size_t values = 0;

				std::function<void(tinyxml2::XMLNode*)> countWalk;
				countWalk = [&](tinyxml2::XMLNode* n)
					{
						if(auto txt = n->ToText())
							if(txt->Value() && *txt->Value())
								values++;

						for(auto c = n->FirstChild(); c; c = c->NextSibling())
							countWalk(c);
					};

				countWalk(doc.RootElement());

				// ===== SCAN (NE MIJENJA values) =====
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;

						std::function<void(tinyxml2::XMLNode*)> scanWalk;
						scanWalk = [&](tinyxml2::XMLNode* n)
							{
								if(auto txt = n->ToText())
									if(txt->Value())
										sink += strlen(txt->Value());

								for(auto c = n->FirstChild(); c; c = c->NextSibling())
									scanWalk(c);
							};

						for(int rep = 0; rep < repeat; ++rep)
							scanWalk(doc.RootElement());
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);
				b.ok = true;

				return b;
			}());

		// Pugi
		lines.push_back([&]()
			{
				BenchLine b; b.name = "pugixml";

				pugi::xml_document doc;

				Timer t; t.start();
				b.ok = doc.load_buffer(fileContent.data(), fileContent.size());
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;

				// ===== COUNT =====
				size_t values = 0;

				std::function<void(pugi::xml_node)> countWalk;
				countWalk = [&](pugi::xml_node n)
					{
						if(n.type() == pugi::node_pcdata && *n.value())
							values++;

						for(auto c : n.children())
							countWalk(c);
					};

				countWalk(doc);

				// ===== SCAN =====
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;

						std::function<void(pugi::xml_node)> scanWalk;
						scanWalk = [&](pugi::xml_node n)
							{
								if(n.type() == pugi::node_pcdata)
									sink += strlen(n.value());

								for(auto c : n.children())
									scanWalk(c);
							};

						for(int rep = 0; rep < repeat; ++rep)
							scanWalk(doc);
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);
				b.ok = true;

				return b;
			}());
	}
	// ================= XLSX =================
	else if(ext == "xlsx")
	{
		// Custom
		lines.push_back([&]()
			{
				BenchLine b; b.name = "Custom XLSX";

				XLSXParser x(filepath);

				Timer t; t.start();
				b.ok = x.load();
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) return b;

				size_t r = x.rowCount();
				size_t c = x.colCount();
				size_t values = r * c;

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
							for(size_t i = 0; i < r; ++i)
								for(size_t j = 0; j < c; ++j)
									sink += x.valueView(i, j).size();
					}) / repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// OpenXLSX
		lines.push_back([&]()
			{
				BenchLine b; b.name = "OpenXLSX";

				using namespace OpenXLSX;

				Timer t; t.start();
				XLDocument doc;
				doc.open(filepath);
				auto ws = doc.workbook().worksheet(doc.workbook().worksheetNames()[0]);
				t.end(); b.loadSecs = t.seconds();

				size_t values = 0;

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(auto& row : ws.rows())
							for(auto& cell : row.cells())
							{
								try {
									auto v = cell.value().get<std::string>();
									sink += v.size();
									values++;
								}
								catch(...) {}
							}
					});

				doc.close();

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// xlnt
		lines.push_back([&]()
			{
				BenchLine b; b.name = "xlnt";

				Timer t; t.start();
				xlnt::workbook wb;
				wb.load(filepath);
				auto ws = wb.active_sheet();
				t.end(); b.loadSecs = t.seconds();

				size_t values = 0;

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(auto row : ws.rows(false))
							for(auto cell : row)
							{
								try {
									auto v = cell.to_string();
									sink += v.size();
									values++;
								}
								catch(...) {}
							}
					});

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());
	}

	// ================= SORT =================
	ListView_DeleteAllItems(hListView);
	g_bestTotal = -1.0;

	int idx = 0;

	for(const auto& b : lines)
	{
		double load = b.loadSecs;
		double scan = b.scanSecs;
		double total = load + scan;

		std::wstring name(b.name.begin(), b.name.end());

		LVITEMW item{};
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = idx;
		item.iSubItem = 0;
		item.pszText = const_cast<LPWSTR>(name.c_str());
		item.lParam = idx;   // bitno za sort

		int row = ListView_InsertItem(hListView, &item);

		auto setDouble = [&](int col, double v, int prec = 4)
			{
				std::wstringstream ss;
				ss << std::fixed << std::setprecision(prec) << v;
				std::wstring tmp = ss.str();
				ListView_SetItemText(hListView, row, col, const_cast<LPWSTR>(tmp.c_str()));
			};

		auto setUInt64 = [&](int col, uint64_t v)
			{
				std::wstringstream ss;
				ss << v;
				std::wstring tmp = ss.str();
				ListView_SetItemText(hListView, row, col, const_cast<LPWSTR>(tmp.c_str()));
			};

		setDouble(1, load, 4);
		setDouble(2, scan, 4);
		setDouble(3, total, 4);
		setDouble(4, b.mbPerSec, 1);
		setUInt64(5, b.valuesPerSec);

		idx++;
	}

	for(int i = 0; i < 6; ++i)
		ListView_SetColumnWidth(hListView, i, LVSCW_AUTOSIZE_USEHEADER);
}