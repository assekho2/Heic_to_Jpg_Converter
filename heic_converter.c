/*
compile with, 
gcc -o heic_converter heic_converter.c \
-I/opt/homebrew/include \
-L/opt/homebrew/lib \
-L/opt/homebrew/opt/jpeg/lib \
-I/opt/homebrew/opt/jpeg/include \
-lheif -ljpeg

run with, 
./heic_converter
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "/opt/homebrew/include/libheif/heif.h"
#include <jpeglib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define MAX_PATH 1024

typedef struct {
    struct heif_context* ctx;
    struct heif_image_handle* handle;
    struct heif_image* img;
    FILE* outfile;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
} ConversionContext;

static inline int has_heic_extension(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".HEIC") == 0);  // Case-insensitive comparison
}

static inline int ensure_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return 1;  // Directory exists
    return (mkdir(path, 0755) == 0);
}

static void cleanup_context(ConversionContext* ctx) {
    if (!ctx) return;
    
    if (ctx->outfile) {
        fclose(ctx->outfile);
        ctx->outfile = NULL;
    }
    
    if (ctx->img) {
        heif_image_release(ctx->img);
        ctx->img = NULL;
    }
    
    if (ctx->handle) {
        heif_image_handle_release(ctx->handle);
        ctx->handle = NULL;
    }
    
    if (ctx->ctx) {
        heif_context_free(ctx->ctx);
        ctx->ctx = NULL;
    }
    
    jpeg_destroy_compress(&ctx->cinfo);
}

static int convert_heic_to_jpg(const char* input_path, const char* output_dir, int jpeg_quality) {
    ConversionContext ctx = {0};  // Zero-initialize all members
    struct heif_error err;
    int success = 0;
    
    // Initialize JPEG error handler first
    ctx.cinfo.err = jpeg_std_error(&ctx.jerr);
    jpeg_create_compress(&ctx.cinfo);
    
    // Initialize HEIF context
    if (!(ctx.ctx = heif_context_alloc())) {
        fprintf(stderr, "Failed to allocate HEIF context\n");
        goto cleanup;
    }
    
    // Read HEIC file
    if ((err = heif_context_read_from_file(ctx.ctx, input_path, NULL)).code != heif_error_Ok) {
        fprintf(stderr, "Could not read HEIC file: %s\n", input_path);
        goto cleanup;
    }
    
    // Get primary image handle
    if ((err = heif_context_get_primary_image_handle(ctx.ctx, &ctx.handle)).code != heif_error_Ok) {
        fprintf(stderr, "Could not get primary image handle\n");
        goto cleanup;
    }
    
    // Decode the image
    if ((err = heif_decode_image(ctx.handle, &ctx.img, heif_colorspace_RGB, 
                                heif_chroma_interleaved_RGB, NULL)).code != heif_error_Ok) {
        fprintf(stderr, "Could not decode image\n");
        goto cleanup;
    }
    
    // Get image data
    int stride;
    const uint8_t* data = heif_image_get_plane_readonly(ctx.img, heif_channel_interleaved, &stride);
    
    // Create output filename
    char output_filename[MAX_PATH];
    const char* base_name = strrchr(input_path, '/');
    base_name = base_name ? base_name + 1 : input_path;
    
    snprintf(output_filename, sizeof(output_filename), "%s/%.*s.jpg",
             output_dir, (int)(strrchr(base_name, '.') - base_name), base_name);
    
    // Open output file
    if (!(ctx.outfile = fopen(output_filename, "wb"))) {
        fprintf(stderr, "Could not create output file: %s\n", output_filename);
        goto cleanup;
    }
    
    // Setup JPEG compression
    jpeg_stdio_dest(&ctx.cinfo, ctx.outfile);
    ctx.cinfo.image_width = heif_image_get_width(ctx.img, heif_channel_interleaved);
    ctx.cinfo.image_height = heif_image_get_height(ctx.img, heif_channel_interleaved);
    ctx.cinfo.input_components = 3;
    ctx.cinfo.in_color_space = JCS_RGB;
    
    jpeg_set_defaults(&ctx.cinfo);
    jpeg_set_quality(&ctx.cinfo, jpeg_quality, TRUE);
    jpeg_start_compress(&ctx.cinfo, TRUE);
    
    // Write scan lines
    while (ctx.cinfo.next_scanline < ctx.cinfo.image_height) {
        JSAMPROW row_pointer = (JSAMPROW)&data[ctx.cinfo.next_scanline * stride];
        jpeg_write_scanlines(&ctx.cinfo, &row_pointer, 1);
    }
    
    jpeg_finish_compress(&ctx.cinfo);
    success = 1;

cleanup:
    cleanup_context(&ctx);
    return success;
}

int main(void) {
    const char* input_dir = "Photos";
    const char* output_dir = "output";
    DIR* dir = NULL;
    struct dirent* entry;
    char input_path[MAX_PATH];
    int files_processed = 0;
    int success = 0;
    int jpeg_quality;

    printf("Enter JPEG quality (1-100, recommended 75-95): ");
    if (scanf("%d", &jpeg_quality) != 1 || jpeg_quality < 1 || jpeg_quality > 100) {
        fprintf(stderr, "Invalid quality value. Please enter a number between 1 and 100.\n");
        return 1;
    }
    printf("Using JPEG quality: %d\n", jpeg_quality);
    
    if (!ensure_directory(output_dir)) {
        fprintf(stderr, "Failed to create output directory\n");
        return 1;
    }
    
    if (!(dir = opendir(input_dir))) {
        fprintf(stderr, "Could not open Photos directory: %s\n", strerror(errno));
        return 1;
    }

    printf("Converting files...\n");
    
    while ((entry = readdir(dir)) != NULL) {
        if (has_heic_extension(entry->d_name)) {
            snprintf(input_path, sizeof(input_path), "%s/%s", input_dir, entry->d_name);
            if (convert_heic_to_jpg(input_path, output_dir, jpeg_quality)) {
                files_processed++;
            }
        }
    }
    
    if (closedir(dir) == 0) {
        success = 1;
    }
    
    if (files_processed == 0) {
        printf("No HEIC files found in the Photos directory.\n");
    } else {
        printf("Successfully converted %d photos to JPEG format.\n", files_processed);
    }
    
    return !success;
}