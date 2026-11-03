/*
 * TheoryCraftOS's own checks of comdlg32.dll, in the style of Wine's tests (and run with them: scripts/winetest.sh comdlg32 tcos). Wine's tests of this DLL need real windows
 * (hook procedures, child windows of the dialogs); these do not: they check what comdlg32 does with the program's structures - the error codes, GetFileTitle, the answers when
 * the machine shows no dialog. Written for this project (GNU AGPL).
 */
#include "windows.h"
#include "commdlg.h"
#include "cderr.h"
#include "wine/test.h"

static UINT_PTR CALLBACK dummy_hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) { return 0; }

static void test_file_title(void)
{
    char title[MAX_PATH];
    WCHAR wtitle[MAX_PATH];
    short ret;

    ret = GetFileTitleA("C:\\dir\\sub\\file.txt", title, sizeof(title));
    ok(ret == 0 && !strcmp(title, "file.txt"), "got %d '%s'\n", ret, title);
    ret = GetFileTitleA("file.txt", title, sizeof(title));
    ok(ret == 0 && !strcmp(title, "file.txt"), "got %d '%s'\n", ret, title);
    ret = GetFileTitleA("C:file.txt", title, sizeof(title));
    ok(ret == 0 && !strcmp(title, "file.txt"), "got %d '%s'\n", ret, title);
    ret = GetFileTitleA("dir/file", title, sizeof(title));
    ok(ret == 0 && !strcmp(title, "file"), "got %d '%s'\n", ret, title);
    ret = GetFileTitleA("C:\\dir\\file.txt", title, 5);
    ok(ret == 9, "a small buffer: got %d (expected the 9 it needs)\n", ret);
    ret = GetFileTitleA("C:\\dir\\file.txt", NULL, 0);
    ok(ret == 9, "no buffer: got %d\n", ret);
    ret = GetFileTitleA(NULL, title, sizeof(title));
    ok(ret == -1, "NULL name: got %d\n", ret);
    ret = GetFileTitleA("", title, sizeof(title));
    ok(ret == -1, "empty name: got %d\n", ret);
    ret = GetFileTitleA("C:\\dir\\", title, sizeof(title));
    ok(ret == -1, "a folder: got %d\n", ret);
    ret = GetFileTitleA("C:", title, sizeof(title));
    ok(ret == -1, "a drive: got %d\n", ret);
    ret = GetFileTitleA("C:\\dir\\*.txt", title, sizeof(title));
    ok(ret == -1, "a wildcard: got %d\n", ret);
    ret = GetFileTitleA("C:\\dir\\[a]", title, sizeof(title));
    ok(ret == -1, "brackets: got %d\n", ret);

    ret = GetFileTitleW(L"C:\\dir\\sub\\file.txt", wtitle, MAX_PATH);
    ok(ret == 0 && !lstrcmpW(wtitle, L"file.txt"), "wide: got %d\n", ret);
    ret = GetFileTitleW(L"C:\\dir\\file.txt", wtitle, 8);
    ok(ret == 9, "wide, small buffer: got %d\n", ret);
    ret = GetFileTitleW(L"C:\\dir\\file.txt", wtitle, 9);
    ok(ret == 0 && !lstrcmpW(wtitle, L"file.txt"), "wide, exact buffer: got %d\n", ret);
    ret = GetFileTitleW(NULL, wtitle, MAX_PATH);
    ok(ret == -1, "wide NULL: got %d\n", ret);
}

static void test_file_dialog_params(void)
{
    OPENFILENAMEA ofn;
    OPENFILENAMEW ofnw;
    char file[MAX_PATH] = "";
    WCHAR wfile[MAX_PATH] = L"";
    BOOL ret;


    ret = GetOpenFileNameA(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "NULL: %d %#lx\n", ret, CommDlgExtendedError());
    ret = GetSaveFileNameW(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "NULL, wide: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn) - 1;
    ret = GetOpenFileNameA(&ofn);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size: %d %#lx\n", ret, CommDlgExtendedError());
    ofn.lStructSize = sizeof(ofn) + 8;
    ret = GetSaveFileNameA(&ofn);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size, save: %d %#lx\n", ret, CommDlgExtendedError());

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_ENABLEHOOK;
    ret = GetOpenFileNameA(&ofn);
    ok(!ret && CommDlgExtendedError() == CDERR_NOHOOK, "hook without a function: %d %#lx\n", ret, CommDlgExtendedError());
    ofn.Flags = 0;

    /* the old (version 4.00) structure is accepted too; with no screen to show the dialog on the call fails with CDERR_DIALOGFAILURE and does not touch the buffer */
    strcpy(file, "name.txt");
    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
    ret = GetOpenFileNameA(&ofn);
    ok(!ret, "got %d\n", ret);
    ok(CommDlgExtendedError() == CDERR_DIALOGFAILURE || CommDlgExtendedError() == 0, "dialog: %#lx\n", CommDlgExtendedError());
    ok(!strcmp(file, "name.txt"), "buffer changed to '%s'\n", file);

    memset(&ofnw, 0, sizeof(ofnw));
    ofnw.lStructSize = sizeof(ofnw);
    ofnw.lpstrFile = wfile;
    ofnw.nMaxFile = MAX_PATH;
    ofnw.lpstrFilter = L"Text\0*.txt\0All\0*.*\0";
    ofnw.lpstrDefExt = L"txt";
    ret = GetSaveFileNameW(&ofnw);
    ok(!ret, "got %d\n", ret);
    ok(CommDlgExtendedError() == CDERR_DIALOGFAILURE || CommDlgExtendedError() == 0, "wide dialog: %#lx\n", CommDlgExtendedError());

    /* the code belongs to the last call: a good call resets it */
    GetOpenFileNameA(NULL);
    ok(CommDlgExtendedError() == CDERR_INITIALIZATION, "after NULL: %#lx\n", CommDlgExtendedError());
    ofn.lStructSize = sizeof(ofn);
    GetOpenFileNameA(&ofn);
    ok(CommDlgExtendedError() != CDERR_INITIALIZATION, "the error was not cleared: %#lx\n", CommDlgExtendedError());
}

static void test_color(void)
{
    CHOOSECOLORA cc;
    CHOOSECOLORW ccw;
    COLORREF custom[16] = {0};
    BOOL ret;

    ret = ChooseColorA(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "NULL: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc) - 1;
    cc.lpCustColors = custom;
    ret = ChooseColorA(&cc);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size: %d %#lx\n", ret, CommDlgExtendedError());

    cc.lStructSize = sizeof(cc);
    cc.lpCustColors = NULL;
    ret = ChooseColorA(&cc);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "no custom colors: %d %#lx\n", ret, CommDlgExtendedError());

    cc.lpCustColors = custom;
    cc.Flags = CC_ENABLEHOOK;
    ret = ChooseColorA(&cc);
    ok(!ret && CommDlgExtendedError() == CDERR_NOHOOK, "hook: %d %#lx\n", ret, CommDlgExtendedError());

    cc.Flags = CC_RGBINIT;
    cc.rgbResult = RGB(1, 2, 3);
    ret = ChooseColorA(&cc);
    ok(!ret, "got %d\n", ret);
    ok(CommDlgExtendedError() == CDERR_DIALOGFAILURE || CommDlgExtendedError() == 0, "dialog: %#lx\n", CommDlgExtendedError());
    ok(cc.rgbResult == RGB(1, 2, 3), "the colour changed to %#lx\n", cc.rgbResult);

    memset(&ccw, 0, sizeof(ccw));
    ccw.lStructSize = sizeof(ccw);
    ccw.lpCustColors = custom;
    ccw.Flags = CC_ENABLEHOOK;
    ccw.lpfnHook = dummy_hook;
    ret = ChooseColorW(&ccw);
    ok(!ret && CommDlgExtendedError() != CDERR_NOHOOK, "wide with a hook: %d %#lx\n", ret, CommDlgExtendedError());
}

static void test_font(void)
{
    CHOOSEFONTA cf;
    CHOOSEFONTW cfw;
    LOGFONTA lf;
    LOGFONTW lfw;
    BOOL ret;

    ret = ChooseFontA(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "NULL: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&cf, 0, sizeof(cf));
    memset(&lf, 0, sizeof(lf));
    cf.lStructSize = sizeof(cf) - 1;
    cf.lpLogFont = &lf;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size: %d %#lx\n", ret, CommDlgExtendedError());

    cf.lStructSize = sizeof(cf);
    cf.lpLogFont = NULL;
    cf.Flags = CF_SCREENFONTS;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "no LOGFONT: %d %#lx\n", ret, CommDlgExtendedError());

    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_ENABLEHOOK;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CDERR_NOHOOK, "hook: %d %#lx\n", ret, CommDlgExtendedError());

    cf.Flags = CF_SCREENFONTS | CF_ENABLETEMPLATE;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CDERR_NOTEMPLATE, "template: %d %#lx\n", ret, CommDlgExtendedError());

    cf.Flags = CF_SCREENFONTS | CF_ENABLETEMPLATEHANDLE;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CDERR_NOHINSTANCE, "template handle: %d %#lx\n", ret, CommDlgExtendedError());

    cf.Flags = 0;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CFERR_NOFONTS, "no kind of font: %d %#lx\n", ret, CommDlgExtendedError());

    cf.Flags = CF_SCREENFONTS | CF_LIMITSIZE;
    cf.nSizeMin = 20;
    cf.nSizeMax = 10;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CFERR_MAXLESSTHANMIN, "max < min: %d %#lx\n", ret, CommDlgExtendedError());

    cf.Flags = CF_SCREENFONTS;
    ret = ChooseFontA(&cf);
    ok(!ret && CommDlgExtendedError() == CFERR_NOFONTS, "no fonts: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&cfw, 0, sizeof(cfw));
    memset(&lfw, 0, sizeof(lfw));
    cfw.lStructSize = sizeof(cfw);
    cfw.lpLogFont = &lfw;
    cfw.Flags = CF_PRINTERFONTS;
    ret = ChooseFontW(&cfw);
    ok(!ret && CommDlgExtendedError() == CFERR_NOFONTS, "wide: %d %#lx\n", ret, CommDlgExtendedError());
}

#define FIND_FAILS(function, fr, code) do { \
        HWND h = function(fr); \
        ok(h == NULL && CommDlgExtendedError() == (code), #function ": %p %#lx (expected %#x)\n", h, CommDlgExtendedError(), (unsigned)(code)); \
    } while (0)

static void test_find_replace(void)
{
    FINDREPLACEA fr;
    char find[32] = "abc", replace[32] = "def";

    FIND_FAILS(FindTextA, NULL, CDERR_INITIALIZATION);
    FIND_FAILS(ReplaceTextA, NULL, CDERR_INITIALIZATION);

    memset(&fr, 0, sizeof(fr));
    FIND_FAILS(FindTextA, &fr, CDERR_STRUCTSIZE);
    FIND_FAILS(ReplaceTextW, (FINDREPLACEW *)&fr, CDERR_STRUCTSIZE);
    fr.lStructSize = sizeof(fr);

    FIND_FAILS(FindTextA, &fr, CDERR_DIALOGFAILURE);      /* no owner window */
    fr.hwndOwner = (HWND)(ULONG_PTR)0x1234;

    FIND_FAILS(FindTextA, &fr, FRERR_BUFFERLENGTHZERO);
    fr.wFindWhatLen = sizeof(find);
    FIND_FAILS(FindTextA, &fr, FRERR_BUFFERLENGTHZERO);
    fr.lpstrFindWhat = find;

    FIND_FAILS(ReplaceTextA, &fr, FRERR_BUFFERLENGTHZERO);
    fr.lpstrReplaceWith = replace;

    fr.Flags = FR_ENABLEHOOK;
    FIND_FAILS(FindTextA, &fr, CDERR_NOHOOK);
    FIND_FAILS(ReplaceTextA, &fr, CDERR_NOHOOK);
    fr.Flags = FR_ENABLETEMPLATEHANDLE;
    FIND_FAILS(FindTextA, &fr, CDERR_NOHINSTANCE);
    fr.Flags = FR_ENABLETEMPLATE;
    FIND_FAILS(FindTextA, &fr, CDERR_FINDRESFAILURE);
    fr.hInstance = GetModuleHandleA(NULL);
    FIND_FAILS(FindTextA, &fr, CDERR_FINDRESFAILURE);
    fr.Flags = 0;
    fr.hInstance = NULL;

    /* all the parameters are good: the modeless dialog needs a window system */
    FIND_FAILS(FindTextA, &fr, CDERR_DIALOGFAILURE);
    FIND_FAILS(ReplaceTextA, &fr, CDERR_DIALOGFAILURE);
}

static void test_print(void)
{
    PRINTDLGA pd;
    PRINTDLGW pdw;
    PAGESETUPDLGA psd;
    PRINTDLGEXW ex;
    PRINTPAGERANGE range[2];
    BOOL ret;
    HRESULT hr;

    ret = PrintDlgA(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "NULL: %d %#lx\n", ret, CommDlgExtendedError());
    ret = PageSetupDlgW(NULL);
    ok(!ret && CommDlgExtendedError() == CDERR_INITIALIZATION, "page setup NULL: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd) - 1;
    ret = PrintDlgA(&pd);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size: %d %#lx\n", ret, CommDlgExtendedError());
    pd.lStructSize = sizeof(pd) + 1;
    pd.Flags = PD_RETURNDEFAULT;
    ret = PrintDlgA(&pd);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "size + 1: %d %#lx\n", ret, CommDlgExtendedError());
    pd.lStructSize = sizeof(pd);
    ret = PrintDlgA(&pd);
    ok(!ret && CommDlgExtendedError() == PDERR_NODEFAULTPRN, "no printer: %d %#lx\n", ret, CommDlgExtendedError());
    ok(!pd.hDevMode && !pd.hDevNames, "got %p %p\n", pd.hDevMode, pd.hDevNames);
    memset(&pdw, 0, sizeof(pdw));
    pdw.lStructSize = sizeof(pdw);
    ret = PrintDlgW(&pdw);
    ok(!ret && CommDlgExtendedError() == PDERR_NODEFAULTPRN, "wide, no printer: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&psd, 0, sizeof(psd));
    psd.lStructSize = sizeof(psd) - 1;
    ret = PageSetupDlgA(&psd);
    ok(!ret && CommDlgExtendedError() == CDERR_STRUCTSIZE, "page setup size: %d %#lx\n", ret, CommDlgExtendedError());
    psd.lStructSize = sizeof(psd);
    psd.Flags = PSD_RETURNDEFAULT | PSD_NOWARNING;
    ret = PageSetupDlgA(&psd);
    ok(!ret && CommDlgExtendedError() == PDERR_NODEFAULTPRN, "page setup, no printer: %d %#lx\n", ret, CommDlgExtendedError());

    memset(&ex, 0, sizeof(ex));
    ex.lStructSize = sizeof(ex) - 1;
    hr = PrintDlgExW(&ex);
    ok(hr == E_INVALIDARG, "size: %#lx\n", hr);
    ex.lStructSize = sizeof(ex);
    hr = PrintDlgExW(&ex);
    ok(hr == E_HANDLE, "no owner: %#lx\n", hr);
    ex.hwndOwner = (HWND)(ULONG_PTR)0x1234;
    ex.Flags = PD_RETURNDEFAULT | PD_NOWARNING | PD_NOPAGENUMS;
    hr = PrintDlgExW(&ex);
    ok(hr == E_INVALIDARG, "start page 0: %#lx\n", hr);
    ex.Flags = PD_RETURNDEFAULT | PD_NOWARNING;
    ex.nStartPage = START_PAGE_GENERAL;
    hr = PrintDlgExW(&ex);
    ok(hr == E_INVALIDARG, "page ranges asked for but none given: %#lx\n", hr);
    ex.lpPageRanges = range;
    hr = PrintDlgExW(&ex);
    ok(hr == E_INVALIDARG, "ranges without a maximum: %#lx\n", hr);
    ex.lpPageRanges = NULL;
    ex.nMaxPageRanges = 1;
    hr = PrintDlgExW(&ex);
    ok(hr == E_INVALIDARG, "a maximum without ranges: %#lx\n", hr);
    ex.lpPageRanges = range;
    hr = PrintDlgExW(&ex);
    ok(hr == E_FAIL && CommDlgExtendedError() == PDERR_NODEFAULTPRN, "no printer: %#lx %#lx\n", hr, CommDlgExtendedError());
    ok(ex.dwResultAction == PD_RESULT_CANCEL, "result action %lu\n", ex.dwResultAction);
}

START_TEST(tcos)
{
    test_file_title();
    test_file_dialog_params();
    test_color();
    test_font();
    test_find_replace();
    test_print();
}
