#include "converter_core.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <sys/stat.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <direct.h>
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

#include <libheif/heif.h>
#include <jpeglib.h>

// MSVC's <sys/stat.h> does not provide these POSIX macros
#if defined(_WIN32) && !defined(S_ISDIR)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#if defined(_WIN32) && !defined(S_ISREG)
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

#define PATH_BUF_SIZE 1024

namespace hjc {

bool ends_with_heic_ci(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    if (ext.size() != 5) return false;
    static const char* target = ".heic";
    for (size_t i = 0; i < 5; ++i) {
        if (std::tolower((unsigned char)ext[i]) != target[i]) return false;
    }
    return true;
}

std::string path_basename(const std::string& path) {
    size_t slash = path.find_last_of('/');
#ifdef _WIN32
    size_t bslash = path.find_last_of('\\');
    if (slash == std::string::npos || (bslash != std::string::npos && bslash > slash))
        slash = bslash;
#endif
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
#ifdef _WIN32
    return (_mkdir(path.c_str()) == 0);
#else
    return (mkdir(path.c_str(), 0755) == 0);
#endif
}

namespace {

struct ConversionContext {
    struct heif_context* ctx = nullptr;
    struct heif_image_handle* handle = nullptr;
    struct heif_image* img = nullptr;
    FILE* outfile = nullptr;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
};

void cleanup_context(ConversionContext* ctx) {
    if (!ctx) return;
    if (ctx->outfile) { fclose(ctx->outfile); ctx->outfile = nullptr; }
    if (ctx->img)     { heif_image_release(ctx->img); ctx->img = nullptr; }
    if (ctx->handle)  { heif_image_handle_release(ctx->handle); ctx->handle = nullptr; }
    if (ctx->ctx)     { heif_context_free(ctx->ctx); ctx->ctx = nullptr; }
    jpeg_destroy_compress(&ctx->cinfo);
}

} // namespace

bool convert_heic_to_jpg(const std::string& input_path,
                         const std::string& output_dir,
                         int quality,
                         std::string& err) {
    ConversionContext ctx{};
    struct heif_error herr{};

    ctx.cinfo.err = jpeg_std_error(&ctx.jerr);
    jpeg_create_compress(&ctx.cinfo);

    struct Guard {
        ConversionContext* c;
        ~Guard() { cleanup_context(c); }
    } guard{&ctx};

    ctx.ctx = heif_context_alloc();
    if (!ctx.ctx) { err = "Failed to allocate HEIF context"; return false; }

    herr = heif_context_read_from_file(ctx.ctx, input_path.c_str(), nullptr);
    if (herr.code != heif_error_Ok) { err = "Could not read HEIC file"; return false; }

    herr = heif_context_get_primary_image_handle(ctx.ctx, &ctx.handle);
    if (herr.code != heif_error_Ok) { err = "Could not get primary image handle"; return false; }

    herr = heif_decode_image(ctx.handle, &ctx.img,
                             heif_colorspace_RGB,
                             heif_chroma_interleaved_RGB,
                             nullptr);
    if (herr.code != heif_error_Ok) { err = "Could not decode image"; return false; }

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(ctx.img, heif_channel_interleaved, &stride);
    if (!data || stride <= 0) { err = "Could not get decoded plane"; return false; }

    const int width  = heif_image_get_width(ctx.img, heif_channel_interleaved);
    const int height = heif_image_get_height(ctx.img, heif_channel_interleaved);
    if (width <= 0 || height <= 0) { err = "Invalid image dimensions"; return false; }

    std::string base = path_basename(input_path);
    auto dotpos = base.find_last_of('.');
    if (dotpos != std::string::npos) base.resize(dotpos);

    char output_path[PATH_BUF_SIZE];
    std::snprintf(output_path, sizeof(output_path), "%s/%s.jpg", output_dir.c_str(), base.c_str());

    ctx.outfile = std::fopen(output_path, "wb");
    if (!ctx.outfile) { err = "Could not create output file"; return false; }

    jpeg_stdio_dest(&ctx.cinfo, ctx.outfile);
    ctx.cinfo.image_width = width;
    ctx.cinfo.image_height = height;
    ctx.cinfo.input_components = 3;
    ctx.cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&ctx.cinfo);
    jpeg_set_quality(&ctx.cinfo, quality, TRUE);
    jpeg_start_compress(&ctx.cinfo, TRUE);

    while (ctx.cinfo.next_scanline < ctx.cinfo.image_height) {
        JSAMPROW row_pointer = (JSAMPROW)&data[ctx.cinfo.next_scanline * stride];
        jpeg_write_scanlines(&ctx.cinfo, &row_pointer, 1);
    }

    jpeg_finish_compress(&ctx.cinfo);
    return true;
}

std::vector<std::string> list_heic_files(const std::string& input_dir) {
    std::vector<std::string> files;

#ifdef _WIN32
    char pattern[PATH_BUF_SIZE];
    std::snprintf(pattern, sizeof(pattern), "%s\\*", input_dir.c_str());

    WIN32_FIND_DATAA find_data;
    HANDLE find = FindFirstFileA(pattern, &find_data);
    if (find == INVALID_HANDLE_VALUE) return files;

    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!ends_with_heic_ci(find_data.cFileName)) continue;
        char fullpath[PATH_BUF_SIZE];
        std::snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir.c_str(), find_data.cFileName);
        files.emplace_back(fullpath);
    } while (FindNextFileA(find, &find_data));

    FindClose(find);
#else
    DIR* dir = opendir(input_dir.c_str());
    if (!dir) return files;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!ends_with_heic_ci(entry->d_name)) continue;
        char fullpath[PATH_BUF_SIZE];
        std::snprintf(fullpath, sizeof(fullpath), "%s/%s", input_dir.c_str(), entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        files.emplace_back(fullpath);
    }
    closedir(dir);
#endif

    return files;
}

namespace {

// Simple closable work queue feeding the thread pool.
class WorkQueue {
public:
    void push(std::string item) {
        { std::lock_guard<std::mutex> lk(m_); q_.push(std::move(item)); }
        cv_.notify_one();
    }
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        cv_.notify_all();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<std::string> q_;
    bool closed_ = false;
};

} // namespace

void convert_files(const std::vector<std::string>& files,
                   const std::string& output_dir,
                   int quality,
                   int threads,
                   Progress& progress,
                   const FileDoneCb& on_file_done) {
    progress.total.store((int)files.size());
    progress.done.store(0);
    progress.ok.store(0);
    progress.failed.store(0);
    progress.cancel.store(false);
    progress.running.store(true);

    if (files.empty()) { progress.running.store(false); return; }
    if (threads < 1) threads = 1;

    ensure_directory(output_dir);

    WorkQueue wq;

    auto worker = [&] {
        std::string path;
        while (wq.pop(path)) {
            if (progress.cancel.load()) break;
            std::string err;
            bool ok = convert_heic_to_jpg(path, output_dir, quality, err);
            if (ok) progress.ok.fetch_add(1, std::memory_order_relaxed);
            else    progress.failed.fetch_add(1, std::memory_order_relaxed);
            progress.done.fetch_add(1, std::memory_order_relaxed);
            if (on_file_done) on_file_done(path, ok, err);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve((size_t)threads);
    for (int i = 0; i < threads; i++) pool.emplace_back(worker);

    for (const auto& f : files) wq.push(f);
    wq.close();

    for (auto& t : pool) t.join();

    progress.running.store(false);
}

} // namespace hjc
