// TextParser.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <string>
#include <iomanip>
#include "Timer.h"
#include "CSVParser.h"  
#include "JSONParser.h" 
#include "XMLParser.h"  
#include "XLSXParser.h" 
#include "TextFileParser.h"
#include "TextUIParser.h"
using namespace std;

namespace large_file_io
{

    void testJSON()
    {
        cout << "\n[ JSON TEST ]\n";
        Timer timer;
        timer.start();

        JSONParser json("JSON Files/weather-parquet.json");
        if(!json.load()) {
            cerr << "Greska pri citanju JSON fajla!\n";
            return;
        }

        timer.end();
        cout << "JSON fajl ucitan za " << timer.seconds() << " sekundi.\n";
        cout << "Redova: " << json.rowCount() << ", Kolona: " << json.colCount() << endl;

        if(json.rowCount() > 0)
            cout << "Prva vrijednost: " << json.value(0, 0) << endl;
    }

    void testXML()
    {
        cout << "\n[ XML TEST ]\n";
        Timer timer;
        timer.start();

        XMLParser xml("XML Files/testdata.xml");
        if(!xml.load()) {
            cerr << "Greska pri citanju XML fajla!\n";
            return;
        }

        timer.end();
        cout << "XML fajl ucitan za " << timer.seconds() << " sekundi.\n";
        cout << "Redova: " << xml.rowCount() << ", Kolona: " << xml.colCount() << endl;

        if(xml.rowCount() > 0)
            cout << "Prva vrijednost: " << xml.value(0, 0) << endl;
    }

    void testXLSX()
    {
        cout << "\n[ XLSX TEST ]\n";
        Timer timer;
        timer.start();

        XLSXParser xlsx("XLSX Files/sheet1.xml");
        if(!xlsx.load()) {
            cerr << "Greska pri citanju XLSX fajla (XML verzija)!\n";
            return;
        }

        timer.end();
        cout << "XLSX (sheet1.xml) fajl ucitan za " << timer.seconds() << " sekundi.\n";
        cout << "Redova: " << xlsx.rowCount() << ", Kolona: " << xlsx.colCount() << endl;

        if(xlsx.rowCount() > 0)
            cout << "Prva vrijednost: " << xlsx.getColName(1) << endl;
    }
}

int main()
{
    cout << "=== Large File Parsing Demo ===\n";
    cout << "Metodologija i alati za parsiranje velikih tekstualnih fajlova\n";
    cout << "------------------------------------------\n";
    {
        //cout << "Pokrećem grafički interfejs...\n";
        HINSTANCE hInst = GetModuleHandle(nullptr);
        TextParserUI::Run(hInst, SW_SHOW);
    }
    //else
    //{
    //    cout << "Nepoznata opcija. Izlaz.\n";
    //}

    return 0;
}
//int main()
//{
//	//readInfo();
//	//readGlobalTemperatures();
//        // Putanja do CSV fajla
//// Primer CSV fajl
//    readGlobalTemperatures();
//    Timer timer;
//    timer.start();
//    std::string filename = "GlobalTemperatures.csv";
//    
//    CSVFReader csv(filename , ',', -1, -1);
//    if (!csv.load()) 
//    {
//        std::cout << "Failed to load file\n";
//        return 1;
//    }
//
//    size_t rows = csv.getRowCount();
//    size_t cols = csv.getColCount();
//
//    //std::cout << "Rows: " << rows << ", Cols: " << cols << "\n";
//
//    timer.end();
//    std::cout << "Vrijeme koristeci licni csv je: " << timer.seconds() << std::endl;
//    return 0;
//	//CCsvInfo csv("GlobalTemperatures.csv");
//	//csv.ReadCsv();
//	//auto s = csv.GetColumnNames();
//	//auto b = csv.GetColumns();
//	//int k = 0;
//}
