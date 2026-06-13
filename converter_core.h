// Reusable HEIC -> JPEG conversion engine shared by the CLI and the GUI.
//
// The CLI (heic_converter_mt.cpp) and the GUI (gui/main.cpp) both link this
// so the actual decode/encode/threading logic lives in exactly one place.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace hjc {

// True if filename ends with ".heic" (case-insensitive).
bool ends_with_heic_ci(const std::string& filename);

// Last path component, handling both '/' and '\\' separators.
std::string path_basename(const std::string& path);

// Create directory if it does not exist. Returns true if it exists afterwards.
bool ensure_directory(const std::string& path);

// List every .heic file (full paths) in a directory. Empty on error.
std::vector<std::string> list_heic_files(const std::string& dir);

// Convert one HEIC file to a .jpg in output_dir. On failure returns false and
// fills err with a human-readable reason.
bool convert_heic_to_jpg(const std::string& input_path,
                         const std::string& output_dir,
                         int quality,
                         std::string& err);

// Live progress shared between the worker threads and the caller (e.g. the GUI
// polls these from its render loop). All fields are atomic.
struct Progress {
    std::atomic<int>  total{0};
    std::atomic<int>  done{0};
    std::atomic<int>  ok{0};
    std::atomic<int>  failed{0};
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};   // set true to stop early
};

// Called once per finished file (from a worker thread, so keep it thread-safe).
using FileDoneCb = std::function<void(const std::string& path,
                                      bool ok,
                                      const std::string& err)>;

// Convert an explicit list of files using a pool of `threads` workers. Blocks
// until everything is done (or cancelled), so run it on a background thread in
// GUI contexts. `progress` is updated live; `on_file_done` may be empty.
void convert_files(const std::vector<std::string>& files,
                   const std::string& output_dir,
                   int quality,
                   int threads,
                   Progress& progress,
                   const FileDoneCb& on_file_done);

} // namespace hjc
