// Model path resolution (src/convert.cpp), without running the converter.
#include "check.h"

#include "args.h"
#include "convert.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct temp_dir {
    fs::path path;
    temp_dir() {
        path = fs::temp_directory_path() / ("onyx-unit-" + std::to_string((unsigned long long) (uintptr_t) this) +
                                            "-" + std::to_string(fs::file_time_type::clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string & name, const std::string & content) const {
        const fs::path p = path / name;
        std::ofstream(p, std::ios::binary) << content;
        return p;
    }
};

std::string resolve(const fs::path & p, std::string & err) {
    onyx_args a;
    a.model_path = p.string();
    err.clear();
    return onyx_resolve_model(a, err);
}

} // namespace

TEST(convert, missing_path) {
    temp_dir d;
    std::string err;
    CHECK(resolve(d.path / "nope.gguf", err).empty());
    CHECK(err.find("does not exist") != std::string::npos);
}

TEST(convert, gguf_by_extension_is_passed_through) {
    temp_dir d;
    const fs::path p = d.write("model.gguf", "anything");
    std::string err;
    CHECK_EQ(resolve(p, err), p.string());
    CHECK(err.empty());
}

TEST(convert, gguf_by_magic_is_passed_through) {
    temp_dir d;
    const fs::path p = d.write("model.bin", std::string("GGUF") + std::string(60, '\0'));
    std::string err;
    CHECK_EQ(resolve(p, err), p.string());
}

TEST(convert, other_files_are_refused) {
    temp_dir d;
    const fs::path p = d.write("notes.txt", "hello");
    std::string err;
    CHECK(resolve(p, err).empty());
    CHECK(err.find("neither a GGUF file nor a recognized safetensors model") != std::string::npos);
}

TEST(convert, directory_needs_config_and_weights) {
    temp_dir d;
    std::string err;
    CHECK(resolve(d.path, err).empty());           // empty directory
    CHECK(!err.empty());
    d.write("config.json", "{}");
    CHECK(resolve(d.path, err).empty());           // config but no weights
    CHECK(!err.empty());
}
