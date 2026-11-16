#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <processsnapshot.h>
#include <psapi.h>
#endif

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Thread {
    std::uint32_t pid, id;
    std::uint64_t created, kernel, user, start, instruction = 0;
};
struct Module { std::uint64_t base, size; std::string name; };
struct Cpu { std::uint64_t kernel, user; };

bool sameProcess(std::uint32_t pid, std::uint64_t created,
                 std::uint32_t expectedPid, std::uint64_t expectedCreated) {
    return pid == expectedPid && created == expectedCreated;
}
Cpu cpuDelta(const Thread& before, const Thread& after) {
    if (!sameProcess(after.pid, after.created, before.pid, before.created) || before.id != after.id)
        throw std::runtime_error("Thread identity changed between snapshots");
    if (after.kernel < before.kernel || after.user < before.user)
        throw std::runtime_error("Thread CPU counters moved backwards");
    return {after.kernel - before.kernel, after.user - before.user};
}
const Module* moduleAt(std::uint64_t address, const std::vector<Module>& modules) {
    for (const auto& module : modules)
        if (address >= module.base && address - module.base < module.size) return &module;
    return nullptr;
}
Json location(std::uint64_t address, const std::vector<Module>& modules) {
    if (const auto* module = moduleAt(address, modules))
        return {{"module", module->name}, {"rva", address - module->base}};
    return {{"module", nullptr}, {"rva", nullptr}};
}
void selfCheck() {
    const Thread before{7, 8, 9, 10, 20, 100}, after{7, 8, 9, 40, 120, 100};
    const auto delta = cpuDelta(before, after);
    if (delta.kernel != 30 || delta.user != 100 || sameProcess(7, 10, 7, 9) ||
        sameProcess(8, 9, 7, 9)) throw std::runtime_error("CPU/identity self-check failed");
    bool rejected = false;
    try { auto reused = after; ++reused.created; cpuDelta(before, reused); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Thread-reuse self-check failed");
    rejected = false;
    try { auto backwards = after; backwards.kernel = 0; cpuDelta(before, backwards); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("CPU counter self-check failed");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const std::vector<Module> modules{{100, 20, "one.dll"}, {maximum - 3, 3, "edge.dll"}};
    if (moduleAt(99, modules) || !moduleAt(100, modules) || !moduleAt(119, modules) ||
        moduleAt(120, modules) || !moduleAt(maximum - 1, modules) || moduleAt(maximum, modules))
        throw std::runtime_error("Module-range self-check failed");
    if (location(119, modules).at("rva") != 19 || !location(99, modules).at("module").is_null())
        throw std::runtime_error("Module-attribution self-check failed");
}

#ifdef _WIN32
static_assert(sizeof(void*) == 8, "Build this diagnostic for x64");
[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(error) + ")");
}
struct ProcessHandle {
    HANDLE value = nullptr;
    ~ProcessHandle() { if (value) CloseHandle(value); }
};
struct Snapshot {
    HPSS value = nullptr;
    ~Snapshot() { if (value) PssFreeSnapshot(GetCurrentProcess(), value); }
    void release() {
        const auto handle = value;
        value = nullptr;
        if (handle) {
            const auto error = PssFreeSnapshot(GetCurrentProcess(), handle);
            if (error) fail("Free local snapshot", error);
        }
    }
};
struct Marker {
    HPSSWALK value = nullptr;
    ~Marker() { if (value) PssWalkMarkerFree(value); }
    void release() {
        const auto handle = value;
        value = nullptr;
        if (handle) {
            const auto error = PssWalkMarkerFree(handle);
            if (error) fail("Free snapshot walk marker", error);
        }
    }
};
std::uint64_t ticks(FILETIME time) {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
std::uint64_t decimal(const std::wstring& text) {
    if (text.empty()) throw std::runtime_error("Empty numeric argument");
    std::uint64_t value = 0;
    for (wchar_t digit : text) {
        if (digit < L'0' || digit > L'9' ||
            value > (std::numeric_limits<std::uint64_t>::max() - (digit - L'0')) / 10)
            throw std::runtime_error("Invalid unsigned decimal argument");
        value = value * 10 + digit - L'0';
    }
    return value;
}
std::string utf8(const std::wstring& text) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) fail("Convert module name");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()),
                             result.data(), size, nullptr, nullptr)) fail("Convert module name");
    return result;
}
std::wstring normalPath(const std::wstring& text) {
    auto path = std::filesystem::path(text);
    if (!path.is_absolute()) throw std::runtime_error("Expected executable path must be absolute");
    return path.lexically_normal().make_preferred().native();
}
void validateProcess(HANDLE process, DWORD expectedPid, std::uint64_t created,
                     const std::wstring& expectedPath) {
    FILETIME start, exit, kernel, user;
    if (!GetProcessTimes(process, &start, &exit, &kernel, &user)) fail("Read process identity");
    if (!sameProcess(GetProcessId(process), ticks(start), expectedPid, created))
        throw std::runtime_error("PID or creation time does not match the launched test process");
    DWORD status = 0;
    if (!GetExitCodeProcess(process, &status)) fail("Read process status");
    if (status != STILL_ACTIVE) throw std::runtime_error("Test process exited during attribution");
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &size)) fail("Read executable path");
    image.resize(size);
    image = normalPath(image);
    if (CompareStringOrdinal(image.c_str(), int(image.size()), expectedPath.c_str(),
                             int(expectedPath.size()), TRUE) != CSTR_EQUAL)
        throw std::runtime_error("Executable path does not match the launched test process");
    USHORT machine = 0, native = 0;
    if (!IsWow64Process2(process, &machine, &native)) fail("Read target architecture");
    if (native != IMAGE_FILE_MACHINE_AMD64 || machine != IMAGE_FILE_MACHINE_UNKNOWN)
        throw std::runtime_error("Only native x64 Windows targets are supported");
}
std::vector<Module> modulesFor(HANDLE process) {
    HMODULE handles[2048];
    DWORD bytes = 0;
    if (!EnumProcessModulesEx(process, handles, sizeof(handles), &bytes, LIST_MODULES_64BIT))
        fail("Enumerate test-process modules");
    if (bytes > sizeof(handles) || bytes % sizeof(HMODULE))
        throw std::runtime_error("Test-process module list exceeds the diagnostic limit");
    std::vector<Module> modules;
    for (std::size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
        MODULEINFO info{};
        if (!GetModuleInformation(process, handles[i], &info, sizeof(info))) fail("Read module range");
        std::wstring name(32768, L'\0');
        const auto length = GetModuleFileNameExW(process, handles[i], name.data(), DWORD(name.size()));
        if (!length || length >= name.size()) fail("Read module name");
        name.resize(length);
        modules.push_back({reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage,
                           utf8(std::filesystem::path(name).filename().native())});
    }
    return modules;
}
std::vector<Thread> threadsFor(HANDLE process, DWORD pid, DWORD contextThread = 0) {
    const bool contexts = contextThread != 0;
    Snapshot snapshot;
    const auto flags = static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_THREADS |
        (contexts ? PSS_CAPTURE_THREAD_CONTEXT : 0));
    auto error = PssCaptureSnapshot(process, flags, contexts ? CONTEXT_CONTROL : 0, &snapshot.value);
    if (error) fail("Capture test-process snapshot", error);
    Marker marker;
    error = PssWalkMarkerCreate(nullptr, &marker.value);
    if (error) fail("Create snapshot walk marker", error);
    std::vector<Thread> threads;
    for (;;) {
        PSS_THREAD_ENTRY entry{};
        error = PssWalkSnapshot(snapshot.value, PSS_WALK_THREADS, marker.value, &entry, sizeof(entry));
        if (error == ERROR_NO_MORE_ITEMS) break;
        if (error) fail("Read snapshot thread", error);
        if (entry.ProcessId != pid) throw std::runtime_error("Snapshot returned a thread from another process");
        if (threads.size() == 1024) throw std::runtime_error("Test process exceeds the 1024-thread diagnostic limit");
        Thread thread{entry.ProcessId, entry.ThreadId, ticks(entry.CreateTime), ticks(entry.KernelTime),
                      ticks(entry.UserTime), reinterpret_cast<std::uintptr_t>(entry.Win32StartAddress)};
        if (entry.ThreadId == contextThread) {
            if (!entry.ContextRecord || entry.SizeOfContextRecord < offsetof(CONTEXT, Rip) + sizeof(DWORD64) ||
                (entry.ContextRecord->ContextFlags & CONTEXT_CONTROL) != CONTEXT_CONTROL)
                throw std::runtime_error("Snapshot did not supply the requested x64 control context");
            thread.instruction = entry.ContextRecord->Rip;
        }
        threads.push_back(thread);
    }
    if (threads.empty()) throw std::runtime_error("Snapshot returned no test-process threads");
    marker.release();
    snapshot.release();
    return threads;
}

int sample(int argc, wchar_t** argv) {
    DWORD pid = 0, mainThread = 0;
    std::uint64_t created = 0;
    std::wstring expectedPath;
    bool contexts = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring key = argv[i];
        if (key == L"--contexts") { if (contexts) throw std::runtime_error("Duplicate context flag"); contexts = true; continue; }
        if (++i == argc) throw std::runtime_error("Missing option value");
        const std::wstring value = argv[i];
        if (key == L"--exe" && expectedPath.empty()) expectedPath = normalPath(value);
        else if (key == L"--created" && !created) {
            created = decimal(value);
            if (!created) throw std::runtime_error("Creation FILETIME must be nonzero");
        }
        else if ((key == L"--pid" && !pid) || (key == L"--main-thread" && !mainThread)) {
            const auto number = decimal(value);
            if (!number || number > MAXDWORD) throw std::runtime_error("PID/thread ID is outside DWORD range");
            if (key == L"--pid") pid = DWORD(number); else mainThread = DWORD(number);
        } else throw std::runtime_error("Unknown or duplicate option");
    }
    if (!pid || !created || expectedPath.empty())
        throw std::runtime_error("Use --pid PID --exe ABSOLUTE_PATH --created FILETIME [--contexts] [--main-thread TID]");
    ProcessHandle process;
    process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process.value) fail("Open the launched test process");
    validateProcess(process.value, pid, created, expectedPath);
    const auto begun = Clock::now();
    const auto before = threadsFor(process.value, pid);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    validateProcess(process.value, pid, created, expectedPath);
    const auto after = threadsFor(process.value, pid);
    const auto interval = std::chrono::duration<double>(Clock::now() - begun).count();
    const auto modules = modulesFor(process.value);
    Json rows = Json::array();
    DWORD hottest = 0;
    std::uint64_t hottestCreated = 0, hottestCpu = 0;
    std::size_t unmatched = 0;
    bool mainFound = !mainThread;
    for (const auto& thread : after) {
        if (thread.id == mainThread) mainFound = true;
        const auto old = std::find_if(before.begin(), before.end(), [&](const Thread& candidate) {
            return candidate.id == thread.id && candidate.created == thread.created;
        });
        if (old == before.end()) { ++unmatched; continue; }
        const auto cpu = cpuDelta(*old, thread);
        rows.push_back({{"tid", thread.id}, {"user_cpu_seconds", double(cpu.user) / 1e7},
            {"kernel_cpu_seconds", double(cpu.kernel) / 1e7}, {"start", location(thread.start, modules)}});
        if (thread.id != mainThread && cpu.user + cpu.kernel > hottestCpu) {
            hottest = thread.id; hottestCreated = thread.created; hottestCpu = cpu.user + cpu.kernel;
        }
    }
    if (!mainFound) throw std::runtime_error("Supplied GUI thread was not found in the test process");
    std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        return a.at("user_cpu_seconds").get<double>() + a.at("kernel_cpu_seconds").get<double>() >
               b.at("user_cpu_seconds").get<double>() + b.at("kernel_cpu_seconds").get<double>();
    });
    Json samples = Json::array();
    double snapshotMilliseconds = 0;
    if (contexts) {
        if (!hottest) throw std::runtime_error("No worker consumed CPU during the attribution interval");
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        constexpr unsigned delays[]{53, 79, 61, 97, 71};
        for (unsigned i = 0; i < 24 && Clock::now() < deadline; ++i) {
            validateProcess(process.value, pid, created, expectedPath);
            const auto start = Clock::now();
            const auto threads = threadsFor(process.value, pid, hottest);
            snapshotMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            const auto hot = std::find_if(threads.begin(), threads.end(), [&](const Thread& thread) {
                return thread.id == hottest && thread.created == hottestCreated;
            });
            if (hot == threads.end()) throw std::runtime_error("Selected worker exited or its thread ID was reused");
            samples.push_back(location(hot->instruction, modulesFor(process.value)));
            std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(delays[i % 5])));
        }
    }
    validateProcess(process.value, pid, created, expectedPath);
    const auto total = std::chrono::duration<double>(Clock::now() - begun).count();
    std::cout << Json{{"kind", "process-specific attribution; not a performance benchmark"},
        {"pid", pid}, {"creation_filetime", created}, {"cpu_snapshot_wall_span_seconds", interval},
        {"total_seconds", total}, {"threads", rows}, {"unmatched_after_threads", unmatched},
        {"before_thread_count", before.size()}, {"after_thread_count", after.size()},
        {"excluded_gui_tid", mainThread ? Json(mainThread) : Json(nullptr)},
        {"context_target_tid", contexts ? Json(hottest) : Json(nullptr)},
        {"instruction_samples", samples}, {"context_snapshot_wall_ms", snapshotMilliseconds},
        {"limits", {"Only matching thread lifetimes contribute CPU deltas; exited/new threads can be omitted.",
            "The CPU snapshot wall span includes collection time; counters are captured within those calls.",
            "Start module identifies a thread entrypoint, not its current execution.",
            "Instruction samples are wall-time observations, not CPU-weighted stacks; kernel execution is unresolved.",
            "Context capture can perturb this test process; snapshots are freed after each pass.",
            "No ETW, VA clone, stack memory, manual suspension or unrelated process enumeration.",
            "At most 24 context snapshots within a two-second scheduling budget; individual Windows calls have no timeout."}}}
        .dump(2) << '\n';
    return 0;
}
#endif
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        selfCheck();
        if (argc == 2 && std::wstring(argv[1]) == L"--self-check") {
            std::cout << "{\"self_check\":\"passed\"}\n"; return 0;
        }
        return sample(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "thread attribution: " << error.what() << '\n'; return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        if (argc != 2 || std::string(argv[1]) != "--self-check")
            throw std::runtime_error("Runtime sampling requires native x64 Windows");
        selfCheck(); std::cout << "{\"self_check\":\"passed\"}\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
