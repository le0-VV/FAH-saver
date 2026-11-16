#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <psapi.h>
#include <WebView2.h>
#include <fah/Model.hpp>
#include <nlohmann/json.hpp>
#include "Options.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using fah::web_benchmark::Options;
constexpr UINT finishMessage = WM_APP + 1;

void require(HRESULT result,const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << unsigned(result) << ')';
        throw std::runtime_error(message.str());
    }
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!length) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(length,L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),length))
        throw std::runtime_error("Cannot convert UTF-8 text");
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if (!length) throw std::runtime_error("Invalid Unicode text");
    std::string result(length,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),length,nullptr,nullptr))
        throw std::runtime_error("Cannot convert Unicode text");
    return result;
}
struct Text {
    LPWSTR value = nullptr;
    ~Text() { CoTaskMemFree(value); }
    std::wstring string() const { return value ? value : L""; }
};
template<class T> struct Com {
    T* value = nullptr;
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value(std::exchange(other.value,nullptr)) {}
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    T** put() { reset(); return &value; }
    void set(T* object) { reset(); value = object; if (value) value->AddRef(); }
    void reset() { if (value) { value->Release(); value = nullptr; } }
};
template<class Interface,class... Arguments> class Callback final : public Interface {
    std::atomic<ULONG> references{1};
    IID id;
    std::function<HRESULT(Arguments...)> function;
public:
    Callback(REFIID identifier,std::function<HRESULT(Arguments...)> action) : id(identifier),function(std::move(action)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID requested,void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(requested,IID_IUnknown) || IsEqualIID(requested,id)) {
            *output = static_cast<Interface*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override { return function(arguments...); }
};
template<class Interface,class... Arguments,class Function>
Com<Interface> callback(REFIID id,Function function) {
    Com<Interface> result;
    result.value = new Callback<Interface,Arguments...>(id,std::move(function));
    return result;
}

std::uint64_t ticks(FILETIME time) { return (std::uint64_t(time.dwHighDateTime)<<32) | time.dwLowDateTime; }
double cpu(HANDLE process) {
    FILETIME created,exited,kernel,user;
    if (!GetProcessTimes(process,&created,&exited,&kernel,&user)) throw std::runtime_error("Cannot read process CPU time");
    return double(ticks(kernel)+ticks(user))/1e7;
}
struct Process {
    HANDLE handle = nullptr;
    DWORD pid = 0;
    int kind = 0;
    double cpuSeconds = 0;
    SIZE_T peakWorkingSet = 0,peakPrivateCommit = 0;
    ~Process() { if (handle) CloseHandle(handle); }
};

struct App : std::enable_shared_from_this<App> {
    Options options;
    HWND window = nullptr;
    HMODULE loader = nullptr;
    HANDLE browserExit = nullptr;
    Com<ICoreWebView2Environment> environment;
    Com<ICoreWebView2Environment8> inventory;
    Com<ICoreWebView2Controller> controller;
    Com<ICoreWebView2> web;
    EventRegistrationToken messageToken{},navigationToken{},loadedToken{},processToken{};
    bool hasMessage = false,hasNavigation = false,hasLoaded = false,hasProcessToken = false;
    bool done = false,closed = false,inventoryAvailable = false,processMetricsLimited = false,requestedEnvironment = false;
    std::wstring profile,url,scriptId;
    std::string injection,version;
    UINT32 browserPid = 0;
    Clock::time_point started = Clock::now();
    double initialCPU = 0;
    Json result;
    std::map<std::pair<DWORD,std::uint64_t>,std::unique_ptr<Process>> processes;
    SIZE_T peakWorkingSum = 0,peakPrivateSum = 0;

    ~App() { close(); if (browserExit) CloseHandle(browserExit); if (loader) FreeLibrary(loader); }

    void fail(const std::string& text) {
        if (done) return;
        done = true; result = {{"status","error"},{"error",text},{"host","WebView2"}};
        PostMessageW(window,finishMessage,0,0);
    }
    template<class Function> HRESULT guarded(Function function) {
        if (done) return S_OK;
        try { function(); } catch (const std::exception& error) { fail(error.what()); }
        return S_OK;
    }
    void resize() {
        if (!controller) return;
        RECT bounds;
        if (!GetClientRect(window,&bounds)) throw std::runtime_error("Cannot read benchmark drawable size");
        require(controller->put_Bounds(bounds),"Set WebView2 bounds");
    }
    void discover() {
        if (!inventory) return;
        Com<ICoreWebView2ProcessInfoCollection> list;
        if (FAILED(inventory->GetProcessInfos(list.put()))) { processMetricsLimited = true; return; }
        UINT32 count = 0;
        if (FAILED(list->get_Count(&count)) || count > 1024) { processMetricsLimited = true; return; }
        for (UINT32 i = 0; i < count; ++i) {
            Com<ICoreWebView2ProcessInfo> item;
            INT32 pid = 0; COREWEBVIEW2_PROCESS_KIND kind{};
            if (FAILED(list->GetValueAtIndex(i,item.put())) || FAILED(item->get_ProcessId(&pid)) || FAILED(item->get_Kind(&kind)) || pid <= 0) {
                processMetricsLimited = true; continue;
            }
            HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,FALSE,DWORD(pid));
            if (!handle) { processMetricsLimited = true; continue; }
            FILETIME created,exited,kernel,user;
            if (!GetProcessTimes(handle,&created,&exited,&kernel,&user)) { CloseHandle(handle); processMetricsLimited = true; continue; }
            const auto key = std::make_pair(DWORD(pid),ticks(created));
            if (processes.count(key)) { CloseHandle(handle); continue; }
            auto entry = std::make_unique<Process>(); entry->handle = handle; entry->pid = DWORD(pid); entry->kind = int(kind);
            processes.emplace(key,std::move(entry));
        }
    }
    void sample() {
        discover();
        SIZE_T working = 0,committed = 0;
        PROCESS_MEMORY_COUNTERS_EX host{}; host.cb = sizeof(host);
        if (GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&host),sizeof(host))) {
            working += host.WorkingSetSize; committed += host.PrivateUsage;
        } else processMetricsLimited = true;
        for (auto& entry : processes) {
            auto& process = *entry.second;
            try { process.cpuSeconds = cpu(process.handle); } catch (...) { processMetricsLimited = true; }
            DWORD status = 0;
            if (!GetExitCodeProcess(process.handle,&status) || status != STILL_ACTIVE) continue;
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
            if (!GetProcessMemoryInfo(process.handle,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) {
                processMetricsLimited = true; continue;
            }
            process.peakWorkingSet = std::max(process.peakWorkingSet,memory.PeakWorkingSetSize);
            process.peakPrivateCommit = std::max(process.peakPrivateCommit,memory.PrivateUsage);
            working += memory.WorkingSetSize; committed += memory.PrivateUsage;
        }
        peakWorkingSum = std::max(peakWorkingSum,working); peakPrivateSum = std::max(peakPrivateSum,committed);
    }

    void receive(ICoreWebView2WebMessageReceivedEventArgs* event) {
        Text source,message;
        require(event->get_Source(&source.value),"Read WebView2 message source");
        if (_wcsicmp(source.string().c_str(),url.c_str()) != 0) throw std::runtime_error("Unexpected benchmark message origin");
        require(event->get_WebMessageAsJson(&message.value),"Read benchmark result");
        const auto text = utf8(message.string());
        if (text.size() > 1024*1024) throw std::runtime_error("Benchmark result exceeds 1 MiB");
        auto data = Json::parse(text);
        if (!data.is_object() || !data.contains("status") || !data["status"].is_string())
            throw std::runtime_error("Invalid WebGL benchmark result");
        if (data["status"] != "ok") {
            const auto error = data.find("error");
            throw std::runtime_error(error != data.end() && error->is_string() ? error->get<std::string>() : "WebGL benchmark failed");
        }
        if (!data.contains("width") || !data.contains("height") || data["width"] != options.width || data["height"] != options.height)
            throw std::runtime_error("WebGL drawable does not match the requested physical pixel dimensions");
        result = std::move(data); done = true; PostMessageW(window,finishMessage,0,0);
    }

    void controllerReady(ICoreWebView2Controller* value) {
        if (!value) throw std::runtime_error("WebView2 returned no controller");
        controller.set(value);
        require(controller->get_CoreWebView2(web.put()),"Get WebView2");
        Com<ICoreWebView2Controller3> pixels;
        require(controller->QueryInterface(IID_ICoreWebView2Controller3,reinterpret_cast<void**>(pixels.put())),"Require physical-pixel WebView2 bounds");
        require(pixels->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS),"Set physical-pixel bounds mode");
        resize();
        require(controller->put_IsVisible(TRUE),"Show WebView2");
        require(web->get_BrowserProcessId(&browserPid),"Read browser process ID");
        browserExit = OpenProcess(SYNCHRONIZE,FALSE,browserPid);
        Com<ICoreWebView2Settings> settings;
        require(web->get_Settings(settings.put()),"Get WebView2 settings");
        require(settings->put_IsWebMessageEnabled(TRUE),"Enable benchmark message bridge");
        require(settings->put_AreDevToolsEnabled(FALSE),"Disable developer tools during benchmark");
        require(settings->put_AreDefaultContextMenusEnabled(FALSE),"Disable browser context menus");
        require(settings->put_IsStatusBarEnabled(FALSE),"Disable browser status bar");
        const auto weak = weak_from_this();
        auto messages = callback<ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>(
            IID_ICoreWebView2WebMessageReceivedEventHandler,[weak](auto*,auto* event) {
                if (auto app = weak.lock()) return app->guarded([&] { app->receive(event); });
                return S_OK;
            });
        require(web->add_WebMessageReceived(messages.value,&messageToken),"Register benchmark result handler"); hasMessage = true;
        auto navigation = callback<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>(
            IID_ICoreWebView2NavigationStartingEventHandler,[weak](auto*,auto* event) {
                if (auto app = weak.lock()) return app->guarded([&] {
                    Text destination; require(event->get_Uri(&destination.value),"Read navigation URI");
                    if (_wcsicmp(destination.string().c_str(),app->url.c_str()) != 0) {
                        require(event->put_Cancel(TRUE),"Cancel unexpected navigation");
                        throw std::runtime_error("Benchmark attempted an unexpected navigation");
                    }
                });
                return S_OK;
            });
        require(web->add_NavigationStarting(navigation.value,&navigationToken),"Register navigation guard"); hasNavigation = true;
        auto loaded = callback<ICoreWebView2NavigationCompletedEventHandler,ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*>(
            IID_ICoreWebView2NavigationCompletedEventHandler,[weak](auto*,auto* event) {
                if (auto app = weak.lock()) return app->guarded([&] {
                    BOOL success = FALSE; require(event->get_IsSuccess(&success),"Read navigation result");
                    if (!success) throw std::runtime_error("Cannot load benchmark HTML or scripts");
                    require(app->web->RemoveScriptToExecuteOnDocumentCreated(app->scriptId.c_str()),"Release capture bootstrap script");
                    app->scriptId.clear();
                    const auto script = "try{if(!window.fahBenchmark?.ready)throw new Error('Benchmark page did not initialise');"
                        "delete window.nativeCapture;document.getElementById('colour').value='#"+app->options.colour+"';window.fahBenchmark.start();}"
                        "catch(e){chrome.webview.postMessage({status:'error',error:String(e.message||e)});}";
                    require(app->web->ExecuteScript(wide(script).c_str(),nullptr),"Start benchmark");
                });
                return S_OK;
            });
        require(web->add_NavigationCompleted(loaded.value,&loadedToken),"Register page-loaded handler"); hasLoaded = true;
        auto injected = callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,HRESULT,LPCWSTR>(
            IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,[weak](HRESULT status,LPCWSTR id) {
                if (auto app = weak.lock()) return app->guarded([&] {
                    require(status,"Inject capture");
                    if (!id || !*id) throw std::runtime_error("WebView2 returned no bootstrap script identifier");
                    app->scriptId = id; app->injection.clear(); app->injection.shrink_to_fit();
                    require(app->web->Navigate(app->url.c_str()),"Load local benchmark");
                });
                return S_OK;
            });
        require(web->AddScriptToExecuteOnDocumentCreated(wide(injection).c_str(),injected.value),"Register capture injection");
        sample();
    }

    void environmentReady(ICoreWebView2Environment* value) {
        if (!value) throw std::runtime_error("WebView2 returned no environment");
        environment.set(value);
        Text browserVersion;
        require(environment->get_BrowserVersionString(&browserVersion.value),"Read runtime version"); version = utf8(browserVersion.string());
        const auto status = environment->QueryInterface(IID_ICoreWebView2Environment8,reinterpret_cast<void**>(inventory.put()));
        inventoryAvailable = SUCCEEDED(status);
        if (inventoryAvailable) {
            const auto weak = weak_from_this();
            auto changed = callback<ICoreWebView2ProcessInfosChangedEventHandler,ICoreWebView2Environment*,IUnknown*>(
                IID_ICoreWebView2ProcessInfosChangedEventHandler,[weak](auto*,auto*) {
                    if (auto app = weak.lock()) return app->guarded([&] { app->discover(); });
                    return S_OK;
                });
            require(inventory->add_ProcessInfosChanged(changed.value,&processToken),"Register process inventory handler"); hasProcessToken = true;
        } else if (status != E_NOINTERFACE) require(status,"Query WebView2 process inventory");
        const auto weak = weak_from_this();
        auto created = callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>(
            IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,[weak](HRESULT status,auto* controller) {
                if (auto app = weak.lock()) return app->guarded([&] { require(status,"Create WebView2 controller"); app->controllerReady(controller); });
                return S_OK;
            });
        require(environment->CreateCoreWebView2Controller(window,created.value),"Request WebView2 controller");
    }

    void start() {
        started = Clock::now(); initialCPU = cpu(GetCurrentProcess());
        const auto capture = fah::readCapture(options.capture);
        injection = "(()=>{if(window.top!==window)return;window.nativeCapture=JSON.parse("+
            Json(fah::writeCapture(capture)).dump()+");window.webkit={messageHandlers:{benchmark:{postMessage:r=>chrome.webview.postMessage(r)}}};})();";
        const auto html = std::filesystem::absolute(std::filesystem::u8path(options.html)).lexically_normal();
        if (!std::filesystem::is_regular_file(html)) throw std::runtime_error("Benchmark HTML file does not exist");
        std::wstring uri(131072,L'\0'); DWORD characters = DWORD(uri.size());
        require(UrlCreateFromPathW(html.c_str(),uri.data(),&characters,0),"Create local HTML URL"); uri.resize(wcslen(uri.c_str()));
        url = uri+L"?benchmark=1&autostart=0&fps="+std::to_wstring(options.fps)+L"&seconds="+std::to_wstring(options.seconds)+
            L"&warmup="+std::to_wstring(options.warmup)+L"&style="+wide(options.style)+L"&playback="+wide(options.playback)+L"&background="+wide(options.background);
        wchar_t temporary[32768]; const DWORD length = GetTempPathW(32768,temporary);
        if (!length || length >= 32768) throw std::runtime_error("Cannot locate temporary profile directory");
        GUID id{}; require(CoCreateGuid(&id),"Create private profile identifier");
        wchar_t identifier[40]; if (!StringFromGUID2(id,identifier,40)) throw std::runtime_error("Cannot name temporary profile");
        profile = (std::filesystem::path(temporary)/(std::wstring(L"FAHWebBenchmark-")+identifier)).wstring();
        if (!CreateDirectoryW(profile.c_str(),nullptr)) throw std::runtime_error("Cannot create isolated WebView2 profile");
        std::wstring executable(32768,L'\0'); const DWORD size = GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
        if (!size || size == executable.size()) throw std::runtime_error("Cannot locate WebView2 loader"); executable.resize(size);
        const auto library = std::filesystem::path(executable).parent_path()/L"WebView2Loader.dll";
        loader = LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!loader) throw std::runtime_error("Official WebView2Loader.dll is unavailable beside the benchmark executable");
        using CreateEnvironment = decltype(&CreateCoreWebView2EnvironmentWithOptions);
        const auto create = reinterpret_cast<CreateEnvironment>(GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions"));
        if (!create) throw std::runtime_error("WebView2 loader does not export its environment API");
        const auto weak = weak_from_this();
        auto created = callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>(
            IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,[weak](HRESULT status,auto* environment) {
                if (auto app = weak.lock()) return app->guarded([&] {
                    if (status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) throw std::runtime_error("Microsoft Edge WebView2 Runtime is not installed");
                    require(status,"Create WebView2 environment"); app->environmentReady(environment);
                });
                return S_OK;
            });
        const auto status = create(nullptr,profile.c_str(),nullptr,created.value);
        if (status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) throw std::runtime_error("Microsoft Edge WebView2 Runtime is not installed");
        require(status,"Request WebView2 environment");
        requestedEnvironment = true;
        if (!SetTimer(window,1,500,nullptr)) throw std::runtime_error("Cannot start benchmark timeout and process sampler");
    }

    void collectResult() {
        sample();
        result["host"] = "WebView2"; result["runtime_version"] = version; result["browser_process_id"] = browserPid;
        result["host_elapsed_seconds_including_startup"] = std::chrono::duration<double>(Clock::now()-started).count();
        result["host_cpu_seconds_including_startup"] = cpu(GetCurrentProcess())-initialCPU;
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)))
            result["host_peak_working_set_bytes"] = memory.PeakWorkingSetSize;
        else { result["host_peak_working_set_bytes"] = nullptr; processMetricsLimited = true; }
        result["requested_drawable_width"] = options.width; result["requested_drawable_height"] = options.height;
        result["complete_process_cost_measured"] = false;
        result["webview_process_inventory_available"] = inventoryAvailable;
        result["process_metric_access_or_sampling_failures"] = processMetricsLimited;
        result["process_sampling_interval_ms"] = 500;
        result["owned_processes"] = Json::array();
        double browserCPU = 0;
        for (const auto& entry : processes) {
            const auto& value = *entry.second; browserCPU += value.cpuSeconds;
            result["owned_processes"].push_back({{"pid",value.pid},{"kind",value.kind},{"cpu_seconds_since_process_creation",value.cpuSeconds},
                {"peak_working_set_bytes",value.peakWorkingSet},{"sampled_peak_private_committed_bytes",value.peakPrivateCommit}});
        }
        result["sampled_browser_group_cpu_seconds_since_creation"] = inventoryAvailable ? Json(browserCPU) : Json(nullptr);
        result["sampled_host_and_browser_peak_working_set_sum_bytes_including_shared_pages"] = inventoryAvailable ? Json(peakWorkingSum) : Json(nullptr);
        result["sampled_host_and_browser_peak_private_committed_sum_bytes"] = inventoryAvailable ? Json(peakPrivateSum) : Json(nullptr);
        result["process_metric_limitations"] = {
            "WebView2 process inventory excludes crashpad.","Polling and inventory events can miss short-lived processes.",
            "Working-set sums count shared pages more than once; they are not unique resident memory.",
            "Metrics include profile/browser startup, trial and immediate controller close; the shutdown wait is separate.",
            "GPU memory, displayed-frame timestamps and power remain unmeasured."};
    }

    void close() {
        if (closed) return; closed = true;
        if (window) KillTimer(window,1);
        if (web) {
            if (hasMessage) web->remove_WebMessageReceived(messageToken);
            if (hasNavigation) web->remove_NavigationStarting(navigationToken);
            if (hasLoaded) web->remove_NavigationCompleted(loadedToken);
        }
        if (inventory && hasProcessToken) inventory->remove_ProcessInfosChanged(processToken);
        if (controller) controller->Close();
        web.reset(); controller.reset(); inventory.reset(); environment.reset();
    }
    void save() {
        std::error_code error;
        if (!profile.empty()) {
            const auto start = Clock::now();
            bool exited = !requestedEnvironment;
            while (browserExit && std::chrono::duration<double>(Clock::now()-start).count() < 5) {
                const DWORD status = MsgWaitForMultipleObjectsEx(1,&browserExit,100,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if (status == WAIT_OBJECT_0) { exited = true; break; }
                if (status == WAIT_FAILED) break;
                MSG message;
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
                }
            }
            result["browser_shutdown_wait_seconds"] = std::chrono::duration<double>(Clock::now()-start).count();
            if (exited) {
                std::filesystem::remove_all(profile,error);
                result["temporary_profile_cleanup"] = error ? "retained_files_in_use" : "removed";
            } else result["temporary_profile_cleanup"] = "retained_browser_exit_unverified";
            if (error || !exited) result["retained_temporary_profile"] = utf8(profile);
        }
        const auto text = result.dump(2)+"\n";
        if (!options.output.empty()) {
            std::ofstream output(std::filesystem::u8path(options.output),std::ios::binary);
            if (!output || !output.write(text.data(),std::streamsize(text.size()))) throw std::runtime_error("Cannot write benchmark result file");
            output.close();
            if (!output) throw std::runtime_error("Cannot finish writing benchmark result file");
        }
        std::fwrite(text.data(),1,text.size(),stdout); std::fflush(stdout);
    }
};

LRESULT CALLBACK windowProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window,message,wparam,lparam);
    try {
        if (message == WM_SIZE) { app->resize(); return 0; }
        if (message == WM_TIMER) {
            if (std::chrono::duration<double>(Clock::now()-app->started).count() > app->options.seconds+app->options.warmup+30)
                app->fail("WebView2 comparison timed out");
            else app->sample();
            return 0;
        }
        if (message == finishMessage) { DestroyWindow(window); return 0; }
        if (message == WM_CLOSE) { app->fail("Benchmark window closed before trial completion"); return 0; }
        if (message == WM_DESTROY) { app->close(); PostQuitMessage(0); return 0; }
    } catch (const std::exception& error) { app->fail(error.what()); return 0; }
    return DefWindowProcW(window,message,wparam,lparam);
}

int run(std::vector<std::string> arguments) {
    auto app = std::make_shared<App>();
    app->options = fah::web_benchmark::parse(arguments);
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        throw std::runtime_error("Cannot enable physical-pixel DPI awareness");
    require(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Initialise COM STA");
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    WNDCLASSW type{}; type.lpfnWndProc = windowProcedure; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"FAHWebView2Benchmark"; type.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register benchmark window");
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0,0,LONG(app->options.width),LONG(app->options.height)};
    if (!AdjustWindowRectExForDpi(&bounds,style,FALSE,0,GetDpiForSystem())) throw std::runtime_error("Cannot size physical-pixel benchmark window");
    const HWND window = CreateWindowExW(0,type.lpszClassName,L"FAH WebGL comparison — WebView2",style,
        CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,type.hInstance,app.get());
    if (!window) throw std::runtime_error("Cannot create benchmark window");
    RECT actual{0,0,LONG(app->options.width),LONG(app->options.height)};
    if (!AdjustWindowRectExForDpi(&actual,style,FALSE,0,GetDpiForWindow(window)) ||
        !SetWindowPos(window,nullptr,0,0,actual.right-actual.left,actual.bottom-actual.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        DestroyWindow(window); throw std::runtime_error("Cannot set benchmark client pixel dimensions");
    }
    ShowWindow(window,SW_SHOW); UpdateWindow(window);
    try { app->start(); } catch (const std::exception& error) { app->fail(error.what()); }
    MSG message;
    for (;;) {
        const BOOL status = GetMessageW(&message,nullptr,0,0);
        if (status == -1) { app->fail("Cannot read benchmark window events"); break; }
        if (!status) break;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (!app->done) app->fail("Benchmark ended without a result");
    if (IsWindow(window)) DestroyWindow(window);
    app->collectResult(); app->close(); app->save();
    const int result = app->result.value("status",std::string("error")) == "ok" ? 0 : 1;
    app.reset();
    return result;
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        int count = 0; LPWSTR* values = CommandLineToArgvW(GetCommandLineW(),&count);
        if (!values) throw std::runtime_error("Cannot read benchmark arguments");
        std::vector<std::string> arguments;
        for (int i = 1; i < count; ++i) arguments.push_back(utf8(values[i]));
        LocalFree(values);
        if (arguments.size() == 1 && arguments[0] == "--help") {
            std::puts("fah-webview-benchmark --capture FILE --html index.html [--output result.json] [--width 1280] [--height 720] [--fps 60] [--seconds 10] [--warmup 0] [--style 1|2|3|cycle|random] [--playback latest|loop] [--background sky|solid] [--colour 050609]");
            return 0;
        }
        return run(std::move(arguments));
    } catch (const std::exception& error) { std::fprintf(stderr,"WebView2 benchmark: %s\n",error.what()); return 1; }
}
