/* Emit LibRaw's rawpy-equivalent 16-bit processed bitmap for oracle comparison. */
#include <libraw/libraw.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: libraw_processed_probe INPUT OUTPUT_U16\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) return 3;
    const std::streamoff length = input.tellg();
    if (length <= 0) return 3;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) return 3;

    LibRaw raw;
    raw.imgdata.rawparams.max_raw_memory_mb = 512;
    int rc = raw.open_buffer(bytes.data(), bytes.size());
    if (rc == LIBRAW_SUCCESS) rc = raw.unpack();
    if (rc != LIBRAW_SUCCESS) {
        std::cerr << "LibRaw input failed: " << libraw_strerror(rc) << '\n';
        return 4;
    }
    auto& params = raw.imgdata.params;
    params.output_color = 6;
    params.output_bps = 16;
    params.no_auto_bright = 1;
    params.gamm[0] = 1.0;
    params.gamm[1] = 1.0;
    params.use_camera_wb = 1;
    params.half_size = 0;
    params.threshold = 0.0f;
    rc = raw.dcraw_process();
    if (rc != LIBRAW_SUCCESS) {
        std::cerr << "LibRaw process failed: " << libraw_strerror(rc) << '\n';
        return 5;
    }

    int status = LIBRAW_SUCCESS;
    libraw_processed_image_t* image = raw.dcraw_make_mem_image(&status);
    if (!image || status != LIBRAW_SUCCESS ||
        image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3 || image->bits != 16) {
        if (image) LibRaw::dcraw_clear_mem(image);
        return 6;
    }
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(image->data), image->data_size);
    const bool wrote = static_cast<bool>(output);
    std::cout << "libraw=" << LibRaw::version()
              << " width=" << image->width
              << " height=" << image->height
              << " bytes=" << image->data_size << '\n';
    LibRaw::dcraw_clear_mem(image);
    return wrote ? 0 : 7;
}
