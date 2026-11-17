#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winver.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr wchar_t desktopKey[] = L"Control Panel\\Desktop";
constexpr wchar_t installerKey[] = L"Software\\FAHSaver\\Installer";
constexpr wchar_t arpKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FAHSaver";
constexpr wchar_t markerName[] = L".fah-installer-test";
constexpr char markerText[] = "FAH Saver installer lifecycle fixture v1\n";
const std::array<const wchar_t*, 4> desktopValues{
    L"SCRNSAVE.EXE", L"ScreenSaveActive", L"ScreenSaveTimeOut", L"ScreenSaverIsSecure"};
const std::array<const wchar_t*, 24> managedFiles{
    L"FAHSaver.scr",
    L"Uninstall.exe",
    L"LICENSE",
    L"COPYRIGHT",
    L"THIRD-PARTY-NOTICES.md",
    L"SOURCE.txt",
    L"shaders\\model.vert.spv",
    L"shaders\\model.frag.spv",
    L"shaders\\background.vert.spv",
    L"shaders\\background.frag.spv",
    L"nlohmann-json-LICENSE.MIT",
    L"licences\\libcxx-LICENSE.TXT",
    L"licences\\libcxxabi-LICENSE.TXT",
    L"licences\\libunwind-LICENSE.TXT",
    L"licences\\mingw-runtime-COPYING.txt",
    L"licences\\zig-compiler-runtime-LICENSE.MIT",
    L"licences\\LGPL-2.1.txt",
    L"licences\\NSIS-COPYING.txt",
    L"licences\\NSIS-Modern-UI-LICENSE.txt",
    L"licences\\Vulkan-Headers-LICENSE.md",
    L"licences\\Vulkan-Headers-MIT.txt",
    L"licences\\mingw-DISCLAIMER.PD",
    L"licences\\mingw-gdtoa-LICENSE.txt",
    L"licences\\winhttp-header-NOTICE.txt"};

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void winRequire(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string(message) + " (Win32 " + std::to_string(GetLastError()) + ")");
}
static std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    winRequire(size > 0, "UTF-8 length conversion failed");
    std::string result(size, '\0');
    winRequire(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size, nullptr, nullptr) == size,
               "UTF-8 conversion failed");
    return result;
}
static std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    winRequire(size > 0, "UTF-16 length conversion failed");
    std::wstring result(size, L'\0');
    winRequire(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size) == size,
               "UTF-16 conversion failed");
    return result;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
static std::wstring fullPath(const fs::path& path) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(path.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
    winRequire(length && length < buffer.size(), "Path normalisation failed");
    std::wstring result(buffer.data(), length);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}
static bool equalText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}
static std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
static std::string hex(const std::vector<unsigned char>& data) {
    static const char digits[] = "0123456789abcdef";
    std::string result; result.reserve(data.size() * 2);
    for (unsigned char byte : data) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
static std::vector<unsigned char> unhex(const std::string& text) {
    require(text.size() % 2 == 0 && text.size() <= 8 * 1024 * 1024, "Invalid snapshot data length");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid snapshot hexadecimal data");
    };
    std::vector<unsigned char> result;
    for (std::size_t i = 0; i < text.size(); i += 2) result.push_back(static_cast<unsigned char>((digit(text[i]) << 4) | digit(text[i + 1])));
    return result;
}
static std::vector<unsigned char> readFile(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    winRequire(file.value != INVALID_HANDLE_VALUE, "File read open failed");
    LARGE_INTEGER size{};
    winRequire(GetFileSizeEx(file.value, &size), "File size query failed");
    require(size.QuadPart >= 0 && size.QuadPart <= 64 * 1024 * 1024, "File is outside test size limits");
    std::vector<unsigned char> data(static_cast<std::size_t>(size.QuadPart));
    DWORD received = 0;
    winRequire(ReadFile(file.value, data.data(), DWORD(data.size()), &received, nullptr) && received == data.size(), "File read failed");
    return data;
}
static void writeFile(const fs::path& path, const std::string& text, bool exclusive = false) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, exclusive ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    winRequire(file.value != INVALID_HANDLE_VALUE, "File write open failed");
    DWORD written = 0;
    winRequire(WriteFile(file.value, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size(), "File write failed");
    winRequire(FlushFileBuffers(file.value), "File flush failed");
}
static std::string sha256(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider unavailable");
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> result{};
    const NTSTATUS created = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    NTSTATUS status = created;
    if (status >= 0) status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
    if (status >= 0) status = BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    require(status >= 0, "SHA256 calculation failed");
    return hex({result.begin(), result.end()});
}
static std::string sha256(const std::string& text) { return sha256(std::vector<unsigned char>(text.begin(), text.end())); }

static Json readValue(const std::wstring& path, const std::wstring& name) {
    Key key;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return nullptr;
    require(status == ERROR_SUCCESS, "Registry open failed");
    DWORD type = 0, size = 0;
    status = RegQueryValueExW(key.value, name.c_str(), nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return nullptr;
    require(status == ERROR_SUCCESS && size <= 4 * 1024 * 1024, "Registry value size query failed");
    std::vector<unsigned char> data(size);
    require(RegQueryValueExW(key.value, name.c_str(), nullptr, &type, data.data(), &size) == ERROR_SUCCESS && size == data.size(),
            "Registry value changed while reading");
    return Json{{"type", type}, {"data", hex(data)}};
}
static void setValue(const std::wstring& path, const std::wstring& name, const Json& value) {
    if (readValue(path, name) == value) return;
    Key key;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                            nullptr, &key.value, nullptr) == ERROR_SUCCESS, "Registry write open failed");
    if (value.is_null()) {
        const LONG status = RegDeleteValueW(key.value, name.c_str());
        require(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND, "Registry value removal failed");
    } else {
        auto data = unhex(value.at("data").get<std::string>());
        require(RegSetValueExW(key.value, name.c_str(), 0, value.at("type").get<DWORD>(), data.data(), DWORD(data.size())) == ERROR_SUCCESS,
                "Registry value restore failed");
    }
}
static Json stringValue(const std::wstring& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str());
    return Json{{"type", REG_SZ}, {"data", hex({bytes, bytes + (text.size() + 1) * sizeof(wchar_t)})}};
}
static std::wstring valueString(const Json& value) {
    require(!value.is_null() && (value.at("type") == REG_SZ || value.at("type") == REG_EXPAND_SZ), "Expected registry string");
    auto bytes = unhex(value.at("data").get<std::string>());
    require(bytes.size() >= 2 && bytes.size() % 2 == 0 && !bytes.back() && !bytes[bytes.size() - 2], "Invalid registry string");
    std::wstring result(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(result.data(), bytes.data(), bytes.size()); result.pop_back();
    return result;
}
static Json readTree(const std::wstring& path, bool omitInstaller = false, unsigned depth = 0) {
    require(depth < 12, "Registry test snapshot is too deep");
    Key key;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return Json{{"exists", false}};
    require(status == ERROR_SUCCESS, "Registry tree open failed");
    Json values = Json::object(), children = Json::object();
    for (DWORD index = 0;; ++index) {
        require(index < 4096, "Registry test snapshot has too many values");
        std::array<wchar_t, 16384> name{}; DWORD size = DWORD(name.size());
        status = RegEnumValueW(key.value, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        require(status == ERROR_SUCCESS, "Registry value enumeration failed");
        std::wstring entry(name.data(), size); values[utf8(entry)] = readValue(path, entry);
    }
    for (DWORD index = 0;; ++index) {
        require(index < 4096, "Registry test snapshot has too many subkeys");
        std::array<wchar_t, 256> name{}; DWORD size = DWORD(name.size());
        status = RegEnumKeyExW(key.value, index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        require(status == ERROR_SUCCESS, "Registry subkey enumeration failed");
        std::wstring entry(name.data(), size);
        if (!omitInstaller || !equalText(entry, L"Installer")) children[utf8(entry)] = readTree(path + L"\\" + entry, false, depth + 1);
    }
    return Json{{"exists", true}, {"values", values}, {"children", children}};
}
static void deleteTree(const std::wstring& path) {
    Key parent;
    const auto slash = path.find_last_of(L'\\');
    require(slash != std::wstring::npos, "Invalid registry snapshot path");
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, path.substr(0, slash).c_str(), 0,
                                     KEY_READ | KEY_WRITE | DELETE | KEY_WOW64_64KEY, &parent.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
    require(opened == ERROR_SUCCESS, "Registry snapshot parent open failed");
    const LONG status = RegDeleteTreeW(parent.value, path.substr(slash + 1).c_str());
    require(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND, "Registry snapshot removal failed");
}
static void writeTree(const std::wstring& path, const Json& tree, unsigned depth = 0) {
    require(depth < 12, "Invalid registry recovery depth");
    if (!tree.at("exists").get<bool>()) return;
    Key key;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                            nullptr, &key.value, nullptr) == ERROR_SUCCESS, "Registry snapshot key restore failed");
    for (auto it = tree.at("values").begin(); it != tree.at("values").end(); ++it) setValue(path, wide(it.key()), it.value());
    for (auto it = tree.at("children").begin(); it != tree.at("children").end(); ++it) {
        const auto name = wide(it.key()); require(name.find(L'\\') == std::wstring::npos, "Invalid registry recovery subkey");
        writeTree(path + L"\\" + name, it.value(), depth + 1);
    }
}
static Json desktopSnapshot() {
    Json result = Json::object();
    for (const auto* name : desktopValues) result[utf8(name)] = readValue(desktopKey, name);
    return result;
}
static bool active() {
    BOOL value = FALSE;
    winRequire(SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &value, 0), "Cannot query screen saver activation");
    return value != FALSE;
}
static void setActive(bool value) {
    if (active() != value) winRequire(SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, value ? TRUE : FALSE, nullptr, SPIF_SENDCHANGE),
                                     "Cannot restore screen saver activation");
}
static Json preservedUserData() {
    Json preferences = readTree(L"Software\\FAHSaver", true);
    if (preferences.at("exists").get<bool>() && preferences.at("values").empty() && preferences.at("children").empty())
        preferences = Json{{"exists", false}};
    std::array<wchar_t, 32768> local{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), DWORD(local.size()));
    winRequire(size && size < local.size(), "Cannot locate the existing cache");
    const fs::path cache = fs::path(local.data()) / L"FAHSaver" / L"latest-model.cache";
    Json result{{"preferences_sha256", sha256(preferences.dump())}, {"cache_exists", fs::exists(cache)}};
    if (fs::exists(cache)) result["cache_sha256"] = sha256(readFile(cache));
    return result;
}
static DWORD runChild(const fs::path& executable, const std::wstring& arguments) {
    Handle job(CreateJobObjectW(nullptr, nullptr)); winRequire(job.value != nullptr, "Cannot create child job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    winRequire(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Cannot limit child job");
    std::wstring command = quote(executable.native()) + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    winRequire(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "Cannot start installer test child");
    Handle processHandle(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, process.hProcess)) {
        TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 5000);
        throw std::runtime_error("Cannot assign installer test child to its job");
    }
    winRequire(ResumeThread(process.hThread) != DWORD(-1), "Cannot resume installer test child");
    // NSIS uninstallers start a temporary child; wait for the whole job.
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION state{};
        winRequire(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr), "Cannot query installer test job");
        if (!state.ActiveProcesses) break;
        if (GetTickCount64() >= deadline) {
            winRequire(TerminateJobObject(job.value, 124), "Cannot terminate the timed-out installer test job");
            const auto stoppedDeadline = GetTickCount64() + 5000;
            do {
                winRequire(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &state, sizeof(state), nullptr),
                           "Cannot verify timed-out child cleanup");
                if (!state.ActiveProcesses) break;
                Sleep(20);
            } while (GetTickCount64() < stoppedDeadline);
            require(!state.ActiveProcesses, "Timed-out installer child processes did not stop");
            throw std::runtime_error("Installer test child exceeded 30 seconds");
        }
        Sleep(20);
    }
    DWORD code = STILL_ACTIVE;
    winRequire(GetExitCodeProcess(process.hProcess, &code), "Cannot read installer test child exit code");
    require(code != STILL_ACTIVE, "Installer test child has not exited");
    for (;;) {
        Handle mutex(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\FAHSaver.Setup"));
        if (!mutex.value) {
            require(GetLastError() == ERROR_FILE_NOT_FOUND, "Cannot check the installer mutex");
            break;
        }
        require(GetTickCount64() < deadline, "Installer mutex remained after its child job ended");
        Sleep(20);
    }
    return code;
}
static std::wstring shortPath(const fs::path& path) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD size = GetShortPathNameW(path.c_str(), buffer.data(), DWORD(buffer.size()));
    winRequire(size && size < buffer.size(), "Cannot obtain an 8.3 test path");
    const std::wstring result(buffer.data(), size);
    require(!equalText(result, path.native()), "The test volume did not provide a distinct 8.3 path");
    return result;
}
static Json payload(const fs::path& folder) {
    Json result = Json::object();
    for (const auto* name : managedFiles) {
        const auto bytes = readFile(folder / name); require(!bytes.empty(), "Installed payload file is empty");
        result[utf8(name)] = Json{{"bytes", bytes.size()}, {"sha256", sha256(bytes)}};
    }
    return result;
}
static void verifyResources(const fs::path& scr, const std::wstring& version) {
    HMODULE module = LoadLibraryExW(scr.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    winRequire(module != nullptr, "Cannot open installed screen saver resources");
    std::array<wchar_t, 128> label{};
    const int length = LoadStringW(module, 1, label.data(), int(label.size())); FreeLibrary(module);
    require(length == 9 && std::wstring(label.data(), length) == L"FAH Saver", "Installed screen saver ordinal 1 name is incorrect");
    DWORD ignored = 0; const DWORD size = GetFileVersionInfoSizeW(scr.c_str(), &ignored);
    winRequire(size > 0, "Installed version resource is absent");
    std::vector<unsigned char> data(size);
    winRequire(GetFileVersionInfoW(scr.c_str(), 0, size, data.data()), "Cannot read installed version resources");
    for (const auto* name : {L"FileDescription", L"ProductName", L"FileVersion", L"ProductVersion"}) {
        const std::wstring key = std::wstring(L"\\StringFileInfo\\040904b0\\") + name;
        wchar_t* value = nullptr; UINT count = 0;
        winRequire(VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &count), "Installed version string is absent");
        require(count > 0 && std::wstring(value) == ((std::wstring(name) == L"FileVersion" || std::wstring(name) == L"ProductVersion") ? version : L"FAH Saver"),
                "Installed product name or version is incorrect");
    }
}
static void verifyInstalled(const fs::path& folder, const std::wstring& version) {
    require(valueString(readValue(installerKey, L"InstallDir")) == folder.native(), "Installer metadata location is incorrect");
    require(valueString(readValue(installerKey, L"Version")) == version, "Installer metadata version is incorrect");
    require(valueString(readValue(arpKey, L"DisplayName")) == L"FAH Saver", "Windows Apps display name is incorrect");
    require(valueString(readValue(arpKey, L"DisplayVersion")) == version, "Windows Apps version is incorrect");
    require(valueString(readValue(arpKey, L"InstallLocation")) == folder.native(), "Windows Apps location is incorrect");
    require(valueString(readValue(arpKey, L"UninstallString")) == quote((folder / L"Uninstall.exe").native()), "Windows Apps uninstall path is not quoted correctly");
    require(valueString(readValue(arpKey, L"QuietUninstallString")) == quote((folder / L"Uninstall.exe").native()) + L" /S", "Windows Apps quiet uninstall path is incorrect");
    require(valueString(readValue(desktopKey, L"SCRNSAVE.EXE")) == (folder / L"FAHSaver.scr").native() && active(), "Installer did not select its owned screen saver");
    verifyResources(folder / L"FAHSaver.scr", version);
}
static fs::path checkedRoot(const fs::path& candidate) {
    std::array<wchar_t, 32768> temporary{};
    const DWORD size = GetTempPathW(DWORD(temporary.size()), temporary.data());
    winRequire(size && size < temporary.size(), "Cannot locate the test temporary directory");
    const fs::path root(fullPath(candidate));
    require(root.is_absolute() && equalText(fullPath(root.parent_path()), fullPath(temporary.data())) &&
            root.filename().native().rfind(L"FAHSaver-installer-", 0) == 0, "Test root must be a direct TEMP child named FAHSaver-installer-*");
    return root;
}
static void stopOwnedSaver(const fs::path& scr) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    winRequire(snapshot.value != INVALID_HANDLE_VALUE, "Cannot check owned test processes");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return;
    do {
        if (!equalText(entry.szExeFile, L"FAHSaver.scr")) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!process.value) continue;
        std::array<wchar_t, 32768> image{}; DWORD length = DWORD(image.size());
        if (QueryFullProcessImageNameW(process.value, 0, image.data(), &length) && equalText(fullPath(std::wstring(image.data(), length)), fullPath(scr))) {
            winRequire(TerminateProcess(process.value, 125), "Cannot stop the owned test screen saver");
            require(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "Owned test screen saver did not stop");
        }
    } while (Process32NextW(snapshot.value, &entry));
}
struct Recovery {
    fs::path root;
    Json state;
    bool ready = false;
    bool restored = false;
    void restore() {
        stopOwnedSaver(root / L"App With Spaces" / L"FAHSaver.scr");
        setValue(desktopKey, L"SCRNSAVE.EXE", state.at("desktop").at("SCRNSAVE.EXE"));
        setActive(state.at("active").get<bool>());
        for (const auto* name : desktopValues) setValue(desktopKey, name, state.at("desktop").at(utf8(name)));
        for (const auto& pair : {std::pair<const wchar_t*, const char*>{installerKey, "installer"}, {arpKey, "arp"}}) {
            if (readTree(pair.first) != state.at(pair.second)) { deleteTree(pair.first); writeTree(pair.first, state.at(pair.second)); }
        }
        require(desktopSnapshot() == state.at("desktop") && active() == state.at("active").get<bool>() &&
                readTree(installerKey) == state.at("installer") && readTree(arpKey) == state.at("arp"), "Original Windows state was not restored");
        restored = true;
    }
    void cleanup() {
        require(restored, "Refusing fixture cleanup before state restoration");
        const auto marker = readFile(root / markerName);
        require(std::string(marker.begin(), marker.end()) == markerText, "Owned test marker changed; fixture retained");
        const fs::path folder = root / L"App With Spaces";
        for (const auto* name : managedFiles) { std::error_code error; fs::remove(folder / name, error); require(!error, "Cannot remove an owned test file"); }
        for (const auto* name : {L"keep-user-data.txt", L"shaders", L"licences"}) {
            std::error_code error; fs::remove(folder / name, error); require(!error, "Cannot remove an owned test fixture");
        }
        std::error_code error; fs::remove(folder, error); require(!error, "Unknown files remain in the owned test folder");
        fs::remove(root / L"Setup Download.exe", error); require(!error, "Cannot remove the owned setup download");
        fs::remove(root / L"recovery.json", error); require(!error, "Cannot remove the recovery snapshot");
        fs::remove(root / markerName, error); require(!error, "Cannot remove the ownership marker");
        fs::remove(root, error); require(!error, "Unknown files remain in the owned test root");
    }
};

static void selfCheck() {
    require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 self-check failed");
    const std::vector<std::wstring> arguments{L"C:\\App With Spaces\\helper.exe", L"a b", L"trailing\\", L"quote\"inside", L""};
    std::wstring command;
    for (const auto& argument : arguments) { if (!command.empty()) command += L' '; command += quote(argument); }
    int count = 0; wchar_t** parsed = CommandLineToArgvW(command.c_str(), &count);
    winRequire(parsed != nullptr, "Command quoting self-check parse failed");
    bool same = count == int(arguments.size());
    for (int i = 0; same && i < count; ++i) same = parsed[i] == arguments[std::size_t(i)];
    LocalFree(parsed); require(same, "Command quoting self-check failed");
    require(unhex(hex({0, 1, 127, 128, 255})) == std::vector<unsigned char>({0, 1, 127, 128, 255}), "Snapshot hexadecimal self-check failed");
    std::array<wchar_t, 32768> executable{};
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    winRequire(size && size < executable.size(), "Cannot locate the helper self-check executable");
    require(runChild(fs::path(std::wstring(executable.data(), size)), L" --child-self-check") == 17,
            "Bounded child-process self-check failed");
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--self-check") {
        try { selfCheck(); std::puts("Installer helper self-checks passed"); return 0; }
        catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--child-self-check") return 17;
    const bool restoreOnly = argc > 1 && std::wstring(argv[1]) == L"--restore";
    if (restoreOnly ? argc != 4 : argc != 5) {
        std::fputs("usage: installer-lifecycle SETUP_EXE EXPECTED_VERSION OWNED_TEMP_ROOT OUTPUT_JSON\n       installer-lifecycle --restore OWNED_TEMP_ROOT OUTPUT_JSON\n", stderr); return 2;
    }
    const fs::path output(argv[argc - 1]);
    Json report{{"passed", false}, {"restored", false}, {"cleanup", false}, {"checks", Json::object()}, {"child_exit_codes", Json::array()}};
    std::unique_ptr<Recovery> recovery;
    try {
        selfCheck();
        const fs::path root = checkedRoot(argv[restoreOnly ? 2 : 3]);
        const auto rootPrefix = fullPath(root) + L"\\", outputPath = fullPath(output);
        require(!equalText(fullPath(output.parent_path()), fullPath(root)) &&
                !(outputPath.size() >= rootPrefix.size() && equalText(outputPath.substr(0, rootPrefix.size()), rootPrefix)),
                "Output report must be outside the owned test root");
        require(output.is_absolute() && fs::is_directory(output.parent_path()), "Output report needs an existing absolute parent directory");
        recovery = std::make_unique<Recovery>(); recovery->root = root;
        if (restoreOnly) {
            const auto marker = readFile(root / markerName);
            require(std::string(marker.begin(), marker.end()) == markerText, "Recovery root ownership marker is invalid");
            const auto bytes = readFile(root / L"recovery.json");
            recovery->state = Json::parse(bytes.begin(), bytes.end());
            require(recovery->state.at("format") == 1 && equalText(wide(recovery->state.at("root").get<std::string>()), root.native()), "Recovery snapshot does not match its root");
            recovery->ready = true;
            recovery->restore(); report["restored"] = true; recovery->cleanup(); report["cleanup"] = true; report["passed"] = true;
        } else {
            const fs::path setup(fullPath(argv[1])), folder = root / L"App With Spaces", scr = folder / L"FAHSaver.scr";
            const std::wstring version(argv[2]);
            require(fs::is_regular_file(setup), "Setup executable is missing");
            require(!fs::exists(root), "Owned test root already exists; use --restore if a previous test was interrupted");
            recovery->state = Json{{"format", 1}, {"root", utf8(root.native())}, {"desktop", desktopSnapshot()}, {"active", active()},
                                    {"installer", readTree(installerKey)}, {"arp", readTree(arpKey)}};
            const auto originalSaver = recovery->state["desktop"]["SCRNSAVE.EXE"];
            std::wstring other = valueString(originalSaver);
            if (other.size() >= 2 && other.front() == L'"' && other.back() == L'"') other = other.substr(1, other.size() - 2);
            std::error_code originalFileError;
            require(fs::is_regular_file(other, originalFileError) && !originalFileError,
                    "The original selected screen saver must exist for the preservation test");
            winRequire(CreateDirectoryW(root.c_str(), nullptr), "Cannot create absent owned test root");
            writeFile(root / markerName, markerText, true);
            // This private snapshot permits recovery after an interrupted test process.
            writeFile(root / L"recovery.json", recovery->state.dump(2), true);
            recovery->ready = true;
            report["expected_version"] = utf8(version); report["setup_sha256"] = sha256(readFile(setup));
            DWORD session = 0; winRequire(ProcessIdToSessionId(GetCurrentProcessId(), &session), "Cannot query the test session"); report["session_id"] = session;
            const auto originalData = preservedUserData(); report["preserved_data_before"] = originalData;
            auto unchanged = [&] {
                require(preservedUserData() == originalData, "User preferences or cached model changed during installer testing");
                require(readValue(desktopKey, L"ScreenSaveTimeOut") == recovery->state["desktop"]["ScreenSaveTimeOut"] &&
                        readValue(desktopKey, L"ScreenSaverIsSecure") == recovery->state["desktop"]["ScreenSaverIsSecure"], "Screen saver timeout or security policy changed");
            };
            bool userFileCreated = false;
            auto install = [&](const fs::path& installer) {
                const auto code = runChild(installer, L" /S /D=" + folder.native()); report["child_exit_codes"].push_back(code);
                require(code == 0, "Silent installation or update failed"); verifyInstalled(folder, version); unchanged();
                if (userFileCreated)
                    require(readFile(folder / L"keep-user-data.txt") == std::vector<unsigned char>({'k','e','e','p','\n'}),
                            "Installation or update changed an arbitrary user file");
            };
            auto uninstall = [&] {
                const auto code = runChild(folder / L"Uninstall.exe", L" /S"); report["child_exit_codes"].push_back(code);
                require(code == 0, "Silent uninstaller launcher failed");
                require(!readTree(installerKey).at("exists").get<bool>() && !readTree(arpKey).at("exists").get<bool>(), "Uninstaller metadata remained");
                for (const auto* name : managedFiles) require(!fs::exists(folder / name), "Managed files remained after uninstall");
                require(readFile(folder / L"keep-user-data.txt") == std::vector<unsigned char>({'k','e','e','p','\n'}), "Uninstaller removed an arbitrary user file");
                unchanged();
            };
            const fs::path download = root / L"Setup Download.exe";
            winRequire(CopyFileW(setup.c_str(), download.c_str(), TRUE), "Cannot create the owned downloaded-setup fixture");
            require(sha256(readFile(download)) == report["setup_sha256"], "Downloaded setup fixture differs from its source");
            install(download); report["checks"]["install_name_version_registration_apps_entry"] = true;
            winRequire(DeleteFileW(download.c_str()), "Cannot delete the downloaded setup after installation");
            verifyInstalled(folder, version); require(!fs::exists(download), "Downloaded setup was not deleted");
            report["checks"]["download_can_be_deleted_after_installation"] = true;
            writeFile(folder / L"keep-user-data.txt", "keep\n", true);
            userFileCreated = true;
            const auto firstPayload = payload(folder); report["installed_payload"] = firstPayload;
            install(setup); require(payload(folder) == firstPayload, "Same-version update changed its installed payload");
            report["checks"]["same_version_update_preserves_payload_and_settings"] = true;
            const auto beforeLockDesktop = desktopSnapshot(), beforeLockMetadata = readTree(installerKey), beforeLockArp = readTree(arpKey);
            const bool beforeLockActive = active();
            {
                Handle lock(CreateFileW(scr.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                winRequire(lock.value != INVALID_HANDLE_VALUE, "Cannot lock the owned test screen saver");
                const auto code = runChild(setup, L" /S /D=" + folder.native()); report["child_exit_codes"].push_back(code);
                require(code == 2, "Locked-file update did not fail with exit code 2");
                require(payload(folder) == firstPayload && desktopSnapshot() == beforeLockDesktop && active() == beforeLockActive &&
                        readTree(installerKey) == beforeLockMetadata && readTree(arpKey) == beforeLockArp, "Locked-file failure changed files or registration");
            }
            unchanged(); report["checks"]["locked_update_fails_without_changes"] = true;
            uninstall(); require(readValue(desktopKey, L"SCRNSAVE.EXE").is_null() && !active(), "Uninstall did not clear its owned selection");
            report["checks"]["uninstall_owned_long_path"] = true;
            for (bool quoted : {false, true}) {
                install(setup); const auto selected = shortPath(scr);
                setValue(desktopKey, L"SCRNSAVE.EXE", stringValue(quoted ? L"\"" + selected + L"\"" : selected));
                uninstall(); require(readValue(desktopKey, L"SCRNSAVE.EXE").is_null() && !active(), "Uninstall did not recognise its owned 8.3 selection");
                report["checks"][quoted ? "uninstall_owned_quoted_short_path" : "uninstall_owned_short_path"] = true;
            }
            install(setup); setValue(desktopKey, L"SCRNSAVE.EXE", originalSaver); setActive(recovery->state.at("active").get<bool>());
            const auto otherSelection = readValue(desktopKey, L"SCRNSAVE.EXE"); const bool otherActive = active();
            uninstall(); require(readValue(desktopKey, L"SCRNSAVE.EXE") == otherSelection && active() == otherActive,
                                 "Uninstall changed another selected screen saver");
            report["checks"]["uninstall_preserves_another_saver_and_user_file"] = true;
            recovery->restore(); report["restored"] = true;
            unchanged(); report["preserved_data_after"] = preservedUserData();
            recovery->cleanup(); report["cleanup"] = true; report["passed"] = true;
        }
    } catch (const std::exception& error) {
        report["error"] = error.what();
        if (recovery && recovery->ready) {
            try { recovery->restore(); report["restored"] = true; recovery->cleanup(); report["cleanup"] = true; }
            catch (const std::exception& restoreError) { report["recovery_error"] = restoreError.what(); }
        }
    }
    try { writeFile(output, report.dump(2) + "\n"); }
    catch (const std::exception& error) { std::fprintf(stderr, "Cannot write test report: %s\n", error.what()); return 3; }
    std::puts(report.at("passed").get<bool>() ? "Installer lifecycle checks passed; original state restored" : "Installer lifecycle checks failed; inspect the JSON report");
    return report.at("passed").get<bool>() ? 0 : 1;
}
