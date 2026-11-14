#include "run.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <cwctype>
#include <process.h>
#include <random>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace {
namespace fs = std::filesystem;

#ifdef _WIN32
// Only native executables: .bat/.cmd files would run through cmd.exe, which applies its own,
// different argument parsing. Windows file names are case-insensitive.
bool has_executable_extension(const fs::path &path) {
    auto extension = path.extension().wstring();
    for (auto &c : extension)
        c = static_cast<wchar_t>(std::towlower(c));
    return extension == L".exe" || extension == L".com";
}
#endif

bool executable(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        return false;
#ifdef _WIN32
    return has_executable_extension(path);
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

// The executable a command name refers to. On Windows, `clang++` also matches clang++.exe and
// clang++.com, as it would in a shell; returns an empty path when nothing matches.
fs::path resolve_executable(const fs::path &path) {
    if (executable(path))
        return path;
#ifdef _WIN32
    if (!has_executable_extension(path)) {
        for (const auto *extension : {L".exe", L".com"}) {
            auto candidate = path;
            candidate += extension;
            if (executable(candidate))
                return candidate;
        }
    }
#endif
    return {};
}

fs::path find_on_path(const fs::path &name) {
    if (const char *path = std::getenv("PATH")) {
        const std::string directories(path);
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        for (std::size_t start = 0; start < directories.size();) {
            const auto end = directories.find(separator, start);
            const auto directory = directories.substr(start, end - start);
            if (!directory.empty()) {
                if (auto found = resolve_executable(fs::absolute(fs::path(directory) / name));
                    !found.empty())
                    return found;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    return {};
}

fs::path find_compiler() {
    if (const char *configured = std::getenv("JAQR_CXX"); configured && *configured) {
        const fs::path compiler(configured);
        const auto found = compiler.has_parent_path() ? resolve_executable(fs::absolute(compiler))
                                                      : find_on_path(compiler);
        if (found.empty())
            throw std::runtime_error("JAQR_CXX='" + std::string(configured) +
                                     "' does not name an executable C++ compiler");
        return found;
    }
#ifdef _WIN32
    const std::vector<std::string> candidates = {"g++", "clang++"};
#elif defined(__APPLE__)
    const std::vector<std::string> candidates = {"clang++", "g++"};
#else
    const std::vector<std::string> candidates = {"g++", "clang++"};
#endif
    for (const auto &name : candidates)
        if (auto compiler = find_on_path(name); !compiler.empty())
            return compiler;
    throw std::runtime_error("No C++ compiler found on PATH; set JAQR_CXX to a C++20 compiler");
}

fs::path make_temp_dir() {
    const auto base = fs::temp_directory_path();
#ifdef _WIN32
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto path =
            base / ("jaqr-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (fs::create_directory(path, error))
            return path;
        if (error && error != std::errc::file_exists)
            throw fs::filesystem_error("Could not create temporary directory", path, error);
    }
    throw std::runtime_error("Could not create a unique temporary directory");
#else
    std::string pattern = (base / "jaqr-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
        throw std::system_error(errno, std::generic_category(),
                                "Could not create temporary directory");
    return pattern;
#endif
}

struct temporary_directory {
    fs::path path = make_temp_dir();

    ~temporary_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

#ifdef _WIN32
// Windows passes a child one command line, which the child splits back into arguments using the
// CommandLineToArgvW rules. The CRT spawn functions join argv with plain spaces, so each argument
// must already be quoted: backslashes are literal except directly before a quote, where each must
// be doubled and the quote escaped; trailing backslashes are doubled before the closing quote.
std::wstring quote_argument(const std::wstring &argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"')
            quoted.append(backslashes * 2 + 1, L'\\');
        else
            quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

// Pass arguments to the process API instead of constructing a shell command, so no shell
// interprets them. The child inherits terminal input/output; returns its exit code.
int execute(const std::vector<fs::path> &arguments) {
#ifdef _WIN32
    std::vector<std::wstring> quoted;
    quoted.reserve(arguments.size());
    for (const auto &argument : arguments)
        quoted.push_back(quote_argument(argument.native()));
    std::vector<const wchar_t *> argv;
    argv.reserve(quoted.size() + 1);
    for (const auto &argument : quoted)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const intptr_t status = _wspawnv(_P_WAIT, arguments.front().c_str(), argv.data());
    if (status == -1)
        throw std::system_error(errno, std::generic_category(), "Could not start process");
    return static_cast<int>(status);
#else
    std::vector<char *> argv;
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    const int error =
        posix_spawn(&child, arguments.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (error)
        throw std::system_error(error, std::generic_category(), "Could not start process");
    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "Could not wait for process");
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
#endif
}
} // namespace

int run_generated(const std::vector<std::string> &code, const std::string &output_name, bool run) {
    // Generation always leaves inspectable C++ on disk. Compiling and executing it is
    // opt-in; only the executable is temporary, so diagnostics can refer to this source.
    const auto source = fs::absolute(output_name);
    {
        std::ofstream output(source);
        if (!output)
            throw std::runtime_error("Could not write generated C++ to '" + output_name + "'");
        for (const auto &part : code)
            if (!part.empty())
                output << part << '\n';
        output.close();
        if (!output)
            throw std::runtime_error("Could not finish writing generated C++ to '" + output_name +
                                     "'");
    }
    if (!run)
        return 0;

    const auto compiler = find_compiler();
    temporary_directory temp;
#ifdef _WIN32
    const auto program = temp.path / "program.exe";
#else
    const auto program = temp.path / "program";
#endif
    // TODO: the program is compiled without optimisation and there is no way to add flags;
    //       read extra compiler arguments from a JAQR_CXXFLAGS environment variable.
    // TODO: JAQR_RUNTIME_DIR is the absolute source-tree path baked in at build time, so jaqr
    //       cannot be moved or installed. Find the runtime relative to the executable (with an
    //       environment override) and add an install() rule for jaqr and runtime/ in CMake.
    const int compiled =
        execute({compiler, "-std=c++20", "-I", JAQR_RUNTIME_DIR, source, "-o", program});
    return compiled ? compiled : execute({program});
}
