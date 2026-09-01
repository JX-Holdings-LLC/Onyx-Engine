#include "convert.h"

#include "args.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/wait.h>

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

// every *.safetensors file plus config.json, for cache-freshness comparisons
static std::vector<fs::path> safetensors_source_files(const fs::path & dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".safetensors") {
            out.push_back(entry.path());
        }
    }
    fs::path config = dir / "config.json";
    if (fs::exists(config, ec)) {
        out.push_back(config);
    }
    return out;
}

static fs::path executable_dir() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) {
        return {};
    }
    return exe.parent_path();
}

static std::string shquote(const std::string & s) {
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
}

// ---------------------------------------------------------------------------
// converter script resolution
// ---------------------------------------------------------------------------

static std::string resolve_converter_script(std::string & err) {
    if (const char * env = std::getenv("JX_ENGINE_CONVERT_SCRIPT")) {
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

#ifdef JX_ENGINE_SOURCE_DIR
    {
        fs::path candidate = fs::path(JX_ENGINE_SOURCE_DIR) / "scripts" / CONVERT_SCRIPT_NAME;
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
#endif

    err = std::string("cannot find the safetensors converter script ('") + CONVERT_SCRIPT_NAME +
          "'); set JX_ENGINE_CONVERT_SCRIPT to its path";
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
    const std::string cmd = "python3 " + shquote(script) + " " + shquote(model_dir) +
                             " --outfile " + shquote(outfile) + " --outtype f16 2>&1";

    FILE * pipe = popen(cmd.c_str(), "r");
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

    int status = pclose(pipe);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// public entry point
// ---------------------------------------------------------------------------

std::string jx_resolve_model(const jx_args & args, std::string & err) {
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

    // cache location: <convert_dir or model dir>/jx-cache/<dirname>-f16.gguf
    fs::path cache_dir = args.convert_dir.empty() ? (model_dir / "jx-cache") : fs::path(args.convert_dir);
    std::string dirname = fs::absolute(model_dir, ec).filename().string();
    if (dirname.empty()) {
        dirname = "model";
    }
    fs::path cache_path = cache_dir / (dirname + "-f16.gguf");

    // reuse a fresh cached conversion if one exists
    if (fs::exists(cache_path, ec)) {
        auto cache_time = fs::last_write_time(cache_path, ec);
        bool fresh = !ec;
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
            fprintf(stderr, "jx-engine: reusing cached conversion at '%s'\n", cache_path.string().c_str());
            return cache_path.string();
        }
    }

    const std::string script = resolve_converter_script(err);
    if (script.empty()) {
        return "";
    }

    fs::create_directories(cache_dir, ec);
    fs::path tmp_path = cache_dir / (dirname + "-f16.gguf.tmp");
    fs::remove(tmp_path, ec);

    fprintf(stderr, "jx-engine: converting safetensors model '%s' to GGUF ...\n", model_dir.string().c_str());

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
        err += "hint: conversion requires python3 with numpy installed";
        return "";
    }

    fs::rename(tmp_path, cache_path, ec);
    if (ec) {
        // fall back to copy+remove, e.g. across filesystems
        ec.clear();
        fs::copy_file(tmp_path, cache_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "failed to move converted GGUF into place: " + ec.message();
            return "";
        }
        fs::remove(tmp_path, ec);
    }

    fprintf(stderr, "jx-engine: conversion complete, cached at '%s'\n", cache_path.string().c_str());
    return cache_path.string();
}
