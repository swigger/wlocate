// Exercise the private GUI controller against real Windows common controls,
// with deterministic replies and no index service or worker thread.
#include "../src/gui.cpp"
#include <iostream>

namespace {
struct IconGate {
    wl::Handle started{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    wl::Handle release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<unsigned> calls = 0;
    std::atomic<DWORD> thread = 0;
};
LRESULT CALLBACK host_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto window = reinterpret_cast<wl::Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        window = static_cast<wl::Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd = hwnd;
    }
    if (window && (message == WM_NOTIFY || message == WM_COMMAND || message == wl::icons_completed)) return window->message(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
std::unique_ptr<wl::Completion> reply(wl::Window& window, uint64_t count = 3, uint64_t offset = 0) {
    auto result = std::make_unique<wl::Completion>();
    result->token = window.token; result->offset = offset;
    result->reply.total = count; result->reply.generation = 42;
    for (auto i = offset; i < std::min(count, offset + wl::page_size); ++i) {
        wl::Result row; row.path = "C:\\fixture\\" + std::to_string(i) + "\\Figma.exe";
        result->reply.results.push_back(std::move(row));
    }
    return result;
}
}

int main(int argc, char** argv) {
    try {
        wl::require_utf8_process(); OleInitialize(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        auto instance = GetModuleHandleW(nullptr);
        bool profile = argc > 1 && std::string_view(argv[1]) == "--profile-input";
        auto gate = std::make_shared<IconGate>();
        wl::AsyncIcons::Loader slow_icon = [gate](const wl::IconRequest&) {
            ++gate->calls; gate->thread = GetCurrentThreadId(); SetEvent(gate->started.value);
            WaitForSingleObject(gate->release.value, 10000);
            return wl::FileIcon{nullptr, 7};
        };
        wl::Window window(instance, L"figma.exe", {}, profile ? wl::load_file_icon : slow_icon);
        WNDCLASSW cls{}; cls.lpfnWndProc = host_proc; cls.hInstance = instance; cls.lpszClassName = L"WLocate.CacheTest"; RegisterClassW(&cls);
        auto hwnd = CreateWindowW(cls.lpszClassName, L"WLocate cache regression", WS_OVERLAPPEDWINDOW, 0, 0, 800, 500, nullptr, nullptr, instance, &window);
        window.search = CreateWindowW(L"EDIT", L"figma.exe", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(1), instance, nullptr);
        window.filter = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2), instance, nullptr);
        window.list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
            0, 0, 780, 400, hwnd, reinterpret_cast<HMENU>(3), instance, nullptr);
        ListView_SetExtendedListViewStyle(window.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(window.list, L"Explorer", nullptr);
        LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = 500; ListView_InsertColumn(window.list, 0, &column);
        window.status = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD, 0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (profile) {
            window.icons.start(hwnd, wl::icons_completed);
            auto timed = [](auto action) {
                auto start = std::chrono::steady_clock::now(); action();
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            };
            for (auto query : {L"", L"p", L"py", L"pyt", L"pyth", L"pytho", L"python", L"python*"}) {
                SetWindowTextW(window.search, query);
                auto clear_ms = timed([&] { window.start_search(); });
                window.tasks.clear();
                auto result = std::make_unique<wl::Completion>(); result->token = window.token;
                auto search_ms = timed([&] { result->reply = wl::request_service(window.request()); });
                auto count = result->reply.total;
                auto publish_ms = timed([&] { window.receive(std::move(result)); UpdateWindow(window.list); });
                std::cout << "query=" << wl::utf8(query) << " count=" << count << " clear_ms=" << clear_ms << " search_ms=" << search_ms << " publish_ms=" << publish_ms << std::endl;
                for (int i = 0; i < std::min<uint64_t>(count, 30); ++i) {
                    NMLVDISPINFOW info{}; info.item.iItem = i; info.item.mask = LVIF_IMAGE;
                    auto ms = timed([&] { window.display(&info); });
                    if (ms > 10) std::cout << "  icon_ms=" << ms << " path=" << window.row(i)->path << std::endl;
                }
            }
            DestroyWindow(hwnd); window.hwnd = nullptr; OleUninitialize(); return 0;
        }
        window.receive(reply(window)); UpdateWindow(window.list);
        // Dismissing the Shell menu onto blank list space clears selection but
        // leaves the last row focused. The next automatic refresh used to loop.
        SetFocus(window.list);
        ListView_SetItemState(window.list, 2, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(window.list, -1, 0, LVIS_SELECTED);
        window.tasks.clear(); window.pending.clear();
        unsigned failures = 0, checks = 0;
        auto check = [&](bool ok, const char* what) {
            ++checks; std::cout << (ok ? "PASS " : "FAIL ") << what << '\n'; if (!ok) ++failures;
        };
        window.queue(0, true); window.tasks.clear();
        window.receive(reply(window)); UpdateWindow(window.list);
        check(window.tasks.empty() && window.pending.empty(), "focused last row refresh queues no duplicate query");
        check(window.row(2) && window.row(2)->path == "C:\\fixture\\2\\Figma.exe", "last row remains available after refresh");
        check(ListView_GetNextItem(window.list, -1, LVNI_FOCUSED) == 2, "refresh preserves focused row");
        std::cout << "Unexpected queued queries: " << window.tasks.size() << '\n';
        window.tasks.clear(); window.pending.clear();
        for (unsigned i = 0; i < 100; ++i) {
            window.receive(reply(window)); UpdateWindow(window.list);
        }
        check(window.tasks.empty() && window.pending.empty(), "100 refresh/repaint cycles do not trigger another query");
        auto hint = [&](int from, int to) {
            NMLVCACHEHINT notification{}; notification.hdr = {window.list, 3, LVN_ODCACHEHINT};
            notification.iFrom = from; notification.iTo = to;
            SendMessageW(hwnd, WM_NOTIFY, 3, reinterpret_cast<LPARAM>(&notification));
        };
        hint(0, 2); hint(-1, -1); hint(256, 512); hint(2, 1);
        check(window.tasks.empty(), "cached, negative, reversed and out-of-range cache hints queue no query");
        window.receive(reply(window, 0));
        hint(0, 0); hint(-1, -1);
        check(window.tasks.empty() && !window.row(0) && ListView_GetItemCount(window.list) == 0, "empty results ignore cache hints and clear rows");
        window.receive(reply(window, 300)); window.tasks.clear(); window.pending.clear();
        hint(256, 1000); hint(256, 299);
        check(window.tasks.size() == 1 && window.tasks.front().offset == 256, "last-page hints are bounded by total and deduplicated");
        window.tasks.clear(); window.receive(reply(window, 300, 256));
        check(window.pending.empty() && window.row(299) && !window.row(300), "partial last page loads with correct bounds");
        auto stale = reply(window, 3); stale->token = window.token - 1;
        window.receive(std::move(stale));
        check(window.total == 300 && window.row(299), "stale query completion cannot replace current results");
        // Hold the Shell provider behind a gate. Painting and real EDIT changes
        // must keep working even while it cannot return an icon.
        window.icons.start(hwnd, wl::icons_completed);
        window.pages[0][0].path = "C:\\fixture\\python.py";
        auto icon_row = window.pages[0][0];
        NMLVDISPINFOW info{}; info.item.mask = LVIF_IMAGE; info.item.iItem = 0;
        auto began = GetTickCount64(); window.display(&info);
        check(GetTickCount64() - began < 100 && info.item.iImage == I_IMAGENONE, "painting returns a placeholder without waiting for Shell");
        check(WaitForSingleObject(gate->started.value, 1000) == WAIT_OBJECT_0 && gate->thread != GetCurrentThreadId(), "Shell provider runs on a separate thread");
        for (unsigned i = 0; i < 100; ++i) window.display(&info);
        check(gate->calls == 1, "repaints do not enqueue duplicate icon loads");
        window.queue(0, true); auto old_reply = reply(window, 3); ResetEvent(window.cancel.value);
        began = GetTickCount64();
        for (auto query : {L"p", L"py", L"pyt", L"pyth", L"pytho", L"python", L"python*"}) SetWindowTextW(window.search, query);
        check(GetTickCount64() - began < 100 && wl::window_text(window.search) == L"python*", "typing all prefixes remains responsive while the icon provider is blocked");
        check(window.search_scheduled && window.tasks.empty() && window.pending.empty() && WaitForSingleObject(window.cancel.value, 0) == WAIT_OBJECT_0, "text changes immediately cancel and retire the previous query");
        window.receive(std::move(old_reply)); window.queue(256);
        check(window.total == 300 && window.tasks.empty(), "old replies and page requests cannot refresh results during debounce");
        window.start_search();
        check(window.tasks.size() == 1 && window.tasks.front().request.patterns[0] == "python*", "debounce starts only the latest query");
        SetEvent(gate->release.value);
        auto deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline && !window.icons.has_file_icon(icon_row)) { window.icons.collect(); Sleep(1); }
        check(window.icons.lookup(icon_row) == 7 && window.icons.has_file_icon(icon_row), "asynchronous file icon completion is cached on the UI thread");
        ResetEvent(gate->release.value); ResetEvent(gate->started.value);
        window.icons.lookup(".pyc");
        check(WaitForSingleObject(gate->started.value, 1000) == WAIT_OBJECT_0, "second icon provider call entered");
        began = GetTickCount64(); window.icons.stop();
        check(GetTickCount64() - began < 100, "closing does not wait for a blocked Shell provider");
        SetEvent(gate->release.value);
        window.tasks.clear(); window.pending.clear(); window.busy = false;
        window.receive(reply(window, 3000000)); window.attach_accessibility();
        wl::ComPtr<IRawElementProviderFragment> root;
        window.accessibility.provider()->QueryInterface(IID_PPV_ARGS(&root));
        wl::ComPtr<IRawElementProviderFragment> current;
        root->Navigate(NavigateDirection_FirstChild, &current);
        unsigned accessible_rows = 0; began = GetTickCount64();
        while (current && accessible_rows < 100) {
            ++accessible_rows; wl::ComPtr<IRawElementProviderFragment> next;
            current->Navigate(NavigateDirection_NextSibling, &next); current = std::move(next);
        }
        check(accessible_rows > 0 && accessible_rows <= static_cast<unsigned>(ListView_GetCountPerPage(window.list) + 1), "three million results expose only visible rows to accessibility traversal");
        check(GetTickCount64() - began < 100, "large-result accessibility traversal stays bounded");
        // Test through the real UI Automation client as well as the provider
        // interface; the GUI thread must keep pumping COM/window messages.
        struct AutomationResult { std::atomic<bool> done = false; std::atomic<int> stage = 0; HRESULT hr = E_FAIL; int count = 0; uint64_t ms = 0; };
        auto automation_result = std::make_shared<AutomationResult>();
        std::thread([target = window.list, automation_result] {
            auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); auto start = GetTickCount64();
            automation_result->stage = 1;
            wl::ComPtr<IUIAutomation> automation; wl::ComPtr<IUIAutomationElement> element;
            wl::ComPtr<IUIAutomationCondition> condition; wl::ComPtr<IUIAutomationElementArray> rows;
            auto hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            automation_result->stage = 2;
            if (SUCCEEDED(hr)) hr = automation->ElementFromHandle(target, &element);
            automation_result->stage = 3;
            VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ListItemControlTypeId;
            if (SUCCEEDED(hr)) hr = automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition);
            automation_result->stage = 4;
            if (SUCCEEDED(hr)) hr = element->FindAll(TreeScope_Descendants, condition.Get(), &rows);
            automation_result->stage = 5;
            if (SUCCEEDED(hr)) hr = rows->get_Length(&automation_result->count);
            automation_result->hr = hr; automation_result->ms = GetTickCount64() - start;
            rows.Reset(); condition.Reset(); element.Reset(); automation.Reset();
            if (SUCCEEDED(com)) CoUninitialize(); automation_result->done = true;
        }).detach();
        auto automation_deadline = GetTickCount64() + 10000;
        while (!automation_result->done && GetTickCount64() < automation_deadline) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        check(automation_result->done && SUCCEEDED(automation_result->hr) && automation_result->count > 0 && automation_result->count <= 50,
            "real UI Automation FindAll visits only visible results out of three million");
        check(automation_result->done && automation_result->ms < 1000, "real UI Automation traversal completes in under a second");
        if (automation_result->done) std::cout << "UI Automation visible rows: " << automation_result->count << ", elapsed: " << automation_result->ms << " ms, HRESULT: " << std::hex << automation_result->hr << std::dec << '\n';
        else std::cout << "UI Automation timed out at stage " << automation_result->stage << '\n';
        wl::ComPtr<IItemContainerProvider> container;
        window.accessibility.provider()->QueryInterface(IID_PPV_ARGS(&container));
        VARIANT id{}; id.vt = VT_BSTR; id.bstrVal = SysAllocString(L"row-2999999");
        wl::ComPtr<IRawElementProviderSimple> virtual_row;
        auto hr = container->FindItemByProperty(nullptr, UIA_AutomationIdPropertyId, id, &virtual_row); VariantClear(&id);
        check(SUCCEEDED(hr) && virtual_row, "offscreen results remain discoverable by ItemContainer");
        VARIANT name{};
        check(virtual_row && virtual_row->GetPropertyValue(UIA_NamePropertyId, &name) == UIA_E_ELEMENTNOTAVAILABLE, "unrealized row exposes a virtual placeholder"); VariantClear(&name);
        wl::ComPtr<IVirtualizedItemProvider> realize; virtual_row.As(&realize);
        check(realize && SUCCEEDED(realize->Realize()) && ListView_GetTopIndex(window.list) > 2999900, "VirtualizedItem realizes distant rows without enumerating the whole index");
        wl::ComPtr<ISelectionItemProvider> selection; virtual_row.As(&selection); BOOL selected = FALSE;
        check(selection && SUCCEEDED(selection->Select()) && SUCCEEDED(selection->get_IsSelected(&selected)) && selected && ListView_GetSelectedCount(window.list) == 1, "virtualized result supports native selection");
        wl::ComPtr<IRawElementProviderSimple> next;
        VARIANT ignored{};
        check(SUCCEEDED(container->FindItemByProperty(virtual_row.Get(), 0, ignored, &next)) && !next, "ItemContainer iteration ends after the last result");
        window.accessibility.reset();
        check(virtual_row->GetPropertyValue(UIA_NamePropertyId, &name) == UIA_E_ELEMENTNOTAVAILABLE, "old accessibility rows become unavailable when the query changes");
        window.accessibility.detach();
        check(root->Navigate(NavigateDirection_FirstChild, &current) == UIA_E_ELEMENTNOTAVAILABLE, "providers retained by clients are safe after window detachment");
        DestroyWindow(hwnd); window.hwnd = nullptr; OleUninitialize();
        std::cout << checks << " checks, " << failures << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
