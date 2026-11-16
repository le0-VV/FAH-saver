#include "Options.hpp"
#include "DecimalEdit.hpp"
#include "Renderer.hpp"
#include <fah/Source.hpp>
#include <fah/LiveModel.hpp>
#include <fah/SnapshotCache.hpp>
#include <shellapi.h>
#include <psapi.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using namespace fah::windows;
using Clock = std::chrono::steady_clock;
constexpr UINT sourceMessage = WM_APP + 1;
constexpr wchar_t registryPath[] = L"Software\\FAHSaver";

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!size) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(size,L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),size))
        throw std::runtime_error("Cannot convert UTF-8 text");
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode text");
    std::string result(size,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),size,nullptr,nullptr))
        throw std::runtime_error("Cannot convert Unicode text");
    return result;
}
void errorBox(HWND parent,const std::string& error) { MessageBoxW(parent,wide(error).c_str(),L"FAH Saver",MB_OK | MB_ICONERROR); }

std::string setting(const wchar_t* name) {
    DWORD bytes = 0;
    auto result = RegGetValueW(HKEY_CURRENT_USER,registryPath,name,RRF_RT_REG_SZ,nullptr,nullptr,&bytes);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
    if (result != ERROR_SUCCESS || bytes > 65536 || bytes < sizeof(wchar_t))
        throw std::runtime_error("Cannot read screensaver preferences");
    std::wstring value(bytes / sizeof(wchar_t),L'\0');
    result = RegGetValueW(HKEY_CURRENT_USER,registryPath,name,RRF_RT_REG_SZ,nullptr,value.data(),&bytes);
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot read screensaver preferences");
    value.resize(wcslen(value.c_str()));
    return utf8(value);
}
void store(const wchar_t* name,const std::string& text) {
    const auto value = wide(text);
    const auto result = RegSetKeyValueW(HKEY_CURRENT_USER,registryPath,name,REG_SZ,value.c_str(),DWORD((value.size()+1)*sizeof(wchar_t)));
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot save screensaver preferences");
}
std::string hexColour(const Options& options) {
    char value[7];
    std::snprintf(value,sizeof(value),"%02x%02x%02x",int(options.colour[0]*255+.5f),int(options.colour[1]*255+.5f),int(options.colour[2]*255+.5f));
    return value;
}
Options preferences() {
    std::vector<std::string> arguments;
    const std::pair<const wchar_t*,const char*> entries[]{
        {L"Source","--source"},{L"Unit","--unit"},{L"FPS","--fps"},
        {L"Style","--style"},{L"Playback","--playback"},{L"SnapshotInterval","--snapshot-interval"},
        {L"Background","--background"},{L"Colour","--colour"}};
    for (const auto& entry : entries) {
        auto value = setting(entry.first);
        if (!value.empty()) { arguments.emplace_back(entry.second); arguments.push_back(std::move(value)); }
    }
    return liveScreensaverOptions(parseOptions(arguments));
}

enum Control { sourceControl=100,unitControl,fpsControl,styleControl,playbackControl,
    backgroundControl,colourControl,snapshotIntervalControl,snapshotIntervalLabel,snapshotIntervalUnits,validationControl };
struct Configuration { Options options; std::string error; };
std::wstring editText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(std::size_t(length)+1,L'\0');
    GetWindowTextW(control,text.data(),length+1); text.resize(length);
    return text;
}
std::string controlText(HWND dialog,int identifier) {
    return utf8(editText(GetDlgItem(dialog,identifier)));
}
std::optional<std::wstring> clipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return std::nullopt;
    struct Clipboard { ~Clipboard() { CloseClipboard(); } } clipboard;
    HGLOBAL data = GetClipboardData(CF_UNICODETEXT);
    if (!data) return std::nullopt;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) return std::nullopt;
    struct Locked { HGLOBAL data; ~Locked() { GlobalUnlock(data); } } locked{data};
    const auto count = GlobalSize(data)/sizeof(wchar_t);
    const auto* end = std::find(text,text+count,L'\0');
    if (end == text+count) return std::nullopt;
    return std::wstring(text,end);
}
bool decimalEditAllowed(HWND edit,std::wstring_view insertion) {
    DWORD first = 0,last = 0;
    SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
    return decimalReplacement(editText(edit),first,last,insertion);
}
LRESULT CALLBACK decimalEditProcedure(HWND edit,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR identifier,DWORD_PTR) {
    try {
        if (message == WM_CHAR && wparam >= 32) {
            if (wparam > 127) return 0;
            const wchar_t character = static_cast<wchar_t>(wparam);
            if (!decimalEditAllowed(edit,std::wstring_view(&character,1))) return 0;
        }
        if (message == WM_PASTE || (message == WM_CHAR && wparam == 22) ||
            (message == WM_KEYDOWN && wparam == VK_INSERT && GetKeyState(VK_SHIFT) < 0)) {
            const auto text = clipboardText(edit);
            if (text && decimalEditAllowed(edit,*text))
                SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(text->c_str()));
            return 0;
        }
    } catch (const std::exception&) {
        SetDlgItemTextW(GetParent(edit),validationControl,L"Cannot validate the interval text.");
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit,decimalEditProcedure,identifier);
    return DefSubclassProc(edit,message,wparam,lparam);
}
HWND control(HWND dialog,const wchar_t* type,const wchar_t* text,int identifier,int x,int y,int width,int height,DWORD style) {
    HWND handle = CreateWindowExW(wcscmp(type,L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0,type,text,
        WS_CHILD | WS_VISIBLE | style,x,y,width,height,dialog,reinterpret_cast<HMENU>(INT_PTR(identifier)),GetModuleHandleW(nullptr),nullptr);
    if (!handle) throw std::runtime_error("Cannot create configuration controls");
    SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    return handle;
}
void combo(HWND dialog,int identifier,int y,const std::vector<const wchar_t*>& labels,int selected) {
    HWND list = control(dialog,L"COMBOBOX",L"",identifier,170,y,370,200,WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto label : labels) SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    SendMessageW(list,CB_SETCURSEL,selected,0);
}
void updateSnapshotIntervalControls(HWND dialog) {
    const bool loop = SendDlgItemMessageW(dialog,playbackControl,CB_GETCURSEL,0,0) == 1;
    for (int identifier : {snapshotIntervalControl,snapshotIntervalLabel,snapshotIntervalUnits})
        ShowWindow(GetDlgItem(dialog,identifier),loop ? SW_SHOW : SW_HIDE);
}
INT_PTR CALLBACK configure(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state = reinterpret_cast<Configuration*>(GetWindowLongPtrW(dialog,DWLP_USER));
    int invalidControl = 0;
    try {
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<Configuration*>(lparam); SetWindowLongPtrW(dialog,DWLP_USER,lparam);
            SetWindowPos(dialog,nullptr,0,0,590,442,SWP_NOMOVE | SWP_NOZORDER);
            const auto& options = state->options;
            const auto label = [&](const wchar_t* text,int y) { control(dialog,L"STATIC",text,0,16,y+4,150,24,0); };
            label(L"Live source URL",18); control(dialog,L"EDIT",wide(options.source).c_str(),sourceControl,170,18,370,24,WS_TABSTOP | ES_AUTOHSCROLL);
            label(L"WU ID (optional)",54); control(dialog,L"EDIT",wide(options.unit).c_str(),unitControl,170,54,370,24,WS_TABSTOP | ES_AUTOHSCROLL);
            label(L"Frame-rate limit",90); control(dialog,L"EDIT",wide(std::to_string(options.fps)).c_str(),fpsControl,170,90,90,24,WS_TABSTOP | ES_NUMBER);
            control(dialog,L"STATIC",L"fps (1–240; target 60)",0,272,94,260,24,0);
            label(L"Protein snapshots",126); combo(dialog,playbackControl,126,{L"Latest snapshot",L"Loop available snapshots"},options.loop ? 1 : 0);
            control(dialog,L"STATIC",L"Snapshot interval",snapshotIntervalLabel,16,166,150,24,0);
            HWND interval = control(dialog,L"EDIT",wide(fah::decimalText(options.snapshotInterval)).c_str(),snapshotIntervalControl,170,162,90,24,WS_TABSTOP | ES_AUTOHSCROLL);
            if (!SetWindowSubclass(interval,decimalEditProcedure,1,0))
                throw std::runtime_error("Cannot create the interval input control");
            control(dialog,L"STATIC",L"seconds (minimum 0.05)",snapshotIntervalUnits,272,166,260,24,0);
            updateSnapshotIntervalControls(dialog);
            label(L"Model style",198); combo(dialog,styleControl,198,{L"Space-filling",L"Ball-and-stick",L"Sticks",L"Cycle all three",L"Random"},options.style-1);
            label(L"Background",234); combo(dialog,backgroundControl,234,{L"Light sky and grid",L"Dark sky and grid",L"Solid colour"},options.sky ? (options.darkSky ? 1 : 0) : 2);
            label(L"Solid colour (RGB)",270); control(dialog,L"EDIT",wide(hexColour(options)).c_str(),colourControl,170,270,120,24,WS_TABSTOP | ES_AUTOHSCROLL);
            control(dialog,L"STATIC",L"Use six hexadecimal digits, for example 101820.",0,170,300,370,24,0);
            control(dialog,L"STATIC",L"",validationControl,16,326,524,24,0);
            control(dialog,L"STATIC",wide("Version " FAH_SAVER_VERSION).c_str(),0,16,360,300,24,0);
            control(dialog,L"BUTTON",L"Save",IDOK,364,354,82,28,WS_TABSTOP | BS_DEFPUSHBUTTON);
            control(dialog,L"BUTTON",L"Cancel",IDCANCEL,458,354,82,28,WS_TABSTOP | BS_PUSHBUTTON);
            return TRUE;
        }
        if (message == WM_CTLCOLORSTATIC && GetDlgCtrlID(reinterpret_cast<HWND>(lparam)) == validationControl) {
            HDC context = reinterpret_cast<HDC>(wparam);
            SetTextColor(context,RGB(176,32,32)); SetBkMode(context,TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        if (message == WM_COMMAND && (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE))
            SetDlgItemTextW(dialog,validationControl,L"");
        if (message == WM_COMMAND && LOWORD(wparam) == playbackControl && HIWORD(wparam) == CBN_SELCHANGE) {
            updateSnapshotIntervalControls(dialog); return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
            invalidControl = sourceControl;
            const auto source = controlText(dialog,sourceControl);
            if (source.empty()) throw std::runtime_error("Set a live source URL");
            if (source.rfind("ws://",0) != 0 && source.rfind("wss://",0) != 0)
                throw std::runtime_error("Source URL must begin with ws:// or wss://");
            Options next = state->options;
            const auto apply = [&](int identifier,const char* option,const std::string& value) {
                invalidControl = identifier; next = parseOptions({option,value},next);
            };
            apply(sourceControl,"--source",source);
            apply(unitControl,"--unit",controlText(dialog,unitControl));
            try { apply(fpsControl,"--fps",controlText(dialog,fpsControl)); }
            catch (const std::exception&) { throw std::runtime_error("Enter a whole frame-rate limit from 1 to 240 fps."); }
            const int style = int(SendDlgItemMessageW(dialog,styleControl,CB_GETCURSEL,0,0))+1;
            const std::string styleName = style == 4 ? "cycle" : style == 5 ? "random" : std::to_string(style);
            apply(styleControl,"--style",styleName);
            const auto playback = SendDlgItemMessageW(dialog,playbackControl,CB_GETCURSEL,0,0);
            const auto background = SendDlgItemMessageW(dialog,backgroundControl,CB_GETCURSEL,0,0);
            invalidControl = playbackControl;
            if (playback < 0 || playback > 1) throw std::runtime_error("Select valid snapshot playback");
            const bool loop = playback == 1;
            apply(playbackControl,"--playback",loop ? "loop" : "latest");
            if (loop) {
                invalidControl = snapshotIntervalControl;
                try {
                    const auto text = controlText(dialog,snapshotIntervalControl);
                    if (!fah::decimalPartial(text)) throw std::runtime_error("Invalid decimal text");
                    apply(snapshotIntervalControl,"--snapshot-interval",text);
                } catch (const std::exception&) {
                    throw std::runtime_error("Enter a finite interval of at least 0.05 seconds.");
                }
            }
            invalidControl = backgroundControl;
            if (background < 0 || background > 2) throw std::runtime_error("Select a valid background");
            const std::string backgroundName = background == 0 ? "sky" : background == 1 ? "dark" : "solid";
            apply(backgroundControl,"--background",backgroundName);
            apply(colourControl,"--colour",controlText(dialog,colourControl));
            invalidControl = 0;
            store(L"Source",source); store(L"Unit",next.unit);
            store(L"FPS",std::to_string(next.fps)); store(L"Style",styleName);
            store(L"Playback",loop ? "loop" : "latest");
            store(L"SnapshotInterval",fah::decimalText(next.snapshotInterval)); store(L"Background",backgroundName);
            store(L"Colour",hexColour(next));
            state->options = std::move(next);
            EndDialog(dialog,IDOK); return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL) { EndDialog(dialog,IDCANCEL); return TRUE; }
    } catch (const std::exception& error) {
        if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
            SetDlgItemTextW(dialog,validationControl,wide(error.what()).c_str());
            if (invalidControl) {
                HWND field = GetDlgItem(dialog,invalidControl); SetFocus(field);
                wchar_t type[16]{};
                if (GetClassNameW(field,type,16) && _wcsicmp(type,L"EDIT") == 0) SendMessageW(field,EM_SETSEL,0,-1);
            }
        } else { errorBox(dialog,error.what()); EndDialog(dialog,IDCANCEL); }
        return TRUE;
    }
    return FALSE;
}
void configuration(HWND owner,Options options) {
    options = liveScreensaverOptions(std::move(options));
    struct Template { DLGTEMPLATE dialog; WORD menu; WORD windowClass; wchar_t title[10]; } layout{};
    layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    layout.dialog.cx = 340; layout.dialog.cy = 240;
    std::copy_n(L"FAH Saver",10,layout.title);
    Configuration state{std::move(options),{}};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,owner,configure,reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("Cannot open screensaver configuration");
}

double processSeconds() {
    FILETIME created,exited,kernel,user;
    if (!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))
        throw std::runtime_error("Cannot read process CPU time");
    const auto value = [](FILETIME time) { return (std::uint64_t(time.dwHighDateTime)<<32) | time.dwLowDateTime; };
    return double(value(kernel)+value(user))/1e7;
}
struct BenchmarkPoint { Clock::time_point wall; double processCPU,threadCPU; };
BenchmarkPoint benchmarkPoint() { return {Clock::now(),processSeconds(),threadCpuMilliseconds()/1000}; }
void writeInterval(std::ostream& output,const char* name,const BenchmarkPoint& begin,const BenchmarkPoint& end,const char* scope) {
    output << ",\n  \"" << name << "\":{\"elapsed_seconds\":" << std::chrono::duration<double>(end.wall-begin.wall).count()
        << ",\"cpu_process_seconds\":" << end.processCPU-begin.processCPU
        << ",\"main_thread_cpu_seconds\":" << end.threadCPU-begin.threadCPU << ",\"scope\":\"" << scope << "\"}";
}
std::string escaped(const std::string& text) {
    std::string result;
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c >= 32) result += char(c);
        else throw std::runtime_error("Unexpected control character in adapter name");
    }
    return result;
}

std::filesystem::path snapshotCachePath() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
    if (!required) throw std::runtime_error("Cannot locate LocalAppData for the model cache");
    std::wstring directory(required,L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA",directory.data(),required);
    if (!length || length >= required) throw std::runtime_error("Cannot read LocalAppData for the model cache");
    directory.resize(length);
    return std::filesystem::path(directory) / L"FAHSaver" / L"latest-model.cache";
}
void cacheDiagnostic(const std::string& text) {
    OutputDebugStringA(("FAHSaver cache: "+text+"\n").c_str());
}

struct App {
    HWND window = nullptr,status = nullptr;
    Options options;
    fah::Capture capture;
    fah::LiveModel liveModel;
    std::unique_ptr<fah::SnapshotCache> cache;
    std::thread cacheDrain;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<fah::Source> source;
    bool fullscreen = false,preview = false,redraw = true,failed = false;
    bool benchmark = false;
    std::atomic<bool> deliveryFailed{false};
    POINT initialMouse{};
    std::uint64_t revision = 0,lastRevision = UINT64_MAX,frameCount = 0,gpuCount = 0;
    std::uint64_t pumpIterations = 0,messagesDispatched = 0,renderCalls = 0,timerWakes = 0,inputQueueWakes = 0;
    PhaseTiming sceneBuildUpload,fenceQueryWait,acquire,recordSubmit,present;
    unsigned drawableWidth = 0,drawableHeight = 0;
    std::size_t lastFrame = SIZE_MAX;
    int lastStyle = 0,randomStyle = 1;
    std::uint64_t stylePeriod = UINT64_MAX;
    std::mt19937 random{std::random_device{}()};
    Clock::time_point started = Clock::now();
    Clock::time_point firstSubmission,lastSubmission;
    BenchmarkPoint activeEnd{},shutdownEnd{};
    bool activeEnded = false,shutdownEnded = false;
    double cpuStarted = 0,cpuDrawTotal = 0,gpuTotal = 0,cpuDrawMaximum = 0;
    double threadCpuStarted = 0;
    std::string failure;
    std::string adapterName;

    ~App() { if (cacheDrain.joinable()) cacheDrain.join(); }

    const fah::Capture* displayedCapture() const {
        return options.capture.empty() ? liveModel.capture().get() : &capture;
    }
    bool hasModel() const {
        const auto* value = displayedCapture();
        return value && !value->frames.empty();
    }

    void message(const std::string& text) {
        SetWindowTextW(status,wide(text).c_str()); ShowWindow(status,text.empty() ? SW_HIDE : SW_SHOW);
    }
    void sourceStatus(const std::string& text) {
        if (hasModel()) {
            if (!text.empty()) OutputDebugStringA(("FAHSaver source: "+text+"\n").c_str());
            message("");
        } else message(text);
    }
    void prepareCache() {
        try {
            std::string selector = options.source;
            selector.push_back('\0'); selector += options.unit;
            cache = std::make_unique<fah::SnapshotCache>(snapshotCachePath(),std::move(selector),cacheDiagnostic);
        } catch (const std::exception& error) { cacheDiagnostic(error.what()); }
        if (!cache) return;
        try {
            if (auto snapshot = cache->load()) liveModel.seed(std::move(snapshot));
            revision = liveModel.revision();
        } catch (const std::exception& error) { cacheDiagnostic(error.what()); }
    }
    void start() {
        GetCursorPos(&initialMouse);
        benchmark = !options.metrics.empty();
        renderer = std::make_unique<Renderer>(window,benchmark);
        adapterName = renderer->adapter();
        if (!options.capture.empty()) { capture = fah::readCapture(options.capture); ++revision; message(""); }
        else if (!options.source.empty()) {
            prepareCache();
            const HWND target = window;
            source = fah::makeSource({options.source,options.unit},[this,target](fah::SourceEvent event) {
                auto value = std::make_unique<fah::SourceEvent>(std::move(event));
                if (PostMessageW(target,sourceMessage,0,reinterpret_cast<LPARAM>(value.get()))) value.release();
                else deliveryFailed = true;
            });
            sourceStatus("Connecting to FAH…");
        } else message("Set a live source URL in Screen Saver Settings → Settings.");
        started = Clock::now();
        if (benchmark) { cpuStarted = processSeconds(); threadCpuStarted = threadCpuMilliseconds(); }
    }
    void stop() {
        if (source) { source->stop(); source.reset(); }
        MSG event;
        while (PeekMessageW(&event,window,sourceMessage,sourceMessage,PM_REMOVE))
            delete reinterpret_cast<fah::SourceEvent*>(event.lParam);
        if (cache) cacheDrain = std::thread([pending = std::move(cache)]() mutable { pending.reset(); });
    }
    void snapshotActiveEnd() {
        if (benchmark && !activeEnded) { activeEnd = benchmarkPoint(); activeEnded = true; }
    }
    void snapshotShutdownEnd() {
        if (benchmark && !shutdownEnded) { shutdownEnd = benchmarkPoint(); shutdownEnded = true; }
    }
    void receive(fah::SourceEvent event) {
        const bool completeFrame = liveModel.receive(event,options.loop);
        if (revision != liveModel.revision()) {
            revision = liveModel.revision(); redraw = true;
            if (!hasModel()) renderer->setScene({});
        }
        if (completeFrame && cache) {
            try { cache->queue(liveModel.latestTopology(),liveModel.latestFrame()); }
            catch (const std::exception& error) { cacheDiagnostic(error.what()); }
        }
        switch (event.kind) {
        case fah::SourceEvent::Kind::Reset:
            sourceStatus("Waiting for current work-unit visualisation…"); break;
        case fah::SourceEvent::Kind::Topology: break;
        case fah::SourceEvent::Kind::Frame: message(""); break;
        case fah::SourceEvent::Kind::Status:
        case fah::SourceEvent::Kind::Error: sourceStatus(event.text); break;
        }
    }
    void render() {
        if (benchmark) ++renderCalls;
        if (deliveryFailed) throw std::runtime_error("Cannot queue FAH updates for the screensaver window");
        const double elapsed = std::chrono::duration<double>(Clock::now()-started).count();
        if (options.seconds > 0 && elapsed >= options.seconds) { PostMessageW(window,WM_CLOSE,0,0); return; }
        if (IsIconic(window)) return;
        const auto* shown = displayedCapture();
        if (shown && !shown->frames.empty()) {
            const std::size_t frame = snapshotIndex(elapsed,shown->frames.size(),options);
            int style = options.style;
            const auto period = std::uint64_t(elapsed/10);
            if (style == 4) style = int(period%3)+1;
            if (style == 5) {
                if (period != stylePeriod) { randomStyle = std::uniform_int_distribution<int>(1,3)(random); stylePeriod = period; }
                style = randomStyle;
            }
            if (lastRevision != revision || lastFrame != frame || lastStyle != style) {
                PhaseTimer sceneTimer(benchmark);
                renderer->setScene(fah::makeScene(shown->topology,shown->frames[frame],static_cast<fah::Style>(style)));
                if (benchmark) addPhase(sceneBuildUpload,sceneTimer.finish());
                lastRevision = revision; lastFrame = frame; lastStyle = style;
            }
        } else if (!redraw) return;
        const auto timing = renderer->draw(float(elapsed*.2),options.sky,options.darkSky,options.colour);
        redraw = false;
        if (benchmark) {
            addPhase(fenceQueryWait,timing.fenceQueryWait); addPhase(acquire,timing.acquire);
            addPhase(recordSubmit,timing.recordSubmit); addPhase(present,timing.present);
        }
        if (benchmark && timing.rendered) {
            ++frameCount; cpuDrawTotal += timing.cpuMilliseconds;
            if (frameCount == 1) firstSubmission = timing.submittedAt;
            lastSubmission = timing.submittedAt;
            cpuDrawMaximum = std::max(cpuDrawMaximum,timing.cpuMilliseconds);
            if (timing.gpuMilliseconds >= 0) { ++gpuCount; gpuTotal += timing.gpuMilliseconds; }
        }
    }
    void metrics() {
        if (options.metrics.empty()) return;
        if (!activeEnded || !shutdownEnded) throw std::runtime_error("Benchmark shutdown snapshots are missing");
        const BenchmarkPoint begin{started,cpuStarted,threadCpuStarted/1000},lifecycleEnd = benchmarkPoint();
        const double elapsed = std::chrono::duration<double>(lifecycleEnd.wall-started).count();
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)))
            throw std::runtime_error("Cannot read process memory metrics");
        std::ofstream output(std::filesystem::u8path(options.metrics));
        if (!output) throw std::runtime_error("Cannot open metrics output file");
        output << std::setprecision(10) << "{\n  \"backend\":\"vulkan\",\n  \"adapter\":\"" << escaped(adapterName)
            << "\",\n  \"elapsed_seconds\":" << elapsed << ",\n  \"submitted_frames\":" << frameCount
            << ",\n  \"snapshot_interval_seconds\":" << options.snapshotInterval
            << ",\n  \"snapshot_playback\":\"" << (options.loop ? "loop" : "latest") << '"'
            << ",\n  \"cached_model_shown\":" << (options.capture.empty() && liveModel.usingFallback() ? "true" : "false")
            << ",\n  \"background\":\"" << (options.sky ? (options.darkSky ? "dark" : "sky") : "solid") << '"'
            << ",\n  \"submit_rate_fps\":" << (elapsed > 0 ? frameCount/elapsed : 0)
            << ",\n  \"cpu_process_seconds\":" << lifecycleEnd.processCPU-cpuStarted
            << ",\n  \"cpu_draw_call_mean_ms\":" << (frameCount ? cpuDrawTotal/frameCount : 0)
            << ",\n  \"cpu_draw_call_max_ms\":" << cpuDrawMaximum
            << ",\n  \"gpu_timestamp_samples\":" << gpuCount << ",\n  \"gpu_mean_ms\":";
        if (gpuCount) output << gpuTotal/gpuCount; else output << "null";
        output << ",\n  \"peak_working_set_bytes\":" << memory.PeakWorkingSetSize
            << ",\n  \"drawable_width\":" << drawableWidth << ",\n  \"drawable_height\":" << drawableHeight;
        writeInterval(output,"lifecycle_interval",begin,lifecycleEnd,"Post-setup start to metrics collection; includes active interval and shutdown, excludes initialisation and result writing");
        writeInterval(output,"active_interval",begin,activeEnd,"Post-setup start to WM_DESTROY entry, before source stop and renderer destruction");
        writeInterval(output,"shutdown_interval",activeEnd,shutdownEnd,"WM_DESTROY entry through source stop and renderer destruction");
        output << ",\n  \"legacy_top_level_timing_scope\":\"elapsed_seconds, cpu_process_seconds and submit_rate_fps use lifecycle_interval; the legacy rate is submissions per lifecycle second, not display FPS\""
            << ",\n  \"submission_call_cadence\":{\"scope\":\"Successful vkQueueSubmit return timestamps; excludes initialisation and teardown; not displayed-frame timestamps\",\"interval_count\":" << (frameCount ? frameCount-1 : 0)
            << ",\"first_seconds_after_active_start\":";
        if (frameCount) output << std::chrono::duration<double>(firstSubmission-started).count(); else output << "null";
        output << ",\"last_seconds_after_active_start\":";
        if (frameCount) output << std::chrono::duration<double>(lastSubmission-started).count(); else output << "null";
        output << ",\"interval_seconds\":";
        const double cadence = frameCount > 1 ? std::chrono::duration<double>(lastSubmission-firstSubmission).count() : 0;
        if (frameCount > 1) output << cadence; else output << "null";
        output << ",\"calls_per_second\":";
        if (cadence > 0) output << (frameCount-1)/cadence; else output << "null";
        output << "},\n  \"diagnostics\":{\n    \"main_thread_cpu_seconds\":" << lifecycleEnd.threadCPU-begin.threadCPU
            << ",\n    \"main_thread_cpu_scope\":\"lifecycle_interval\""
            << ",\n    \"pump_iterations\":" << pumpIterations << ",\n    \"messages_dispatched\":" << messagesDispatched
            << ",\n    \"render_calls\":" << renderCalls << ",\n    \"timer_wakes\":" << timerWakes
            << ",\n    \"input_queue_wakes\":" << inputQueueWakes << ",\n    \"phase_totals\":{";
        const std::pair<const char*,const PhaseTiming*> phases[]{
            {"scene_build_upload",&sceneBuildUpload},{"fence_query_wait",&fenceQueryWait},
            {"acquire",&acquire},{"record_submit",&recordSubmit},{"present",&present}};
        bool separator = false;
        for (const auto& entry : phases) {
            if (separator) output << ','; separator = true;
            output << "\n      \"" << entry.first << "\":{\"calls\":" << entry.second->calls
                << ",\"wall_ms\":" << entry.second->wallMilliseconds
                << ",\"main_thread_cpu_ms\":" << entry.second->threadCpuMilliseconds << '}';
        }
        output << "\n    },\n    \"scope\":\"Aggregates only; scene phase includes its upload fence wait; input queue includes all GUI messages; diagnostic sampling overhead is included\"\n  },"
            << "\n  \"presented_frame_timestamps\":null,\n  \"power_watts\":null\n}\n";
        output.close();
        if (!output) throw std::runtime_error("Cannot write metrics output file");
    }
};

LRESULT CALLBACK windowProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window,message,wparam,lparam);
    try {
        switch (message) {
        case WM_CREATE:
            app->status = CreateWindowExW(0,L"STATIC",L"Loading…",WS_CHILD | WS_VISIBLE | SS_LEFT,
                24,24,900,100,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            if (!app->status) throw std::runtime_error("Cannot create source status display");
            SendMessageW(app->status,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            return 0;
        case sourceMessage: {
            std::unique_ptr<fah::SourceEvent> event(reinterpret_cast<fah::SourceEvent*>(lparam));
            app->receive(std::move(*event)); return 0;
        }
        case WM_SIZE: {
            RECT bounds;
            if (!GetClientRect(window,&bounds)) throw std::runtime_error("Cannot read native drawable size");
            app->drawableWidth = unsigned(std::max(0L,bounds.right)); app->drawableHeight = unsigned(std::max(0L,bounds.bottom));
            if (app->renderer) app->renderer->resize();
            if (app->status) MoveWindow(app->status,24,24,std::max(1,int(bounds.right)-48),100,TRUE);
            app->redraw = true; return 0;
        }
        case WM_DISPLAYCHANGE:
            if (app->fullscreen && !SetWindowPos(window,HWND_TOPMOST,
                GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),
                SWP_NOACTIVATE | SWP_NOOWNERZORDER))
                throw std::runtime_error("Cannot resize screensaver after a display change");
            app->redraw = true; return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255));
            SetBkColor(reinterpret_cast<HDC>(wparam),RGB(0,0,0));
            return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (!app->preview && (app->fullscreen || wparam == VK_ESCAPE)) DestroyWindow(window);
            return 0;
        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (app->fullscreen) DestroyWindow(window);
            return message == WM_XBUTTONDOWN ? TRUE : 0;
        case WM_MOUSEMOVE:
            if (app->fullscreen) {
                POINT point; GetCursorPos(&point);
                if (std::abs(point.x-app->initialMouse.x) > 4 || std::abs(point.y-app->initialMouse.y) > 4) DestroyWindow(window);
            }
            return 0;
        case WM_ACTIVATE:
            if (app->fullscreen && LOWORD(wparam) == WA_INACTIVE) DestroyWindow(window);
            return 0;
        case WM_SETCURSOR:
            if (app->fullscreen) { SetCursor(nullptr); return TRUE; } break;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            app->snapshotActiveEnd(); app->stop(); app->renderer.reset(); app->snapshotShutdownEnd();
            PostQuitMessage(0); return 0;
        }
    } catch (const std::exception& error) {
        app->failed = true; app->failure = error.what(); PostMessageW(window,WM_CLOSE,0,0); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

HWND parentHandle(const std::wstring& text,bool required = true) {
    std::size_t end = 0;
    const auto value = std::stoull(text,&end,10);
    if (end != text.size() || value > UINTPTR_MAX || (!value && required)) throw std::runtime_error("Invalid parent window handle");
    if (!value) return nullptr;
    HWND parent = reinterpret_cast<HWND>(std::uintptr_t(value));
    if (!IsWindow(parent)) throw std::runtime_error("Preview parent window no longer exists");
    return parent;
}

int run(const std::vector<std::wstring>& arguments) {
    App app;
    app.options = preferences();
    HWND parent = nullptr;
    if (!arguments.empty() && arguments.front().size() >= 2 && arguments.front()[0] == L'/') {
        const auto mode = towlower(arguments.front()[1]);
        if (mode == L'c') {
            if (arguments.front().size() > 3 && arguments.front()[2] == L':') parent = parentHandle(arguments.front().substr(3),false);
            else if (arguments.size() > 1) parent = parentHandle(arguments[1],false);
            configuration(parent,app.options); return 0;
        }
        if (mode == L's' && arguments.front().size() == 2 && arguments.size() == 1) app.fullscreen = true;
        else if (mode == L'p') {
            if (arguments.front().size() > 3 && arguments.front()[2] == L':') parent = parentHandle(arguments.front().substr(3));
            else if (arguments.size() == 2) parent = parentHandle(arguments[1]);
            else throw std::runtime_error("Preview mode requires its parent window handle");
            app.preview = true;
        } else throw std::runtime_error("Unsupported screensaver argument");
    } else {
        std::vector<std::string> options;
        for (const auto& argument : arguments) options.push_back(utf8(argument));
        app.options = parseOptions(options,app.options);
        app.fullscreen = app.options.fullscreen;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW windowClass{}; windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr); windowClass.lpszClassName = L"FAHSaverWindow";
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassW(&windowClass)) throw std::runtime_error("Cannot register screensaver window");
    int x = CW_USEDEFAULT,y = CW_USEDEFAULT,width = int(app.options.width),height = int(app.options.height);
    DWORD style = WS_OVERLAPPEDWINDOW,extended = 0;
    if (app.preview) {
        RECT bounds; if (!GetClientRect(parent,&bounds)) throw std::runtime_error("Cannot read preview size");
        x = y = 0; width = bounds.right; height = bounds.bottom; style = WS_CHILD;
    } else if (app.fullscreen) {
        // shortcut: one surface spans the desktop; use per-monitor surfaces for independent refresh rates.
        x = GetSystemMetrics(SM_XVIRTUALSCREEN); y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN); height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        style = WS_POPUP; extended = WS_EX_TOPMOST;
    } else {
        RECT bounds{0,0,width,height};
        if (!AdjustWindowRectExForDpi(&bounds,style,FALSE,extended,GetDpiForSystem()))
            throw std::runtime_error("Cannot calculate native window size for requested client pixels");
        width = bounds.right-bounds.left; height = bounds.bottom-bounds.top;
    }
    const HWND window = CreateWindowExW(extended,windowClass.lpszClassName,L"FAH Saver — Vulkan",style,
        x,y,width,height,parent,nullptr,windowClass.hInstance,&app);
    if (!window) throw std::runtime_error("Cannot create screensaver window");
    try {
        if (!app.preview && !app.fullscreen) {
            RECT bounds{0,0,LONG(app.options.width),LONG(app.options.height)};
            if (!AdjustWindowRectExForDpi(&bounds,style,FALSE,extended,GetDpiForWindow(window)) ||
                !SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
                throw std::runtime_error("Cannot set native client pixel dimensions");
        }
        app.start(); ShowWindow(window,SW_SHOW); UpdateWindow(window);
        if (app.liveModel.available()) app.render();
        if (app.source) app.source->start();
        const HANDLE timer = CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
        if (!timer) throw std::runtime_error("High-resolution frame timer unavailable; Windows 10 version 1803 or later is required");
        struct Timer { HANDLE value; ~Timer() { CloseHandle(value); } } timerOwner{timer};
        bool running = true;
        auto next = Clock::now();
        while (running) {
            if (app.benchmark) ++app.pumpIterations;
            MSG message;
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                if (message.message == WM_QUIT) { running = false; break; }
                if (app.benchmark) ++app.messagesDispatched;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (!running) break;
            if (Clock::now() >= next) {
                app.render();
                const double interval = app.hasModel() ? 1.0/app.options.fps : .2;
                next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval));
                if (next < Clock::now()) next = Clock::now();
            }
            const auto remaining = std::max(std::chrono::nanoseconds(100),std::chrono::duration_cast<std::chrono::nanoseconds>(next-Clock::now()));
            LARGE_INTEGER due; due.QuadPart = -std::max<LONGLONG>(1,remaining.count()/100);
            if (!SetWaitableTimerEx(timer,&due,0,nullptr,nullptr,nullptr,0)) throw std::runtime_error("Cannot schedule frame timer");
            const DWORD wait = MsgWaitForMultipleObjectsEx(1,&timer,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED) throw std::runtime_error("Cannot wait for frames or window events");
            if (app.benchmark && wait == WAIT_OBJECT_0) ++app.timerWakes;
            if (app.benchmark && wait == WAIT_OBJECT_0+1) ++app.inputQueueWakes;
        }
        app.stop(); app.metrics();
        if (app.failed) throw std::runtime_error(app.failure);
    } catch (...) {
        app.stop(); if (IsWindow(window)) DestroyWindow(window); throw;
    }
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        int count = 0;
        wchar_t** values = CommandLineToArgvW(GetCommandLineW(),&count);
        if (!values) throw std::runtime_error("Cannot read command-line arguments");
        std::vector<std::wstring> arguments;
        for (int i = 1; i < count; ++i) arguments.emplace_back(values[i]);
        LocalFree(values);
        return run(arguments);
    } catch (const std::exception& error) { errorBox(nullptr,error.what()); return 1; }
}
