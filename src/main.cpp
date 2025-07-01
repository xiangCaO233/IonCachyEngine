#include <fmt/printf.h>

#include <iostream>
extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char *argv[]) {
    std::cout << "test" << std::endl;

    auto file =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    AVFormatContext *format = nullptr;

    // * @return 0 on success, a negative AVERROR on failure.
    auto open_ret = avformat_open_input(&format, file, nullptr, nullptr);
    if (open_ret < 0) {
        fmt::print("read file \'{}\' failed with error code:{}\n", file,
                   open_ret);
    } else {
        fmt::print("read file \'{}\' success\n", file);
    }

    // * @return >=0 if OK, AVERROR_xxx on error
    auto findstream_ret = avformat_find_stream_info(format, nullptr);
    if (findstream_ret < 0) {
        fmt::print(
            "find file \'{}\'s\' stream info failed with error code:{}\n", file,
            findstream_ret);
    } else {
        fmt::print("read file \'{}\'s\' stream info success with ret:{}\n",
                   file, findstream_ret);
    }
    return 0;
}
