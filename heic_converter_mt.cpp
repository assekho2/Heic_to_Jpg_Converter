/*
Build (Apple Silicon / Homebrew):
g++ -std=c++17 -O2 -pthread -o heic_converter_mt heic_converter_mt.cpp \
  -I/opt/homebrew/include \
  -L/opt/homebrew/lib \
  -L/opt/homebrew/opt/jpeg/lib \
  -I/opt/homebrew/opt/jpeg/include \
  -lheif -ljpeg

Run examples:
  ./heic_converter_mt                          # defaults: Photos -> output, quality 90, threads = hw cores
  ./heic_converter_mt Photos output 90 8       # explicit
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <string>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "/opt/homebrew/include/libheif/heif.h"
#include <jpeglib.h>

#define MAX_PATH 1024

typedef struct {
    struct heif_context* ctx;
    struct heif_image_handle* handle;
    struct heif_image* img;
    FILE* outfile;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
} ConversionContext;

static inline bool ends_with_heic_case_insensitive(const char* filename) {
    if (!filename) return false;
    const char* dot = strrchr(filename, '.');
    if (!dot) return false;

    // Compare ".heic" case-insensitively without relying on strcasecmp portability
    const char* ext = dot;
    const char* target = ".heic";
    while (*ext && *target) {
        if (std::tolower((unsigned char)*ext) != std::tolower((unsigned char)*target)) return false;
        ++ext; ++target;
    }
    return (*ext == '\0' && *target == '\0');
}

static inline bool ensure_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return (mkdir(path, 0755) == 0);
}

static void cleanup_context(ConversionContext* ctx) {
    if (!ctx) return;

    if (ctx->outfile) {
        fclose(ctx->outfile);
        ctx->outfile = nullptr;
    }

    if (ctx->img) {
        heif_image_release(ctx->img);
        ctx->img = nullptr;
    }

    if (ctx->handle) {
        heif_image_handle_release(ctx->handle);
        ctx->handle = nullptr;
    }

    if (ctx->ctx) {
        heif_context_free(ctx->ctx);
        ctx->ctx = nullptr;
    }

    jpeg_destroy_compress(&ctx->cinfo);
}

static bool convert_heic_to_jpg(const char* input_path, const char* output_dir, int jpeg_quality) {
    ConversionContext ctx{};
    struct heif_error err{};

    // Init JPEG error handler
    ctx.cinfo.err = jpeg_std_error(&ctx.jerr);
    jpeg_create_compress(&ctx.cinfo);

    // Ensure cleanup no matter how we exit
    struct Guard {
        ConversionContext* c;
        ~Guard() { cleanup_context(c); }
    } guard{&ctx};

    // Init HEIF context
    ctx.ctx = heif_context_alloc();
    if (!ctx.ctx) {
        std::fprintf(stderr, "Failed to allocate HEIF context\n");
        return false;
    }

    // Read HEIC file
    err = heif_context_read_from_file(ctx.ctx, input_path, nullptr);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not read HEIC file: %s\n", input_path);
        return false;
    }

    // Get primary image handle
    err = heif_context_get_primary_image_handle(ctx.ctx, &ctx.handle);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not get primary image handle: %s\n", input_path);
        return false;
    }

    // Decode
    err = heif_decode_image(ctx.handle, &ctx.img,
                            heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB,
                            nullptr);
    if (err.code != heif_error_Ok) {
        std::fprintf(stderr, "Could not decode image: %s\n", input_path);
        return false;
    }

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(ctx.img, heif_channel_interleaved, &stride);
    if (!data || stride <= 0) {
        std::fprintf(stderr, "Could not get decoded plane: %s\n", input_path);
        return false;
    }

    const int width  = heif_image_get_width(ctx.img, heif_channel_interleaved);
    const int height = heif_image_get_height(ctx.img, heif_channel_interleaved);
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "Invalid image dimensions: %s\n", input_path);
        return false;
    }

    // Build output filename: output_dir/base_name.jpg
    const char* base = std::strrchr(input_path, '/');
    base = base ? base + 1 : input_path;

    std::string base_name(base);
    auto dotpos = base_name.find_last_of('.');
    if (dotpos != std::string::npos) base_name.resize(dotpos);

    char output_path[MAX_PATH];
    std::snprintf(output_path, sizeof(output_path), "%s/%s.jpg", output_dir, base_name.c_str());

    ctx.outfile = std::fopen(output_path, "wb");
    if (!ctx.outfile) {
        std::fprintf(stderr, "Could not create output file: %s\n", output_path);
        return false;
    }

    // JPEG setup
    jpeg_stdio_dest(&ctx.cinfo, ctx.outfile);
    ctx.cinfo.image_width = width;
    ctx.cinfo.image_height = height;
    ctx.cinfo.input_components = 3;
    ctx.cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&ctx.cinfo);
    jpeg_set_quality(&ctx.cinfo, jpeg_quality, TRUE);
    jpeg_start_compress(&ctx.cinfo, TRUE);

    // Write scanlines
    while (ctx.cinfo.next_scanline < ctx.cinfo.image_height) {
        JSAMPROW row_pointer = (JSAMPROW)&data[ctx.cinfo.next_scanline * stride];
        jpeg_write_scanlines(&ctx.cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&ctx.cinfo);
    return true;
}

/* ---------------- Thread pool (simple work queue) ---------------- */

class WorkQueue {
public:
    void push(std::string item) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Returns false when no more work and closed
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::string> q_;
    bool closed_ = false;
};

static std::vector<std::string> list_heic_files(const char* input_dir) {
    std::vector<std::string> files;

    DIR* dir = opendir(input_dir);
    if (!dir) {
        std::fprintf(stderr, "Could not open input directory '%s': %s\n", input_dir, std::strerror(errno));
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!ends_with_heic_case_insensitive(entry->d_name)) continue;

        char fullpath[MAX_PATH];
        std::snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir, entry->d_name);

        // Skip non-regular files
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        files.emplace_back(fullpath);
    }

    closedir(dir);
    return files;
}

static int parse_int(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return fallback;
    if (v > INT32_MAX) return fallback;
    if (v < INT32_MIN) return fallback;
    return (int)v;
}

int main(int argc, char** argv) {
    const char* input_dir = "Photos";
    const char* output_dir = "output";
    int jpeg_quality = 90;

    unsigned int hw = std::thread::hardware_concurrency();
    int threads = (hw == 0) ? 4 : (int)hw;  // M1 Air typically reports 8

    if (argc >= 2) input_dir = argv[1];
    if (argc >= 3) output_dir = argv[2];
    if (argc >= 4) jpeg_quality = parse_int(argv[3], 90);
    if (argc >= 5) threads = parse_int(argv[4], threads);

    if (jpeg_quality < 1 || jpeg_quality > 100) {
        std::fprintf(stderr, "Invalid quality value. Use 1-100.\n");
        return 2;
    }
    if (threads < 1) threads = 1;

    if (!ensure_directory(output_dir)) {
        std::fprintf(stderr, "Failed to create output directory '%s'\n", output_dir);
        return 1;
    }

    std::vector<std::string> files = list_heic_files(input_dir);
    if (files.empty()) {
        std::printf("No HEIC files found in '%s'.\n", input_dir);
        return 0;
    }

    std::printf("Found %zu HEIC files. Converting with quality %d using %d threads...\n",
                files.size(), jpeg_quality, threads);

    WorkQueue wq;
    std::atomic<int> converted_ok{0};
    std::atomic<int> converted_fail{0};

    std::mutex print_mtx;

    // Worker function
    auto worker = [&] {
        std::string path;
        while (wq.pop(path)) {
            bool ok = convert_heic_to_jpg(path.c_str(), output_dir, jpeg_quality);
            if (ok) converted_ok.fetch_add(1, std::memory_order_relaxed);
            else converted_fail.fetch_add(1, std::memory_order_relaxed);

            
            //int done = converted_ok.load(std::memory_order_relaxed) + converted_fail.load(std::memory_order_relaxed);
            //if ((done % 25) == 0) {
            //    std::lock_guard<std::mutex> lk(print_mtx);
            //    std::printf("Progress: %d/%zu done\n", done, files.size());
            //}
        }
    };

    // Start workers
    std::vector<std::thread> pool;
    pool.reserve((size_t)threads);
    for (int i = 0; i < threads; i++) pool.emplace_back(worker);

    // Enqueue work
    for (auto& f : files) wq.push(std::move(f));
    wq.close();

    // Join
    for (auto& t : pool) t.join();

    int ok = converted_ok.load();
    int fail = converted_fail.load();

    std::printf("Done. Converted OK: %d, Failed: %d\n", ok, fail);
    return (fail == 0) ? 0 : 1;
}