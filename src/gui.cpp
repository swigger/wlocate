#include "app.hpp"
#include "shell.hpp"
#include "resource.h"
#include "gui_icons.hpp"
#include "gui_accessibility.hpp"
#include "gui_hotkeys.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <deque>
#include <fstream>
#include <unordered_set>

namespace wl {
namespace {
constexpr UINT completed = WM_APP + 1, tray_message = WM_APP + 2, icons_completed = WM_APP + 3;
enum : UINT { id_export = 100, id_exit, id_open, id_parent, id_copy, id_cut, id_paths, id_delete, id_properties,
    id_rename, id_selectall, id_refresh, id_case, id_path, id_word, id_regex, id_status, id_reindex, id_install,
    id_help, id_about, id_tray, id_startup, id_bookmark, id_hotkey, id_bookmarks = 300 };
constexpr uint64_t page_size = 256;
std::wstring window_text(HWND window) { auto n = GetWindowTextLengthW(window); std::wstring text(n + 1, L'\0'); GetWindowTextW(window, text.data(), n + 1); text.resize(n); return text; }
std::filesystem::path settings_path() {
    wchar_t* folder = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
    std::filesystem::path path = folder ? folder : L"."; CoTaskMemFree(folder); path /= L"WLocate"; std::filesystem::create_directories(path); return path / L"settings.ini";
}
std::wstring readable_time(uint64_t ticks) {
    if (!ticks) return {};
    FILETIME utc{DWORD(ticks), DWORD(ticks >> 32)}, local{}; SYSTEMTIME time{};
    FileTimeToLocalFileTime(&utc, &local); FileTimeToSystemTime(&local, &time);
    wchar_t buffer[64]; swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute); return buffer;
}
std::string csv_field(std::string text) { size_t p = 0; while ((p = text.find('"', p)) != text.npos) { text.insert(p, 1, '"'); p += 2; } return '"' + text + '"'; }
void render_window(HWND window, const std::filesystem::path& path) {
    RECT rect; GetWindowRect(window, &rect); auto width = rect.right - rect.left, height = rect.bottom - rect.top;
    auto screen = GetDC(window), memory = CreateCompatibleDC(screen); auto bitmap = CreateCompatibleBitmap(screen, width, height);
    auto old = SelectObject(memory, bitmap); BOOL painted = PrintWindow(window, memory, 2); SelectObject(memory, old); DeleteDC(memory); ReleaseDC(window, screen);
    if (!painted) { DeleteObject(bitmap); throw Error("Window rendering failed"); }
    ComPtr<IWICImagingFactory> factory; auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmap> image; if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image);
    DeleteObject(bitmap); ComPtr<IWICStream> stream; if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder; if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame; if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr); if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    auto format = GUID_WICPixelFormat24bppBGR; if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(image.Get(), nullptr); if (SUCCEEDED(hr)) hr = frame->Commit(); if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) throw Error("Could not save rendered window: " + win_error(static_cast<DWORD>(hr)));
}
struct Task { uint64_t token = 0, offset = 0; Request request; std::filesystem::path export_file; };
struct Completion { uint64_t token, offset; Reply reply; std::string error; std::filesystem::path export_file; };
class Window {
public:
    HWND hwnd = nullptr, search = nullptr, filter = nullptr, list = nullptr, status = nullptr;
    HFONT font = nullptr;
    HICON icon_large = nullptr, icon_small = nullptr;
    HINSTANCE instance;
    std::filesystem::path ini = settings_path();
    uint32_t flags = EverythingSyntax;
    Sort sort = Sort::Name;
    bool descending = false, busy = false, closing = false, close_to_tray = false, search_scheduled = false;
    uint64_t token = 0, total = 0, generation = 0;
    std::wstring initial_query;
    std::filesystem::path test_output;
    int test_stage = 0, test_exit = 0;
    uint64_t test_initial_count = 0, test_deadline = 0, test_last_tick = 0, test_max_tick_gap = 0;
    std::unordered_map<uint64_t, std::vector<Result>> pages;
    std::unordered_set<uint64_t> pending;
    AsyncIcons icons;
    AccessibleList accessibility;
    GlobalHotkey hotkey;
    std::vector<std::wstring> bookmarks;
    Handle work{CreateEventW(nullptr, FALSE, FALSE, nullptr)}, cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex tasks_mutex;
    std::deque<Task> tasks;
    std::jthread worker;
    explicit Window(HINSTANCE app, std::wstring query, std::filesystem::path testing, AsyncIcons::Loader icon_loader = load_file_icon)
        : instance(app), initial_query(std::move(query)), test_output(std::move(testing)), icons(std::move(icon_loader)) {
        if (!test_output.empty()) ini = test_output / L"test-settings.ini";
    }
    ~Window() { shutdown(); if (font) DeleteObject(font); if (icon_large) DestroyIcon(icon_large); if (icon_small) DestroyIcon(icon_small); }
    int scaled(int pixels) const { return MulDiv(pixels, static_cast<int>(GetDpiForWindow(hwnd)), 96); }
    void error(const std::exception& e) {
        if (!test_output.empty()) { std::ofstream file(test_output / L"failure.txt"); file << e.what(); test_exit = 1; closing = true; PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
        MessageBoxW(hwnd, wide(e.what()).c_str(), L"WLocate", MB_OK | MB_ICONERROR);
    }
    void shutdown() {
        accessibility.detach();
        icons.stop();
        if (!worker.joinable()) return;
        worker.request_stop(); SetEvent(cancel.value); SetEvent(work.value); worker.join();
        MSG msg; while (PeekMessageW(&msg, hwnd, completed, completed, PM_REMOVE)) delete reinterpret_cast<Completion*>(msg.lParam);
    }
    Request request() {
        Request r; r.flags = flags; r.sort = sort; r.descending = descending; r.limit = page_size; r.timeout_ms = 120000;
        std::string query = utf8(window_text(search)); auto selection = SendMessageW(filter, CB_GETCURSEL, 0, 0);
        const char* types[]{"", "file:", "folder:", "ext:exe;com;bat;cmd;ps1;msi", "ext:txt;md;pdf;doc;docx;xls;xlsx;ppt;pptx", "ext:jpg;jpeg;png;gif;webp;bmp;svg;ico", "ext:mp3;flac;wav;m4a;ogg", "ext:mp4;mkv;avi;mov;wmv", "ext:zip;7z;rar;tar;gz"};
        r.patterns.push_back(std::move(query));
        if (selection > 0 && selection < std::size(types)) r.patterns.push_back(types[selection]);
        return r;
    }
    void queue(uint64_t offset, bool fresh = false, std::filesystem::path export_file = {}) {
        if (search_scheduled) return;
        auto r = request(); r.offset = offset;
        if (fresh) { ++token; pending.clear(); busy = true; }
        if (export_file.empty() && !pending.insert(offset).second) return;
        if (!export_file.empty()) { r.limit = UINT64_MAX; r.offset = 0; }
        { std::lock_guard guard(tasks_mutex); if (fresh) { tasks.clear(); SetEvent(cancel.value); } tasks.push_back({token, offset, std::move(r), std::move(export_file)}); }
        SetEvent(work.value);
    }
    void run_worker() {
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                WaitForSingleObject(work.value, INFINITE);
                while (!stop.stop_requested()) {
                    Task task;
                    { std::lock_guard guard(tasks_mutex); if (tasks.empty()) break; task = std::move(tasks.front()); tasks.pop_front(); ResetEvent(cancel.value); }
                    auto result = std::make_unique<Completion>(); result->token = task.token; result->offset = task.offset; result->export_file = task.export_file;
                    try {
                        result->reply = request_service(task.request, cancel.value);
                        if (!task.export_file.empty()) {
                            GUID guid; CoCreateGuid(&guid); wchar_t suffix[40]; StringFromGUID2(guid, suffix, 40);
                            auto temporary = task.export_file; temporary += std::wstring(L".") + suffix + L".tmp";
                            try {
                                std::ofstream file(temporary, std::ios::binary | std::ios::trunc); if (!file) throw Error("Cannot create export file");
                                bool csv = fold(utf8(task.export_file.extension().wstring())) == ".csv";
                                if (csv) file << "\xEF\xBB\xBFName,Path,Size,Modified\r\n";
                                for (auto& row : result->reply.results) {
                                    if (stop.stop_requested() || WaitForSingleObject(cancel.value, 0) == WAIT_OBJECT_0) throw Error("Export cancelled");
                                    if (csv) file << csv_field(filename(row.path)) << ',' << csv_field(directory(row.path)) << ',' << row.size << ',' << csv_field(utf8(readable_time(row.modified))) << "\r\n";
                                    else file << row.path << "\r\n";
                                }
                                file.flush(); if (!file) throw Error("Failed while writing export"); file.close();
                                if (!MoveFileExW(temporary.c_str(), task.export_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw Error("Cannot finish export: " + win_error());
                            } catch (...) {
                                std::error_code ignored; std::filesystem::remove(temporary, ignored); throw;
                            }
                            result->reply.results.clear();
                        }
                    } catch (const std::exception& e) { result->error = e.what(); }
                    if (stop.stop_requested()) break;
                    if (PostMessageW(hwnd, completed, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
                }
            }
        });
    }
    void schedule_search() {
        // Retire the old query as soon as the text changes, not when the debounce
        // timer expires: a late reply must not repaint results for earlier text.
        ++token; pending.clear(); busy = true; search_scheduled = true;
        { std::lock_guard guard(tasks_mutex); tasks.clear(); SetEvent(cancel.value); }
        SetTimer(hwnd, 1, 100, nullptr);
    }
    void start_search() {
        search_scheduled = false;
        accessibility.reset();
        KillTimer(hwnd, 1); ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        pages.clear(); total = 0; ListView_SetItemCountEx(list, 0, LVSICF_NOSCROLL); queue(0, true);
        SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"正在搜索…"));
    }
    void receive(std::unique_ptr<Completion> result) {
        if (result->token != token) return;
        pending.erase(result->offset); busy = false;
        if (!result->error.empty()) { SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(wide(result->error).c_str())); return; }
        if (!result->export_file.empty()) { auto text = L"已导出：" + result->export_file.wstring(); SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); return; }
        auto& reply = result->reply;
        if (!result->offset) {
            if (generation != reply.generation || total != reply.total) accessibility.reset();
            pages.clear(); total = reply.total; generation = reply.generation;
        } else if (generation != reply.generation) { queue(0, true); return; }
        pages[result->offset] = std::move(reply.results);
        // LVM_SETITEMCOUNT can synchronously request the focused row through
        // LVN_GETDISPINFO. Publish its cache first, or every completed refresh
        // schedules another query while the cache is temporarily empty.
        if (!result->offset)
            ListView_SetItemCountEx(list, static_cast<int>(std::min<uint64_t>(total, INT_MAX)), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        if (pages.size() > 32) { for (auto it = pages.begin(); it != pages.end(); ++it) if (it->first != result->offset && it->first != 0) { pages.erase(it); break; } }
        auto text = std::to_wstring(total) + L" 个结果    " + std::to_wstring(reply.search_us / 1000.0).substr(0, 7) + L" ms";
        if (!reply.message.empty()) text += L"    " + wide(reply.message);
        SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
        InvalidateRect(list, nullptr, FALSE);
    }
    const Result* row(int index) {
        if (index < 0 || static_cast<uint64_t>(index) >= total) return nullptr;
        auto start = uint64_t(index) / page_size * page_size; auto found = pages.find(start);
        if (found == pages.end()) { queue(start); return nullptr; }
        auto at = uint64_t(index) - start; return at < found->second.size() ? &found->second[static_cast<size_t>(at)] : nullptr;
    }
    std::vector<std::wstring> selection() {
        if (busy) throw Error("Search results are updating; please try again in a moment");
        std::vector<std::wstring> paths;
        for (int i = -1; (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0;) {
            auto value = row(i);
            if (!value) {
                auto start = uint64_t(i) / page_size * page_size; auto r = request(); r.offset = start;
                auto reply = request_service(r);
                if (reply.generation != generation) throw Error("The index changed while loading the selection; refresh and select again");
                pages[start] = std::move(reply.results); value = row(i);
            }
            if (!value) throw Error("Selected rows could not be loaded"); paths.push_back(wide(value->path));
        }
        return paths;
    }
    void display(NMLVDISPINFOW* info) {
        auto value = row(info->item.iItem); std::wstring text = value ? L"" : L"…";
        if (value) {
            switch (info->item.iSubItem) {
            case 0: text = wide(filename(value->path)); break;
            case 1: text = wide(directory(value->path)); break;
            case 2: if (!(value->attributes & FILE_ATTRIBUTE_DIRECTORY)) { wchar_t bytes[64]; StrFormatByteSizeW(static_cast<LONGLONG>(value->size), bytes, 64); text = bytes; } break;
            case 3: text = readable_time(value->modified); break;
            case 4: text = value->attributes & FILE_ATTRIBUTE_DIRECTORY ? L"文件夹" : wide(std::filesystem::path(wide(value->path)).extension().wstring().empty() ? "文件" : utf8(std::filesystem::path(wide(value->path)).extension().wstring())); break;
            }
            if (info->item.mask & LVIF_IMAGE) {
                info->item.iImage = icons.lookup(*value);
            }
        }
        if ((info->item.mask & LVIF_TEXT) && info->item.pszText && info->item.cchTextMax) wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
    }
    void make_menu() {
        HMENU bar = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(), find = CreatePopupMenu(), saved = CreatePopupMenu(), tools = CreatePopupMenu(), help = CreatePopupMenu();
        auto add = [](HMENU menu, UINT id, const wchar_t* text) { AppendMenuW(menu, MF_STRING, id, text); };
        add(file, id_open, L"打开(&O)\tEnter"); add(file, id_parent, L"打开所在文件夹\tCtrl+Enter"); add(file, id_export, L"导出搜索结果(&E)…\tCtrl+S"); add(file, id_exit, L"退出(&X)");
        add(edit, id_copy, L"复制(&C)\tCtrl+C"); add(edit, id_cut, L"剪切(&T)\tCtrl+X"); add(edit, id_paths, L"复制完整路径\tCtrl+Shift+C");
        add(edit, id_rename, L"重命名(&R)\tF2"); add(edit, id_delete, L"删除(&D)\tDelete"); add(edit, id_properties, L"属性(&P)\tAlt+Enter"); add(edit, id_selectall, L"全选(&A)\tCtrl+A");
        add(find, id_case, L"区分大小写(&C)"); add(find, id_path, L"匹配完整路径(&P)"); add(find, id_word, L"全字匹配(&W)"); add(find, id_regex, L"正则表达式(&R)"); add(find, id_refresh, L"刷新(&F)\tF5");
        add(saved, id_bookmark, L"收藏当前搜索(&A)\tCtrl+D");
        for (size_t i = 0; i < bookmarks.size(); ++i) add(saved, id_bookmarks + static_cast<UINT>(i), bookmarks[i].c_str());
        add(tools, id_status, L"索引状态(&S)…"); add(tools, id_reindex, L"重新建立索引(&R)…"); add(tools, id_install, L"安装或修复后台服务(&I)…");
        add(tools, id_tray, L"关闭窗口时留在托盘"); add(tools, id_startup, L"登录时启动界面");
        add(tools, id_hotkey, L"快捷键设置(&K)…");
        add(help, id_help, L"搜索语法(&H)…"); add(help, id_about, L"关于 WLocate(&A)…");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件(&F)");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"编辑(&E)"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(find), L"搜索(&S)");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(saved), L"书签(&B)"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"工具(&T)"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"帮助(&H)");
        auto old = GetMenu(hwnd); SetMenu(hwnd, bar); if (old) DestroyMenu(old);
        CheckMenuItem(bar, id_case, MF_BYCOMMAND | (flags & MatchCase ? MF_CHECKED : MF_UNCHECKED)); CheckMenuItem(bar, id_path, MF_BYCOMMAND | (flags & MatchPath ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(bar, id_word, MF_BYCOMMAND | (flags & WholeWord ? MF_CHECKED : MF_UNCHECKED)); CheckMenuItem(bar, id_regex, MF_BYCOMMAND | (flags & Regex ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(bar, id_tray, MF_BYCOMMAND | (close_to_tray ? MF_CHECKED : MF_UNCHECKED)); DrawMenuBar(hwnd);
    }
    void layout() {
        RECT client; GetClientRect(hwnd, &client); SendMessageW(status, WM_SIZE, 0, 0); RECT bar; GetWindowRect(status, &bar);
        auto margin = scaled(8), height = scaled(28), combo = scaled(150);
        MoveWindow(search, margin, margin, std::max(10L, client.right - 3 * margin - combo), height, TRUE);
        MoveWindow(filter, client.right - margin - combo, margin, combo, scaled(300), TRUE);
        MoveWindow(list, 0, 2 * margin + height, client.right, std::max(0L, client.bottom - (bar.bottom - bar.top) - 2 * margin - height), TRUE);
        if (Header_GetItemCount(ListView_GetHeader(list)) == 5) {
            int fixed = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(hwnd)) + scaled(6);
            for (int column : {0, 2, 3, 4}) fixed += ListView_GetColumnWidth(list, column);
            ListView_SetColumnWidth(list, 1, std::max(scaled(180), static_cast<int>(client.right) - fixed));
        }
    }
    void update_font() {
        auto previous = font;
        font = CreateFontW(-scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (auto window : {search, filter, list, status}) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (previous) DeleteObject(previous);
    }
    void update_icons() {
        auto dpi = GetDpiForWindow(hwnd);
        auto load = [&](int x, int y) { return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_WLOCATE), IMAGE_ICON,
            GetSystemMetricsForDpi(x, dpi), GetSystemMetricsForDpi(y, dpi), LR_DEFAULTCOLOR)); };
        auto next_large = load(SM_CXICON, SM_CYICON), next_small = load(SM_CXSMICON, SM_CYSMICON);
        if (!next_large || !next_small) { if (next_large) DestroyIcon(next_large); if (next_small) DestroyIcon(next_small); throw Error("Cannot load WLocate icon"); }
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(next_large));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(next_small));
        NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = hwnd; tray.uID = 1; tray.uFlags = NIF_ICON; tray.hIcon = next_small;
        Shell_NotifyIconW(NIM_MODIFY, &tray);
        if (icon_large) DestroyIcon(icon_large); if (icon_small) DestroyIcon(icon_small);
        icon_large = next_large; icon_small = next_small;
    }
    void update_tray_tip() {
        auto text = L"WLocate 文件查找（" + hotkey_name(hotkey.config()) + (hotkey.failure().empty() ? L"）" : L"，未生效）");
        NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = hwnd; tray.uID = 1; tray.uFlags = NIF_TIP;
        wcsncpy_s(tray.szTip, text.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &tray);
    }
    void configure_hotkey() {
        HotkeyDialog dialog{hotkey.config(), hotkey.failure(), [this](HotkeyConfig value) {
            hotkey.change(value, [this](const HotkeyConfig& config) { return save_hotkey(ini, config); });
            update_tray_tip();
        }};
        if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_HOTKEY), hwnd, HotkeyDialog::procedure, reinterpret_cast<LPARAM>(&dialog)) == -1)
            throw Error("无法打开快捷键设置：" + win_error());
    }
    void create() {
        flags = GetPrivateProfileIntW(L"Search", L"flags", EverythingSyntax, ini.c_str()) | EverythingSyntax;
        close_to_tray = GetPrivateProfileIntW(L"Window", L"tray", 0, ini.c_str()) != 0;
        for (unsigned i = 0; i < 30; ++i) { wchar_t value[4096]{}; auto key = std::to_wstring(i); GetPrivateProfileStringW(L"Bookmarks", key.c_str(), L"", value, 4096, ini.c_str()); if (*value) bookmarks.emplace_back(value); }
        make_menu();
        search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial_query.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(1), instance, nullptr);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索文件和文件夹，例如 python*.exe 或 ext:cpp size:>1kb"));
        SendMessageW(search, EM_SETLIMITTEXT, 30000, 0);
        filter = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2), instance, nullptr);
        for (const auto* name : {L"全部", L"文件", L"文件夹", L"可执行文件", L"文档", L"图片", L"音频", L"视频", L"压缩文件"}) SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(filter, CB_SETCURSEL, 0, 0);
        list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_SHAREIMAGELISTS, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3), instance, nullptr);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
        SetWindowTheme(list, L"Explorer", nullptr); SetWindowTheme(search, L"Explorer", nullptr);
        const wchar_t* labels[]{L"名称", L"路径", L"大小", L"修改时间", L"类型"}; int widths[]{220, 400, 95, 155, 80};
        for (int i = 0; i < 5; ++i) {
            auto key = std::to_wstring(i); auto width = std::clamp<int>(GetPrivateProfileIntW(L"Columns", key.c_str(), widths[i], ini.c_str()), 40, 2000);
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = const_cast<wchar_t*>(labels[i]); column.cx = scaled(width); column.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(list, i, &column);
        }
        icons.start(hwnd, icons_completed); icons.lookup(""); icons.lookup("<folder>");
        attach_accessibility();
        status = CreateWindowExW(0, STATUSCLASSNAMEW, L"正在连接索引服务…", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
        update_font(); update_icons();
        layout(); run_worker(); start_search(); SetTimer(hwnd, 2, 2000, nullptr); SetFocus(search);
        if (!test_output.empty()) { test_deadline = GetTickCount64() + 60000; SetTimer(hwnd, 3, 200, nullptr); }
        hotkey.start(hwnd, test_output.empty() ? load_hotkey(ini) : HotkeyConfig{false});
        NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = hwnd; tray.uID = 1; tray.uFlags = NIF_MESSAGE | NIF_ICON; tray.uCallbackMessage = tray_message; tray.hIcon = icon_small; Shell_NotifyIconW(NIM_ADD, &tray);
        update_tray_tip();
        if (!hotkey.failure().empty()) {
            tray.uFlags = NIF_INFO; tray.dwInfoFlags = NIIF_WARNING;
            wcscpy_s(tray.szInfoTitle, L"全局快捷键未生效");
            wcscpy_s(tray.szInfo, L"设定的快捷键已被占用或被系统保留。请通过“工具 → 快捷键设置”修改，也可以点击托盘图标打开窗口。");
            Shell_NotifyIconW(NIM_MODIFY, &tray);
        }
    }
    void save() {
        auto value = std::to_wstring(flags); WritePrivateProfileStringW(L"Search", L"flags", value.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Window", L"tray", close_to_tray ? L"1" : L"0", ini.c_str());
        RECT rect; GetWindowRect(hwnd, &rect);
        if (!IsIconic(hwnd)) { value = std::to_wstring(rect.right - rect.left); WritePrivateProfileStringW(L"Window", L"width", value.c_str(), ini.c_str()); value = std::to_wstring(rect.bottom - rect.top); WritePrivateProfileStringW(L"Window", L"height", value.c_str(), ini.c_str()); }
        for (size_t i = 0; i < bookmarks.size(); ++i) { auto key = std::to_wstring(i); WritePrivateProfileStringW(L"Bookmarks", key.c_str(), bookmarks[i].c_str(), ini.c_str()); }
        for (int column = 0; column < 5; ++column) {
            auto key = std::to_wstring(column); value = std::to_wstring(MulDiv(ListView_GetColumnWidth(list, column), 96, GetDpiForWindow(hwnd)));
            WritePrivateProfileStringW(L"Columns", key.c_str(), value.c_str(), ini.c_str());
        }
    }
    void command(UINT id);
    void attach_accessibility() {
        accessibility.attach(list, [this](int index) {
            auto value = row(index); return value ? wide(filename(value->path)) + L" — " + wide(directory(value->path)) : L"正在加载…";
        }, [this](int) { PostMessageW(hwnd, WM_COMMAND, id_open, 0); });
    }
    void test_tick();
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
};
void Window::test_tick() {
    if (GetTickCount64() > test_deadline) throw Error("UI integration test timed out");
    if (initial_query == L"figma.exe") {
        if (busy || !pending.empty() || !total || total > page_size) return;
        bool custom = false;
        for (int i = 0; i < static_cast<int>(total); ++i) {
            auto value = row(i); if (!value) return;
            icons.lookup(*value); if (!icons.has_file_icon(*value)) return;
            custom |= icons.lookup(*value) != icons.lookup(".exe");
        }
        if (!custom) throw Error("Figma results still have only the generic EXE icon");
        render_window(hwnd, test_output / L"file-icons.png");
        std::ofstream report(test_output / L"report.txt"); report << total << " Figma results received their actual full-path icons without input.\n";
        for (int i = 0; i < static_cast<int>(total); ++i) report << icons.lookup(*row(i)) << " " << row(i)->path << '\n';
        closing = true; PostMessageW(hwnd, WM_CLOSE, 0, 0); return;
    }
    if (test_stage >= 4) {
        auto now = GetTickCount64();
        test_max_tick_gap = std::max(test_max_tick_gap, now - test_last_tick); test_last_tick = now;
        if (test_stage < 10) {
            const wchar_t* prefixes[]{L"py", L"pyt", L"pyth", L"pytho", L"python", L"python*"};
            SetWindowTextW(search, prefixes[test_stage - 4]); ++test_stage; return;
        }
    }
    if (busy || !pending.empty()) return;
    auto check = [](bool ok, const char* what) { if (!ok) throw Error(what); };
    if (test_stage == 0) {
        check(GetACP() == CP_UTF8, "GUI process code page is not UTF-8");
        check(icon_large && icon_small && reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0)) == icon_small, "GUI does not use the embedded WLocate icon");
        if (!total) { start_search(); return; }
        check(total > 0, "Initial GUI query returned no results"); check(ListView_GetItemCount(list) == static_cast<int>(total), "Virtual list count mismatch");
        check(Header_GetItemCount(ListView_GetHeader(list)) == 5, "Missing result columns");
        auto value = row(0); check(value && fold(filename(value->path)).starts_with("python"), "Unexpected first GUI result");
        test_initial_count = total; render_window(hwnd, test_output / L"search.png");
        SendMessageW(filter, CB_SETCURSEL, 3, 0); SetWindowTextW(search, L"python*.exe | *.txt"); ++test_stage; start_search();
    } else if (test_stage == 1) {
        check(total == test_initial_count, "GUI type filter did not scope across the OR expression");
        SendMessageW(filter, CB_SETCURSEL, 0, 0); SetWindowTextW(search, L"*.exe"); ++test_stage; start_search();
    } else if (test_stage == 2) {
        check(total > 300, "Expected enough executable files to test virtual paging");
        ListView_EnsureVisible(list, 300, FALSE); row(300); ++test_stage;
    } else if (test_stage == 3) {
        if (!row(300)) return;
        render_window(hwnd, test_output / L"paging.png");
        auto fixture = test_output / L"shell-fixture"; std::filesystem::create_directories(fixture / L"nested");
        auto a = fixture / L"a.txt", b = fixture / L"nested" / L"b.txt";
        { std::ofstream file(a); file << "WLocate UI test"; } { std::ofstream file(b); file << "WLocate UI test"; }
        auto single = shell_menu_verbs(hwnd, {a.wstring()}); auto multi = shell_menu_verbs(hwnd, {a.wstring(), b.wstring()});
        for (auto needed : {"copy", "cut", "delete", "rename", "properties"}) check(std::find(single.begin(), single.end(), needed) != single.end(), "Single-item Shell menu is missing a core verb");
        for (auto needed : {"copy", "cut", "delete", "properties"}) check(std::find(multi.begin(), multi.end(), needed) != multi.end(), "Cross-folder Shell selection is missing a core verb");
        shell_rename(hwnd, a.wstring(), L"renamed-中文.txt");
        check(std::filesystem::exists(fixture / L"renamed-中文.txt") && !std::filesystem::exists(a), "Shell rename did not update the file");
        std::ofstream report(test_output / L"report.txt"); report << "GUI search, filter grouping, virtual paging, native Shell menus and Unicode Shell rename passed.\nInitial matches: " << test_initial_count << "\nSingle-file verbs:\n";
        for (auto& verb : single) report << verb << '\n'; report << "Multi-file verbs:\n"; for (auto& verb : multi) report << verb << '\n';
        ++test_stage; test_last_tick = GetTickCount64();
        SetWindowTextW(search, L"p");
    } else if (test_stage == 10) {
        check(window_text(search) == L"python*" && total > 0, "Typing replay did not finish the latest query");
        check(row(0) && fold(filename(row(0)->path)).starts_with("python"), "Typing replay displayed an obsolete query");
        check(test_max_tick_gap < 1000, "UI heartbeat stalled for at least a second while typing");
        render_window(hwnd, test_output / L"typing.png");
        std::ofstream report(test_output / L"report.txt", std::ios::app);
        report << "Typing replay p -> py -> pyt -> pyth -> pytho -> python -> python* passed.\n"
            << "Maximum UI heartbeat gap (200 ms timer): " << test_max_tick_gap << " ms\n";
        ++test_stage; closing = true; PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}
void Window::command(UINT id) {
    if (id >= id_bookmarks && id < id_bookmarks + bookmarks.size()) { SetWindowTextW(search, bookmarks[id - id_bookmarks].c_str()); start_search(); return; }
    switch (id) {
    case id_exit: closing = true; SendMessageW(hwnd, WM_CLOSE, 0, 0); return;
    case id_refresh: start_search(); return;
    case id_selectall: ListView_SetItemState(list, -1, LVIS_SELECTED, LVIS_SELECTED); return;
    case id_case: flags ^= MatchCase; break;
    case id_path: flags ^= MatchPath; break;
    case id_word: flags ^= WholeWord; break;
    case id_regex: flags ^= Regex; break;
    case id_tray: close_to_tray = !close_to_tray; make_menu(); save(); return;
    case id_hotkey: configure_hotkey(); return;
    case id_bookmark: {
        auto query = window_text(search); if (!query.empty() && std::find(bookmarks.begin(), bookmarks.end(), query) == bookmarks.end()) { if (bookmarks.size() == 30) bookmarks.erase(bookmarks.begin()); bookmarks.push_back(query); make_menu(); save(); } return;
    }
    case id_startup: {
        HKEY key; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) throw Error("Cannot open startup settings");
        DWORD size = 0; bool enabled = RegQueryValueExW(key, L"WLocate", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
        if (enabled) RegDeleteValueW(key, L"WLocate");
        else { auto text = L"\"" + module_path() + L"\" --tray"; RegSetValueExW(key, L"WLocate", 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * 2)); }
        RegCloseKey(key); CheckMenuItem(GetMenu(hwnd), id_startup, MF_BYCOMMAND | (!enabled ? MF_CHECKED : MF_UNCHECKED)); return;
    }
    case id_status: {
        Request r; r.operation = 1; r.timeout_ms = 3000; auto reply = request_service(r);
        MessageBoxW(hwnd, wide(reply.message).c_str(), L"索引状态", MB_OK | MB_ICONINFORMATION); return;
    }
    case id_reindex: case id_install: {
        SHELLEXECUTEINFOW info{sizeof(info)}; info.hwnd = hwnd; auto path = module_path(); info.lpFile = path.c_str(); info.lpVerb = L"runas"; info.lpParameters = id == id_install ? L"--install-service" : L"--reindex"; info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info) && GetLastError() != ERROR_CANCELLED) throw Error(win_error()); return;
    }
    case id_export: {
        wchar_t path[32768] = L"WLocate-results.csv"; OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = hwnd;
        dialog.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0文本文件 (*.txt)\0*.txt\0\0"; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
        dialog.lpstrDefExt = L"csv"; dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetSaveFileNameW(&dialog)) { queue(0, false, path); SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"正在导出全部搜索结果…")); } return;
    }
    case id_help:
        MessageBoxW(hwnd, L"输入文字即时查找名称；空格表示 AND，| 表示 OR，! 排除。\n\n通配符：*  ?  [a-z]\n引号：\"Program Files\"\n类型：file:  folder:  ext:exe;dll\n路径：path:C:\\Windows  parent:C:\\Temp\n大小：size:>1mb  size:1kb..10mb\n时间：dm:today  dc:2026-01-01..2026-12-31\n属性：attrib:h  attrib:r\n其他：case:Python  regex:^python.*\\.exe$\n\nCtrl+L 搜索框，Ctrl+Alt+L 显示窗口\nEnter 打开，Ctrl+Enter 所在文件夹，Alt+Enter 属性\nCtrl+C 复制文件，Ctrl+X 剪切，Ctrl+Shift+C 复制路径\nF2 重命名，Delete 删除，Shift+Delete 永久删除\nCtrl+S 导出全部结果，Ctrl+D 收藏当前搜索\n\n命令行示例：locate -n 10 \"python*.exe\"", L"搜索语法和快捷键", MB_OK | MB_ICONINFORMATION); return;
    case id_about: {
        auto text = L"WLocate 1.0.0 · 原生 x64 文件查找\n\n独立的 NTFS 索引与 USN 增量更新服务。\n界面使用 Windows Shell 右键菜单。\n不依赖 Everything 或 .NET。\n\n全局快捷键：" + hotkey_name(hotkey.config()) + L"\n可在“工具 → 快捷键设置”中修改。";
        MessageBoxW(hwnd, text.c_str(), L"关于 WLocate", MB_OK | MB_ICONINFORMATION); return;
    }
    default: {
        auto paths = selection(); if (paths.empty()) return;
        switch (id) {
        case id_open: for (auto& path : paths) shell_open(hwnd, path); break;
        case id_parent: for (auto& path : paths) shell_open(hwnd, path, true); break;
        case id_copy: clipboard_paths(hwnd, paths, false, false); break;
        case id_cut: clipboard_paths(hwnd, paths, true, false); break;
        case id_paths: clipboard_paths(hwnd, paths, false, true); break;
        case id_delete: shell_verb(hwnd, paths, "delete", GetKeyState(VK_SHIFT) < 0); break;
        case id_properties: shell_verb(hwnd, paths, "properties"); break;
        case id_rename: SetFocus(list); ListView_EditLabel(list, ListView_GetNextItem(list, -1, LVNI_SELECTED)); break;
        }
        return;
    }
    }
    make_menu(); start_search();
}
LRESULT Window::message(UINT message, WPARAM wparam, LPARAM lparam) {
    bool handled; auto shell_result = shell_menu_message(message, wparam, lparam, handled); if (handled) return shell_result;
    switch (message) {
    case WM_CREATE: create(); return 0;
    case WM_SIZE: if (status) layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {scaled(620), scaled(300)}; return 0;
    case WM_DPICHANGED: { update_font(); update_icons(); auto rect = reinterpret_cast<RECT*>(lparam); SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER); layout(); return 0; }
    case WM_SETFOCUS: SetFocus(search); return 0;
    case WM_HOTKEY:
        if (hotkey.handles(wparam) && HIWORD(lparam) == hotkey.config().key && LOWORD(lparam) == hotkey.config().modifiers) {
            auto popup = GetLastActivePopup(hwnd);
            if (popup != hwnd && IsWindowVisible(popup)) { SetForegroundWindow(popup); return 0; }
            ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); SetFocus(search);
        }
        return 0;
    case WM_COPYDATA: {
        auto data = reinterpret_cast<COPYDATASTRUCT*>(lparam);
        if (data->dwData == 1 && data->cbData >= 2 && data->cbData <= 60002 && data->cbData % 2 == 0) {
            auto value = static_cast<const wchar_t*>(data->lpData);
            if (value[data->cbData / 2 - 1] == 0) { SetWindowTextW(search, value); ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); start_search(); return TRUE; }
        }
        return FALSE;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == search && HIWORD(wparam) == EN_CHANGE) { schedule_search(); return 0; }
        if (reinterpret_cast<HWND>(lparam) == filter && HIWORD(wparam) == CBN_SELCHANGE) { start_search(); return 0; }
        if (!lparam) { command(LOWORD(wparam)); return 0; } break;
    case WM_TIMER:
        if (wparam == 1) start_search();
        else if (wparam == 3) test_tick();
        else if (wparam == 2 && test_output.empty() && IsWindowVisible(hwnd) && !busy && pending.empty() && !ListView_GetSelectedCount(list) && !ListView_GetEditControl(list)) queue(0, true);
        return 0;
    case completed: receive(std::unique_ptr<Completion>(reinterpret_cast<Completion*>(lparam))); return 0;
    case icons_completed:
        if (icons.collect()) {
            if (auto images = icons.image_list(); images && images != ListView_GetImageList(list, LVSIL_SMALL)) ListView_SetImageList(list, images, LVSIL_SMALL);
            InvalidateRect(list, nullptr, FALSE);
        }
        return 0;
    case WM_NOTIFY: {
        auto notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->hwndFrom != list) break;
        switch (notification->code) {
        case LVN_GETDISPINFOW: display(reinterpret_cast<NMLVDISPINFOW*>(lparam)); return 0;
        case NM_DBLCLK: command(id_open); return 0;
        case LVN_COLUMNCLICK: {
            auto column = reinterpret_cast<NMLISTVIEW*>(lparam)->iSubItem; Sort values[]{Sort::Name, Sort::Path, Sort::Size, Sort::Modified, Sort::Extension};
            if (sort == values[column]) descending = !descending; else { sort = values[column]; descending = false; }
            auto header = ListView_GetHeader(list);
            for (int i = 0; i < 5; ++i) { HDITEMW item{}; item.mask = HDI_FORMAT; Header_GetItem(header, i, &item); item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN); if (i == column) item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP; Header_SetItem(header, i, &item); }
            start_search(); return 0;
        }
        case LVN_BEGINLABELEDITW: return row(reinterpret_cast<NMLVDISPINFOW*>(lparam)->item.iItem) ? FALSE : TRUE;
        case LVN_ENDLABELEDITW: {
            auto info = reinterpret_cast<NMLVDISPINFOW*>(lparam); auto value = row(info->item.iItem);
            if (value && info->item.pszText) { shell_rename(hwnd, wide(value->path), info->item.pszText); SetTimer(hwnd, 1, 350, nullptr); } return FALSE;
        }
        case LVN_BEGINDRAG: shell_drag(hwnd, selection()); return 0;
        case LVN_ODCACHEHINT: {
            auto hint = reinterpret_cast<NMLVCACHEHINT*>(lparam);
            if (hint->iFrom < 0 || hint->iTo < hint->iFrom || static_cast<uint64_t>(hint->iFrom) >= total) return 0;
            auto first = uint64_t(hint->iFrom) / page_size * page_size;
            auto last = std::min<uint64_t>(hint->iTo, total - 1) / page_size * page_size;
            for (auto offset = first; offset <= std::min(last, first + 2 * page_size); offset += page_size) if (!pages.contains(offset)) queue(offset);
            return 0;
        }
        }
        break;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wparam) == list) {
            auto paths = selection(); if (paths.empty()) return 0;
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (point.x == -1 && point.y == -1) { RECT rect; ListView_GetItemRect(list, ListView_GetNextItem(list, -1, LVNI_SELECTED), &rect, LVIR_BOUNDS); point = {rect.left + scaled(30), rect.bottom}; ClientToScreen(list, &point); }
            auto selected = shell_context(hwnd, paths, point);
            if (selected == shell_open_parent) for (auto& path : paths) shell_open(hwnd, path, true);
            if (selected == shell_copy_path) clipboard_paths(hwnd, paths, false, true);
            if (selected == shell_rename_selected) command(id_rename);
            return 0;
        }
        break;
    case tray_message:
        if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); SetFocus(search); }
        if (lparam == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"显示 WLocate"); AppendMenuW(menu, MF_STRING, 3, L"快捷键设置…"); AppendMenuW(menu, MF_STRING, 2, L"退出"); POINT point; GetCursorPos(&point); SetForegroundWindow(hwnd);
            auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr); DestroyMenu(menu);
            if (selected == 1) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); } if (selected == 2) command(id_exit);
            if (selected == 3) command(id_hotkey);
        }
        return 0;
    case WM_CLOSE:
        if (close_to_tray && !closing) { save(); ShowWindow(hwnd, SW_HIDE); return 0; }
        if (test_output.empty()) save(); shutdown(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        hotkey.stop(); NOTIFYICONDATAW tray{sizeof(tray)}; tray.hWnd = hwnd; tray.uID = 1; Shell_NotifyIconW(NIM_DELETE, &tray); PostQuitMessage(test_exit); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); self->hwnd = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    try { return self->message(message, wparam, lparam); } catch (const std::exception& error) { self->error(error); return 0; }
}
}
int run_gui(HINSTANCE instance, int show, std::wstring initial_query, std::filesystem::path test_output) {
    if (auto existing = test_output.empty() ? FindWindowW(L"WLocate.Main.v1", nullptr) : nullptr) {
        COPYDATASTRUCT data{1, static_cast<DWORD>((initial_query.size() + 1) * 2), initial_query.data()}; DWORD_PTR result;
        SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result); return 0;
    }
    OleInitialize(nullptr); INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WLOCATE)); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = L"WLocate.Main.v1"; RegisterClassExW(&cls);
    Window window(instance, std::move(initial_query), std::move(test_output));
    auto width = std::clamp(GetPrivateProfileIntW(L"Window", L"width", 1200, window.ini.c_str()), 620u, 4000u);
    auto height = std::clamp(GetPrivateProfileIntW(L"Window", L"height", 740, window.ini.c_str()), 300u, 2400u);
    auto hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"WLocate", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, instance, &window);
    if (!hwnd) throw Error("Create main window: " + win_error()); ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0; UINT command = 0;
            bool editing = GetFocus() == window.search || GetFocus() == ListView_GetEditControl(window.list);
            if (ctrl && (message.wParam == 'L' || message.wParam == 'F')) { SetFocus(window.search); SendMessageW(window.search, EM_SETSEL, 0, -1); continue; }
            if (message.wParam == VK_F5) command = id_refresh;
            else if (ctrl && message.wParam == 'S') command = id_export;
            else if (ctrl && message.wParam == 'D') command = id_bookmark;
            else if (message.wParam == VK_RETURN && GetFocus() == window.search) { SetFocus(window.list); if (window.total) { ListView_SetItemState(window.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); } continue; }
            else if (!editing) {
                if (message.wParam == VK_RETURN) command = alt ? id_properties : ctrl ? id_parent : id_open;
                else if (message.wParam == VK_DELETE) command = id_delete;
                else if (message.wParam == VK_F2) command = id_rename;
                else if (ctrl && message.wParam == 'C') command = shift ? id_paths : id_copy;
                else if (ctrl && message.wParam == 'X') command = id_cut;
                else if (ctrl && message.wParam == 'A') command = id_selectall;
            }
            if (command) { SendMessageW(hwnd, WM_COMMAND, command, 0); continue; }
            if (message.wParam == VK_ESCAPE && !ListView_GetEditControl(window.list)) { SetFocus(window.search); SendMessageW(window.search, EM_SETSEL, 0, -1); continue; }
        }
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    window.shutdown(); OleUninitialize(); return static_cast<int>(message.wParam);
}
}
