#include "convert.h"

#include "args.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

static const char * CONVERT_SCRIPT_NAME = "convert-safetensors.py";

// ---------------------------------------------------------------------------
// small filesystem helpers
// ---------------------------------------------------------------------------

static bool has_gguf_magic(const fs::path & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    char magic[4] = {0};
    f.read(magic, 4);
    return f.gcount() == 4 && magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F';
}

static bool has_safetensors_file(const fs::path & dir) {
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".safetensors") {
            return true;
        }
    }
    return false;
}

// Files whose changes conservatively invalidate the conversion cache.
static std::vector<fs::path> safetensors_source_files(const fs::path & dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".safetensors") {
            out.push_back(entry.path());
        }
    }
    for (const char * name : {"config.json", "tokenizer.json", "tokenizer_config.json",
                              "model.safetensors.index.json", "generation_config.json"}) {
        fs::path p = dir / name;
        if (fs::exists(p, ec)) {
            out.push_back(p);
        }
    }
    return out;
}

static fs::path executable_dir() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD) buf.size());
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
#else
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) {
        return {};
    }
    return exe.parent_path();
#endif
}

// Quote one argument for the shell `popen` hands the command line to: a
// POSIX sh on Unix (single quotes, with embedded quotes closed and
// re-opened), cmd.exe on Windows (double quotes; a literal `"` is not
// representable inside a cmd.exe-quoted argument and is dropped).
static std::string shquote(const std::string & s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c != '"') {
            out += c;
        }
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
#endif
}

// The interpreter name differs by platform: `python3` on Unix, where a
// bare `python` may still be Python 2 or absent; `python` on Windows, where
// the python.org installer provides no `python3` alias.
#ifdef _WIN32
static const char * PYTHON_EXE = "python";
#else
static const char * PYTHON_EXE = "python3";
#endif

// ---------------------------------------------------------------------------
// converter script resolution
// ---------------------------------------------------------------------------

static std::string resolve_converter_script(std::string & err) {
    if (const char * env = std::getenv("ONYX_ENGINE_CONVERT_SCRIPT")) {
        if (*env) {
            return env;
        }
    }

    std::error_code ec;
    fs::path bindir = executable_dir();
    if (!bindir.empty()) {
        for (const char * rel : {"../scripts/", "../../scripts/"}) {
            fs::path candidate = bindir / rel / CONVERT_SCRIPT_NAME;
            if (fs::exists(candidate, ec)) {
                return candidate.lexically_normal().string();
            }
        }
    }

#ifdef ONYX_ENGINE_SOURCE_DIR
    {
        fs::path candidate = fs::path(ONYX_ENGINE_SOURCE_DIR) / "scripts" / CONVERT_SCRIPT_NAME;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
#endif

    err = std::string("cannot find the safetensors converter script ('") + CONVERT_SCRIPT_NAME +
          "'); set ONYX_ENGINE_CONVERT_SCRIPT to its path";
    return "";
}

// ---------------------------------------------------------------------------
// running the converter
// ---------------------------------------------------------------------------

// Runs `python3 <script> <model_dir> --outfile <outfile> --outtype f16`,
// streaming its combined stdout+stderr through to our stderr (each line
// prefixed) and keeping the last `keep` lines for error reporting. Returns
// the child's exit code, or -1 if it could not be started/waited on.
static int run_converter(const std::string & script, const std::string & model_dir,
                          const std::string & outfile, std::deque<std::string> & tail, size_t keep) {
    const std::string cmd = std::string(PYTHON_EXE) + " " + shquote(script) + " " + shquote(model_dir) +
                             " --outfile " + shquote(outfile) + " --outtype f16 2>&1";

#ifdef _WIN32
    FILE * pipe = _popen(cmd.c_str(), "r");
#else
    FILE * pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        return -1;
    }

    std::array<char, 4096> buf{};
    std::string line;
    auto flush_line = [&]() {
        fprintf(stderr, "[convert] %s\n", line.c_str());
        tail.push_back(line);
        if (tail.size() > keep) {
            tail.pop_front();
        }
        line.clear();
    };
    while (fgets(buf.data(), (int) buf.size(), pipe)) {
        for (char * p = buf.data(); *p; ++p) {
            if (*p == '\n') {
                flush_line();
            } else {
                line += *p;
            }
        }
    }
    if (!line.empty()) {
        flush_line();
    }

#ifdef _WIN32
    // _pclose returns the child's exit status directly.
    int status = _pclose(pipe);
    return status == -1 ? -1 : status;
#else
    int status = pclose(pipe);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

// ---------------------------------------------------------------------------
// public entry point
// ---------------------------------------------------------------------------

std::string onyx_resolve_model(const onyx_args & args, std::string & err) {
    std::error_code ec;
    fs::path model_path(args.model_path);

    if (!fs::exists(model_path, ec)) {
        err = "model path does not exist: '" + args.model_path + "'";
        return "";
    }

    // already a GGUF file?
    if (fs::is_regular_file(model_path, ec)) {
        if (model_path.extension() == ".gguf" || has_gguf_magic(model_path)) {
            return model_path.string();
        }
    }

    // resolve the safetensors model directory
    fs::path model_dir;
    if (fs::is_directory(model_path, ec)) {
        model_dir = model_path;
    } else if (fs::is_regular_file(model_path, ec) && model_path.extension() == ".safetensors") {
        model_dir = model_path.parent_path();
        if (model_dir.empty()) {
            model_dir = ".";
        }
    } else {
        err = "'" + args.model_path + "' is neither a GGUF file nor a recognized safetensors model "
              "(expected a .gguf file, a directory with config.json + *.safetensors, or a .safetensors file)";
        return "";
    }

    if (!fs::exists(model_dir / "config.json", ec) || !has_safetensors_file(model_dir)) {
        err = "'" + model_dir.string() + "' does not look like a safetensors model directory "
              "(expected config.json and at least one *.safetensors file)";
        return "";
    }

    // cache location: <convert_dir or model dir>/[onyx-cache/]...
    // The path hash keeps two different models that share a directory name
    // from colliding under a shared --convert-dir.
    fs::path cache_dir = args.convert_dir.empty() ? (model_dir / "onyx-cache") : fs::path(args.convert_dir);
    const std::string abs_dir = fs::absolute(model_dir, ec).string();
    std::string dirname = fs::path(abs_dir).filename().string();
    if (dirname.empty()) {
        dirname = "model";
    }
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (char c : abs_dir) {
        h = (h ^ (unsigned char) c) * 1099511628211ULL;
    }
    char hash_hex[17];
    snprintf(hash_hex, sizeof(hash_hex), "%016llx", (unsigned long long) h);
    fs::path cache_path = cache_dir / (dirname + "-" + std::string(hash_hex, 8) + "-converter-v2-f16.gguf");
    const std::string script = resolve_converter_script(err);
    if (script.empty()) {
        return "";
    }

    // reuse a fresh cached conversion if one exists
    if (fs::exists(cache_path, ec)) {
        auto cache_time = fs::last_write_time(cache_path, ec);
        bool fresh = !ec && fs::file_size(cache_path, ec) >= 32 && !ec && has_gguf_magic(cache_path);
        if (fresh && fs::last_write_time(script, ec) > cache_time) fresh = false;
        if (fresh) {
            for (const auto & src : safetensors_source_files(model_dir)) {
                auto src_time = fs::last_write_time(src, ec);
                if (ec || src_time > cache_time) {
                    fresh = false;
                    break;
                }
            }
        }
        if (fresh) {
            fprintf(stderr, "onyx-engine: reusing cached conversion at '%s'\n", cache_path.string().c_str());
            return cache_path.string();
        }
    }

    fs::create_directories(cache_dir, ec);
    if (ec) {
        err = "cannot create conversion cache directory '" + cache_dir.string() + "': " + ec.message();
        return "";
    }
    fs::path tmp_path = cache_path;
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    tmp_path += "." + std::to_string(process_id) + "." +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";

    fprintf(stderr, "onyx-engine: converting safetensors model '%s' to GGUF ...\n", model_dir.string().c_str());

    std::deque<std::string> tail;
    const size_t keep_lines = 20;
    int rc = run_converter(script, model_dir.string(), tmp_path.string(), tail, keep_lines);

    if (rc != 0) {
        fs::remove(tmp_path, ec);
        err = "safetensors conversion failed (exit code " + std::to_string(rc) + ")\n";
        if (!tail.empty()) {
            err += "last " + std::to_string(tail.size()) + " line(s) of converter output:\n";
            for (const auto & l : tail) {
                err += "  " + l + "\n";
            }
        }
        for (const auto & line : tail) {
            if (line.find("No module named 'numpy'") != std::string::npos || line.find("python3: not found") != std::string::npos) {
                err += "hint: conversion requires python3 with numpy installed";
                break;
            }
        }
        return "";
    }

    ec.clear();
#ifdef _WIN32
    if (!MoveFileExW(tmp_path.c_str(), cache_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code((int) GetLastError(), std::system_category());
    }
#else
    fs::rename(tmp_path, cache_path, ec);
#endif
    if (ec) {
        std::error_code verify_ec;
        bool other_fresh = fs::exists(cache_path, verify_ec) && !verify_ec &&
                           fs::file_size(cache_path, verify_ec) >= 32 && !verify_ec && has_gguf_magic(cache_path);
        if (other_fresh) {
            const auto cache_time = fs::last_write_time(cache_path, verify_ec);
            const auto script_time = fs::last_write_time(script, verify_ec);
            other_fresh = !verify_ec && script_time <= cache_time;
            for (const auto & src : safetensors_source_files(model_dir)) {
                const auto src_time = fs::last_write_time(src, verify_ec);
                if (verify_ec || src_time > cache_time) other_fresh = false;
            }
        }
        if (other_fresh) {
            // Another process finished an equivalent conversion first.
            fs::remove(tmp_path, ec);
            return cache_path.string();
        }
        err = "failed to atomically move converted GGUF into place: " + ec.message();
        fs::remove(tmp_path, ec);
        return "";
    }

    fprintf(stderr, "onyx-engine: conversion complete, cached at '%s'\n", cache_path.string().c_str());
    return cache_path.string();
}
