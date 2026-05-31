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

static std::string GetLowerExt(const std::string& path) 
{
	size_t pos = path.find_last_of('.');
	std::string ext = (pos != std::string::npos) ? path.substr(pos + 1) : "";
	for(auto& c : ext) 
		c = (char)tolower((unsigned char)c);
	return ext;
}

static bool RapidJSON_Count(const std::string& path, size_t& rows,size_t& fields, std::string* err = nullptr)
{
	using namespace rapidjson;

	std::ifstream ifs(path);
	if(!ifs) 
	{
		if(err) *err = "Cannot open file";
		return false;
	}
	IStreamWrapper isw(ifs);
	Document d;
	d.ParseStream(isw);

	if(d.HasParseError()) 
	{
		if(err)
		{
			*err = std::string(GetParseError_En(d.GetParseError())) +
				" (offset=" + std::to_string(d.GetErrorOffset()) + ")";
		}
		return false;
	}

	rows = 0;
	fields = 0;

	if(d.IsArray())
	{
		rows = d.Size();
		for(const auto& v : d.GetArray())
		{
			if(v.IsObject())
				fields += v.MemberCount();
			else
				fields += 1;
		}
	}
	else if(d.IsObject())
	{
		rows = 1;
		fields = d.MemberCount();
	}
	else
	{
		rows = 1;
		fields = 1;
	}
	return true;
}
static bool NlohmannJSON_Count(const std::string& path, size_t& rows, size_t& fields, std::string* err = nullptr)
{
	std::ifstream ifs(path);
	if(!ifs) 
	{
		if(err) *err = "Cannot open file";
		return false;
	}

	rows = 0;
	fields = 0;

	try
	{
		nlohmann::json j;
		ifs >> j;

		if(j.is_array())
		{
			rows = j.size();

			for(const auto& v : j)
			{
				if(v.is_object())
					fields += v.size();
				else
					fields += 1;
			}
		}
		else if(j.is_object())
		{
			rows = 1;
			fields = j.size();
		}
		else
		{
			rows = 1;
			fields = 1;
		}
	}
	catch(const std::exception& ex)
	{
		if(err)
			*err = ex.what();
		return false;
	}
	return true;
}

static void CountAllAttrs_Tiny(const tinyxml2::XMLElement* e, size_t& attrCount) 
{
	for(auto a = e->FirstAttribute(); a; a = a->Next()) 
		attrCount++;
	for(auto c = e->FirstChildElement(); c; c = c->NextSiblingElement())
		CountAllAttrs_Tiny(c, attrCount);
}

static void CountAllAttrs_Pugi(const pugi::xml_node& n, size_t& attrCount) 
{
	for(auto a : n.attributes()) 
		attrCount++;
	for(auto c : n.children()) 
		if(c.type() == pugi::node_element) 
			CountAllAttrs_Pugi(c, attrCount);
}

static void Tiny_CountAll(const tinyxml2::XMLElement* e, size_t& elemCount, size_t& attrCount)
{
	if(!e)
		return;
	elemCount++;

	for(auto a = e->FirstAttribute(); a; a = a->Next())
		attrCount++;

	for(auto c = e->FirstChildElement(); c; c = c->NextSiblingElement())
		Tiny_CountAll(c, elemCount, attrCount);
}

static void Tiny_CountAll_Text(const tinyxml2::XMLNode* n, size_t& elem, size_t& text)
{
	if(!n)
		return;
	if(n->ToElement())
		elem++;

	if(auto t = n->ToText())
	{
		const char* s = t->Value();
		if(s && *s)
			text++;
	}

	for(auto c = n->FirstChild(); c; c = c->NextSibling())
		Tiny_CountAll_Text(c, elem, text);
}

static bool TinyXML2_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
	tinyxml2::XMLDocument doc;
	if(doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
	{
		if(err) *err = "TinyXML2: cannot load file";
		return false;
	}

	size_t elem = 0, text = 0;
	Tiny_CountAll_Text(&doc, elem, text);

	rows = elem;
	cols = text;
	return elem > 0;
}

static void Pugi_CountAll(const pugi::xml_node& n, size_t& elemCount, size_t& attrCount)
{
	if(!n || n.type() != pugi::node_element)
		return;
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
	rows = elem;  
	cols = text;
	return elem > 0;
}

static bool CSV_Vincent_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
	rows = cols = 0;
	try 
	{
		csv::CSVFormat fmt; fmt.variable_columns(true).header_row(-1);
		csv::CSVReader reader(path, fmt);
		for(auto& r : reader) 
		{ 
			rows++; 
			cols = max(cols, (size_t)r.size()); 
		}
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

static bool XLSX_OpenXLSX_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
	rows = cols = 0;
	try 
	{
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
	catch(...) 
	{
		return false;
	}
}

static bool XLSX_xlnt_Count(const std::string& path, size_t& rows, size_t& cols, std::string* err = nullptr)
{
	try 
	{
		xlnt::workbook wb; wb.load(path);
		auto ws = wb.active_sheet();
		rows = ws.highest_row(); cols = ws.highest_column().index;
		return true;
	}
	catch(...)
	{
		return false;
	}
}

static bool HasEmoji(const std::string& s)
{
	for(unsigned char c : s)
		if((c & 0xF0) == 0xF0) return true;
	return false;
}

static void RE_AppendColored(HWND hRE, COLORREF color, bool bold, const std::string& text)
{
	CHARRANGE endSel{ -1, -1 };
	SendMessageW(hRE, EM_EXSETSEL, 0, (LPARAM)&endSel);

	bool hasEmoji = false;
	for(unsigned char c : text)
		if((c & 0xF0) == 0xF0)
		{
			hasEmoji = true;
			break;
		}

	const wchar_t* fontFace = hasEmoji ? L"Segoe UI Emoji" : L"Consolas";
	CHARFORMAT2W cf{};
	cf.cbSize = sizeof(cf);
	cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_FACE;
	cf.crTextColor = color;
	cf.wWeight = bold ? FW_BOLD : FW_NORMAL;
	wcscpy_s(cf.szFaceName, fontFace);
	SendMessageW(hRE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

	int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if(len <= 1) 
		return;

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
		hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
			40, 180, 900, 480, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

		ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
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
		break;
	}

	case WM_ERASEBKGND:
	{
		RECT rc; GetClientRect(hwnd, &rc);
		HDC hdc = (HDC)wParam;

		TRIVERTEX vertex[2] = {{0, 0, 20000, 26000, 65535, 0x0000},
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
			if(sortCol == p->iSubItem) 
				asc = !asc;
			else
			{
				sortCol = p->iSubItem;
				asc = true;
			}
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
						ListView_GetItemText(hListView, i, 3, buf, 64);
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

			TRIVERTEX v[2] = {{rc.left, rc.top, GetRValue(start) << 8, GetGValue(start) << 8, GetBValue(start) << 8, 0},
				{rc.right, rc.bottom, GetRValue(end) << 8, GetGValue(end) << 8, GetBValue(end) << 8, 0}
			};

			GRADIENT_RECT gr = { 0, 1 };
			GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, RGB(255, 255, 255));
			DrawTextW(hdc, L"Open File", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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
				ListView_DeleteAllItems(hListView);
				g_bestTotal = -1.0;
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
		if(hBgBrush)
			DeleteObject(hBgBrush);
		if(hMonoFont)
			DeleteObject(hMonoFont);
		if(hWhiteBrush)
			DeleteObject(hWhiteBrush);
		PostQuitMessage(0);
		break;
	}
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

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
		WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720, nullptr, nullptr, hInst, nullptr);
	hButtonParse = CreateWindowEx(0, L"BUTTON", L"📂  Open File & Benchmark", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
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
	while(GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return (int)msg.wParam;
}

static void PrintFirst50_Tiny(const std::string& path, int ns)
{
	tinyxml2::XMLDocument doc;
	if(doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
		return;

	RE_AppendLine(TextParserUI::hEditOutput, RGB(180, 120, 0), true, "TinyXML2 (first n elements):");
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

static void PrintFirst50_Pugi(const std::string& path, int ns)
{
	pugi::xml_document doc;
	if(!doc.load_file(path.c_str()))
		return;

	RE_AppendLine(TextParserUI::hEditOutput, RGB(0, 120, 200), true, "PugiXML (first n elements):");
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

static double Measure(std::function<void()> fn)
{
	Timer t; t.start();
	fn();
	t.end();
	return t.seconds();
}

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

inline double safeTime(double t)
{
	return (t < 1e-6) ? 1e-6 : t;
}

inline std::string fmtTime(double v)
{
	std::ostringstream ss;
	if(v < 0.001)
		ss << std::fixed << std::setprecision(6) << v;
	else if(v < 0.1)
		ss << std::fixed << std::setprecision(4) << v;
	else
		ss << std::fixed << std::setprecision(3) << v;
	return ss.str();
}

inline int getRepeat(double fileMB)
{
	//if(fileMB < 0.1) return 200;
	//if(fileMB < 1.0) return 50;
	//if(fileMB < 5.0) return 10;
	return 1;
}
#if TEST
void testJSON(const JSONParser& j)
{
	JSONValue root(&j, j.rootIndex());
	std::vector<std::string> strings;
	std::vector<double> numbers;
	std::vector<bool> bools;
	std::vector<size_t> arraySizes;
	std::vector<std::string> keys;
	std::function<void(JSONValue)> walk;

	walk =
		[&](JSONValue v)
		{
			if(!v.valid())
				return;

			if(v.isString())
			{
				strings.emplace_back(
					v.asStringView());
			}
			else if(v.isNumber())
			{
				numbers.push_back(
					v.asDouble());
			}
			else if(v.isBool())
			{
				bools.push_back(
					v.asBool());
			}
			else if(v.isArray())
			{
				arraySizes.push_back(
					v.size());

				for(size_t i = 0; i < v.size();	++i)
				{
					walk(v[i]);
				}
			}
			else if(v.isObject())
			{
				const auto& node = j.nodes()[v.index()];
				for(size_t i = 0; i < v.size();++i)
				{
					const auto& kv = j.membersArena()[node.a +(uint32_t)i];
					keys.emplace_back(kv.first);
					auto childIdx = kv.second;

					walk(JSONValue(&j, childIdx));
				}
			}
		};
	walk(root);
	std::vector<std::string> directStrings;
    std::vector<double> directNumbers;
    std::vector<bool> directBools;

    // json["users"]
    auto users = root["users"];
	if(!users.valid())
		return;
    if(users.isArray())
    {
        arraySizes.push_back(users.size()); 

        for(size_t i = 0; i < users.size(); ++i)
        {
            auto user = users[i];

            auto name = user["name"];
            if(name.isString())
                directStrings.emplace_back(name.asStringView());

            auto id = user["id"];
            if(id.isNumber())
                directNumbers.push_back(id.asDouble());

            auto active = user["active"];
            if(active.isBool())
                directBools.push_back(active.asBool());

            auto scores = user["scores"];
            if(scores.isArray())
            {
                arraySizes.push_back(scores.size());

                for(size_t j2 = 0; j2 < scores.size(); ++j2)
                {
                    auto sc = scores[j2];
                    if(sc.isNumber())
                        directNumbers.push_back(sc.asDouble());
                }
            }
        }
    }

}

#if TEST
void testXML(const XMLParser& x)
{
	auto root = x.rootValue();

	if(!root.valid())
		return;

	std::vector<std::string> texts;
	std::vector<std::string> names;
	std::vector<size_t> childCounts;
	auto walk = [&](auto&& self, XMLValue v) -> void
		{
			if(!v.valid())
				return;
			auto n = v.name();
			if(!n.empty())
				names.emplace_back(n);
			if(v.isText())
			{
				auto txt = v.text();
				if(!txt.empty())
					texts.emplace_back(txt);
			}

			size_t childCount = 0;
			for(auto c = v.firstChild(); c.valid(); c = c.nextSibling())
			{
				++childCount;
				self(self, c);
			}

			childCounts.push_back(childCount);
		};

	walk(walk, root);

	std::vector<std::string> userNames;
	std::vector<int> ids;
	std::vector<bool> actives;
	std::vector<double> salaries;
	std::vector<Date> birthDates;
	std::vector<int> scores;

	auto doc = x.rootValue();
	auto r = doc.child("root");
	auto users = r.child("users");
	if(users.valid())
	{
		for(auto user : users.children("user"))
		{
			auto name = user.childText("name");
			if(!name.empty())
				userNames.emplace_back(name);
			auto idNode = user.child("id").firstChild();

			if(auto v = idNode.toInt())
				ids.push_back(*v);
			auto activeNode = user.child("active").firstChild();

			if(auto v = activeNode.toBool())
				actives.push_back(*v);

			auto salaryNode = user.child("salary").firstChild();

			if(auto v = salaryNode.toDouble())
				salaries.push_back(*v);
			auto birthNode = user.child("birthDate").firstChild();

			if(auto d = birthNode.toDate())
				birthDates.push_back(*d);
			auto scoresNode = user.child("scores");

			for(auto score : scoresNode.children("score"))
			{
				auto txt = score.firstChild();
				if(auto s = txt.toInt())
					scores.push_back(*s);
			}
		}
	}
}
#endif
#endif

void TextParserUI::ParseFile(const std::string& filepath)
{
	const std::string ext = GetLowerExt(filepath);
	std::vector<BenchLine> lines;
	double fileMB = 0.0;
	try 
	{
		fileMB = std::filesystem::file_size(filepath) / (1024.0 * 1024.0);
	}
	catch(...) {}
	std::string fileContent;
	{
		std::ifstream f(filepath, std::ios::binary);
		fileContent.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	}
	int repeat = getRepeat(fileMB);
	if(ext == "csv")
	{
		// ===== Custom CSV =====
		lines.push_back([&]()
			{
				BenchLine b; 
				b.name = "Custom CSV";

				CSVParser::Options opt;
				opt.delimiter = ',';
				opt.hasHeader = true;
				opt.allowQuotes = false;
				opt.useMMap = true;

				CSVParser csv(filepath, opt);

				Timer t; t.start();
				b.ok = csv.load();
				t.end();
				b.loadSecs = t.seconds();
				if(!b.ok)
					return b;
				size_t r = csv.rowCount();
				size_t c = csv.colCount();
				size_t values = r * c;
#if TEST
				std::vector<string_view> aStr;
				std::vector<TextFileParser::CellKind> aCellKind;
				std::vector<double> aDbl;
				std::vector<bool> aBool;
				std::vector<string_view> aStrSingle;
				std::vector<Date> aDate;
				auto names = csv.getColNames();
				for(int i = 0; i < r; ++i)
				{
					for(int j = 0; j < c; ++j)
					{
						std::string_view str = csv.valueView(i, j);
						aStr.push_back(csv.valueView(i, j));
						auto cellKind = csv.cellKind(i, j);
						aCellKind.push_back(csv.cellKind(i, j));
						if(cellKind == TextFileParser::CellKind::CK_Date)
						{
							Date date;
							parseDate(str, date);
							aDate.push_back(date);
						}
						else if(cellKind == TextFileParser::CellKind::CK_Number)
						{
							aDbl.push_back(csv.toDouble(i, j).value());
						}
						else if(cellKind == TextFileParser::CellKind::CK_String)
						{
							aStrSingle.push_back(str);
						}
						else if(cellKind == TextFileParser::CellKind::CK_Bool)
						{
							aBool.push_back(csv.toBool(i, j).value());
						}
					}
				}
#endif
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
				BenchLine b;
				b.name = "rapidcsv";

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
				BenchLine b;
				b.name = "Custom JSON";

				JSONParser j(filepath);

				Timer t;
				t.start();
				b.ok = j.load();
				t.end();

				b.loadSecs = t.seconds();

				if(!b.ok)
					return b;

				const auto& nodes = j.nodes();

				// =========================
				// COUNT VALUES
				// =========================
				size_t values = 0;

				for(size_t i = 0; i < nodes.size(); ++i)
				{
					JSONValue v(&j, i);

					if(v.isString() || v.isNumber() || v.isBool() || v.isNull())
						++values;
				}

#if TEST
				testJSON(j);
#endif
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						const auto& nodes = j.nodes();
						for(int rep = 0; rep < repeat;++rep)
						{
							for(const auto& n : nodes)
							{
								switch((JSONType)n.type)
								{
								case JSONType::String:
								{
									sink +=	n.str.size();
									break;
								}

								case JSONType::Number:
								{
									sink += n.raw.size();
									break;
								}

								case JSONType::Bool:
								{
									sink += n.boolVal;
									break;
								}

								case JSONType::Null:
								{
									sink += 1;
									break;
								}

								default:
									break;
								}
							}
						}
					})
					/ repeat;

				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		lines.push_back([&]()
			{
				BenchLine b;
				b.name = "RapidJSON";

				rapidjson::Document doc;

				Timer t;
				t.start();
				doc.Parse(fileContent.c_str());
				t.end();

				b.loadSecs = t.seconds();

				if(doc.HasParseError())
					return b;

				size_t values = 0;
				std::function<void(const rapidjson::Value&)> countWalk;
				countWalk = [&](const rapidjson::Value& v)
					{
						if(v.IsString() || v.IsNumber() || v.IsBool() || v.IsNull())
						{
							++values;
							return;
						}

						if(v.IsArray())
						{
							for(auto& x : v.GetArray())
								countWalk(x);
						}
						else if(v.IsObject())
						{
							for(auto& m : v.GetObject())
								countWalk(m.value);
						}
					};

				countWalk(doc);

				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						std::function<void(const rapidjson::Value&)> scanWalk;
						scanWalk = [&](const rapidjson::Value& v)
							{
								if(v.IsString())
								{
									sink += v.GetStringLength();
								}
								else if(v.IsNumber())
								{
									sink += (size_t)v.GetDouble();
								}
								else if(v.IsBool())
								{
									sink += v.GetBool();
								}
								else if(v.IsNull())
								{
									sink += 1;
								}
								else if(v.IsArray())
								{
									for(auto& x : v.GetArray())
										scanWalk(x);
								}
								else if(v.IsObject())
								{
									for(auto& m : v.GetObject())
										scanWalk(m.value);
								}
							};

						for(int rep = 0; rep < repeat; ++rep)
							scanWalk(doc);

					}) / repeat;

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());
		lines.push_back([&]()
			{
				BenchLine b;
				b.name = "nlohmann/json";

				Timer t;
				t.start();
				nlohmann::json j = nlohmann::json::parse(fileContent);
				t.end();

				b.loadSecs = t.seconds();
				size_t values = 0;
				std::function<void(const nlohmann::json&)> countWalk;
				countWalk = [&](const nlohmann::json& v)
					{
						if(v.is_string() || v.is_number() || v.is_boolean() || v.is_null())
						{
							++values;
							return;
						}

						if(v.is_array())
						{
							for(const auto& x : v)
								countWalk(x);
						}
						else if(v.is_object())
						{
							for(auto& x : v.items())
								countWalk(x.value());
						}
					};

				countWalk(j);
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						std::function<void(const nlohmann::json&)> scanWalk;
						scanWalk = [&](const nlohmann::json& v)
							{
								if(v.is_string())
								{
									sink +=	v.get_ref<const std::string&>().size();
								}
								else if(v.is_number())
								{
									sink += (size_t)v.get<double>();
								}
								else if(v.is_boolean())
								{
									sink += v.get<bool>();
								}
								else if(v.is_null())
								{
									sink += 1;
								}
								else if(v.is_array())
								{
									for(const auto& x : v)
										scanWalk(x);
								}
								else if(v.is_object())
								{
									for(auto& x : v.items())
										scanWalk(x.value());
								}
							};

						for(int rep = 0; rep < repeat; ++rep)
							scanWalk(j);

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
				BenchLine b; 
				b.name = "Custom XML";
				XMLParser::Options opt;
				opt.usePointerDom = true;   
				XMLParser x(filepath, opt);
				Timer t;
				t.start();
				b.ok = x.load();
				t.end();

				b.loadSecs = t.seconds();
#if TEST
				testXML(x);
#endif
				if(!b.ok)
					return b;
				size_t values = 0;
				const auto& nodes = x.nodesP();
				for(const auto& n : nodes)
				{
					if(n.text)
						++values;
				}
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
						{
							for(const auto& n : nodes)
							{
								if(n.text)
									sink += n.textLen;
							}
						}
					}) / repeat;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);

				return b;
			}());

		// TinyXML2
		lines.push_back([&]()
			{
				BenchLine b;
				b.name = "tinyxml2";
				tinyxml2::XMLDocument doc;

				Timer t; t.start();
				b.ok = doc.Parse(fileContent.c_str()) == tinyxml2::XML_SUCCESS;
				t.end();
				b.loadSecs = t.seconds();
				if(!b.ok)
					return b;
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
				BenchLine b;
				b.name = "pugixml";
				pugi::xml_document doc;
				Timer t; t.start();
				b.ok = doc.load_buffer(fileContent.data(), fileContent.size());
				t.end(); b.loadSecs = t.seconds();
				if(!b.ok) 
					return b;
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
				BenchLine b;
				b.name = "Custom XLSX";
				XLSXParser x(filepath);
				Timer t; t.start();
				b.ok = x.load();
				t.end();
				b.loadSecs = t.seconds();
				if(!b.ok)
					return b;

				size_t r = x.rowCount();
				size_t c = x.colCount();
				size_t values = r * c;
#if TEST
				std::vector<string_view> aStr;
				std::vector<TextFileParser::CellKind> aCellKind;
				std::vector<double> aDbl;
				std::vector<bool> aBool;
				std::vector<string_view> aStrSingle;
				std::vector<Date> aDate;
				auto names = x.getColNames();
				for(int i = 0; i < r; ++i)
				{
					for(int j = 0; j < c; ++j)
					{
						std::string_view str = x.valueView(i, j);
						aStr.push_back(x.valueView(i, j));
						auto cellKind = x.cellKind(i, j);
						aCellKind.push_back(x.cellKind(i, j));
						if(cellKind == TextFileParser::CellKind::CK_Date)
						{
							Date date;
							parseDate(str, date);
							aDate.push_back(date);
						}
						else if(cellKind == TextFileParser::CellKind::CK_Number)
						{
							aDbl.push_back(x.toDouble(i, j).value());
						}
						else if(cellKind == TextFileParser::CellKind::CK_String)
						{
							aStrSingle.push_back(str);
						}
						else if(cellKind == TextFileParser::CellKind::CK_Bool)
						{
							aBool.push_back(x.toBool(i, j).value());
						}
					}
				}
#endif
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
				BenchLine b;
				b.name = "OpenXLSX";
				using namespace OpenXLSX;
				Timer t;
				t.start();

				XLDocument doc;
				doc.open(filepath);
				auto ws = doc.workbook().worksheet(doc.workbook().worksheetNames()[0]);
				t.end();
				b.loadSecs = t.seconds();
				size_t values = 0;

				for(auto& row :	ws.rows())
				{
					for(auto& cell : row.cells())
					{
						try
						{
							if(cell.empty())
								continue;
							auto& value = cell.value();
							switch(value.type())
							{
							case XLValueType::String:
							{
								auto s = cell.value().get<std::string>();
								if(!s.empty())
								{
									++values;
								}
								break;
							}

							case XLValueType::Integer:
							case XLValueType::Float:
							case XLValueType::Boolean:
							{
								++values;
								break;
							}

							default:
								break;
							}
						}
						catch(...)
						{
						}
					}
				}
				b.scanSecs = Measure([&]()
						{
							volatile size_t sink = 0;
							for(int rep = 0; rep < repeat; ++rep)
							{
								for(auto& row : ws.rows())
								{
									for(auto& cell : row.cells())
									{
										try
										{
											if(cell.empty())
												continue;
											auto& value = cell.value();
											switch(value.type())
											{
											case XLValueType::String:
											{
												auto s = cell.value().get<std::string>();
												sink += s.size();
												break;
											}

											case XLValueType::Integer:
											{
												sink += 8;
												break;
											}

											case XLValueType::Float:
											{
												sink += 8;
												break;
											}

											case XLValueType::Boolean:
											{
												sink += 1;
												break;
											}

											default:
												break;
											}
										}
										catch(...)
										{
										}
									}
								}
							}
						})
					/ repeat;

				doc.close();

				b.ok = true;
				b.valuesPerSec = values / safeTime(b.scanSecs);
				b.mbPerSec = fileMB / safeTime(b.loadSecs + b.scanSecs);
				return b;
			}());

		// xlnt
		lines.push_back([&]()
			{
				BenchLine b;
				b.name = "xlnt";
				Timer t;
				t.start();

				xlnt::workbook wb;
				wb.load(filepath);
				auto ws = wb.active_sheet();
				t.end();
				b.loadSecs = t.seconds();
				size_t values = 0;
				for(auto row : ws.rows(false))
				{
					for(auto cell : row)
					{
						try
						{
							auto v = cell.to_string();
							if(!v.empty())
								++values;
						}
						catch(...)
						{
						}
					}
				}
				b.scanSecs = Measure([&]()
					{
						volatile size_t sink = 0;
						for(int rep = 0; rep < repeat; ++rep)
						{
							for(auto row : ws.rows(false))
							{
								for(auto cell : row)
								{
									try
									{
										auto v = cell.to_string();
										sink += v.size();
									}
									catch(...)
									{
									}
								}
							}
						}
					}) / repeat;

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
		item.lParam = idx; 
		int row = ListView_InsertItem(hListView, &item);
		auto setDouble = [&](int col, double v, int prec = 4)
			{
				std::wstringstream ss;
				ss << std::fixed << std::setprecision(prec) << v;
				std::wstring tmp = ss.str();
				ListView_SetItemText(hListView, row, col, const_cast<LPWSTR>(tmp.c_str()));
			};

	/*	auto setUInt64 = [&](int col, uint64_t v)
			{
				std::wstringstream ss;
				ss << v;
				std::wstring tmp = ss.str();
				ListView_SetItemText(hListView, row, col, const_cast<LPWSTR>(tmp.c_str()));
			};*/

		setDouble(1, load, 4);
		setDouble(2, scan, 4);
		setDouble(3, total, 4);
		setDouble(4, b.mbPerSec, 3);
		//setUInt64(5, b.valuesPerSec);
		idx++;
	}

	for(int i = 0; i < 6; ++i)
		ListView_SetColumnWidth(hListView, i, LVSCW_AUTOSIZE_USEHEADER);
}