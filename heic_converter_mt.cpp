/*
Cross-platform (Windows / macOS / Linux) multithreaded HEIC -> JPEG converter.

This is the command-line front-end; the actual decode/encode/threading engine
lives in converter_core.{h,cpp} and is shared with the GUI.

Build with the included Makefile:
    make heic_converter_mt

Run examples:
  ./heic_converter_mt                          # convert .heic files in the current directory -> ./output
  ./heic_converter_mt ~/Pictures -q 85 -t 8    # explicit input dir, quality, threads
  ./heic_converter_mt --help
*/

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "converter_core.h"

static int parse_int(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return fallback;
    if (v > INT32_MAX || v < INT32_MIN) return fallback;
    return (int)v;
}

#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif

static void print_usage(const char* prog) {
    std::printf(
        "heic_converter_mt %s - batch HEIC to JPEG converter\n"
        "\n"
        "Usage: %s [options] [input_dir]\n"
        "\n"
        "Converts every .heic file in input_dir (default: current directory)\n"
        "to a .jpg in the output directory, using multiple threads.\n"
        "\n"
        "Options:\n"
        "  -o, --output <dir>   output directory (default: output)\n"
        "  -q, --quality <n>    JPEG quality 1-100 (default: 90)\n"
        "  -t, --threads <n>    worker threads (default: CPU cores)\n"
        "  -h, --help           show this help and exit\n"
        "  -V, --version        show version and exit\n",
        APP_VERSION, prog);
}

int main(int argc, char** argv) {
    const char* input_dir = ".";
    const char* output_dir = "output";
    int jpeg_quality = 90;

    unsigned int hw = std::thread::hardware_concurrency();
    int threads = (hw == 0) ? 4 : (int)hw;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(arg, "-V") == 0 || std::strcmp(arg, "--version") == 0) {
            std::printf("heic_converter_mt %s\n", APP_VERSION);
            return 0;
        } else if (std::strcmp(arg, "-o") == 0 || std::strcmp(arg, "--output") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a directory argument\n", arg); return 2; }
            output_dir = argv[i];
        } else if (std::strcmp(arg, "-q") == 0 || std::strcmp(arg, "--quality") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a number argument\n", arg); return 2; }
            jpeg_quality = parse_int(argv[i], -1);
        } else if (std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--threads") == 0) {
            if (++i >= argc) { std::fprintf(stderr, "%s requires a number argument\n", arg); return 2; }
            threads = parse_int(argv[i], -1);
        } else if (arg[0] == '-' && arg[1] != '\0') {
            std::fprintf(stderr, "Unknown option: %s (try --help)\n", arg);
            return 2;
        } else {
            input_dir = arg;
        }
    }

    if (jpeg_quality < 1 || jpeg_quality > 100) {
        std::fprintf(stderr, "Invalid quality value. Use 1-100.\n");
        return 2;
    }
    if (threads < 1) {
        std::fprintf(stderr, "Invalid thread count. Use a positive number.\n");
        return 2;
    }

    if (!hjc::ensure_directory(output_dir)) {
        std::fprintf(stderr, "Failed to create output directory '%s'\n", output_dir);
        return 1;
    }

    std::vector<std::string> files = hjc::list_heic_files(input_dir);
    if (files.empty()) {
        std::printf("No HEIC files found in '%s'.\n", input_dir);
        return 0;
    }

    std::printf("Found %zu HEIC files. Converting with quality %d using %d threads...\n",
                files.size(), jpeg_quality, threads);

    hjc::Progress progress;
    hjc::convert_files(files, output_dir, jpeg_quality, threads, progress,
                       [](const std::string& path, bool ok, const std::string& err) {
                           if (!ok) std::fprintf(stderr, "Failed: %s (%s)\n",
                                                 path.c_str(), err.c_str());
                       });

    int ok = progress.ok.load();
    int fail = progress.failed.load();
    std::printf("Done. Converted OK: %d, Failed: %d\n", ok, fail);
    return (fail == 0) ? 0 : 1;
}
