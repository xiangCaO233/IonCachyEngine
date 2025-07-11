#include <fmt/base.h>
#include <fmt/printf.h>

#include <ice/tool/AllocationTracker.hpp>
#include <iostream>
#include <string>
#include <vector>

#include "ice/manage/AudioPool.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
}

#define AVCALL_CHECK(func)                                         \
    if (int ret = func < 0) {                                      \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];                     \
        av_strerror(ret, errbuf, sizeof(errbuf));                  \
        std::cerr << "at " << std::string(#func) << ": " << errbuf \
                  << std::endl;                                    \
        return ret;                                                \
    }

void test() {
    auto file1 =
        "/home/xiang/Documents/music game maps/Mind Enhancement - "
        "PIKASONIC/Mind Enhancement - PIKASONIC.mp3";
    auto file2 =
        "/home/xiang/Documents/music game maps/osu/Akasha/Snare 3 - B.wav";
    // test
    ice::AudioPool audiopool;
    auto track1 = audiopool.get_or_load(file1);
    track1 = audiopool.get_or_load(file1);
    auto track2 = audiopool.get_or_load(file2);
    track2 = audiopool.get_or_load(file2);
}

int main(int argc, char* argv[]) {
    // AVFormatContext* format = nullptr;

    // AVCALL_CHECK(avformat_open_input(&format, file, nullptr, nullptr));

    // AVCALL_CHECK(avformat_find_stream_info(format, nullptr));

    // fmt::print("streams count: {}\n", format->nb_streams);

    // // find audio stream
    // int audio_stream_index{-1};
    // for (int i = 0; i < format->nb_streams; i++) {
    //     // codecpar stores decoder info
    //     if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
    //         audio_stream_index = i;
    //         break;
    //     }
    // }
    // if (audio_stream_index == -1) {
    //     fmt::print("there\'s no audio stream");
    //     return -1;
    // }

    // // 获取并打印音频流的参数
    // auto audio_stream = format->streams[audio_stream_index];
    // auto codec_params = audio_stream->codecpar;

    // fmt::print("\n--- Audio Stream Info ---\n");
    // fmt::print("Codec: {}\n", avcodec_get_name(codec_params->codec_id));
    // fmt::print("Sample Rate: {} Hz\n", codec_params->sample_rate);
    // fmt::print("Channels: {}\n", codec_params->ch_layout.nb_channels);

    // // 查找编解码器
    // auto codec = avcodec_find_decoder(codec_params->codec_id);
    // if (!codec) {
    //     fmt::print("no decoder find.\n");
    //     return -1;
    // }

    // // 分配编解码器
    // auto codec_context = avcodec_alloc_context3(codec);
    // if (!codec_context) {
    //     fmt::print("decoder_context_alloc failed.\n");
    //     return -1;
    // }

    // // 将流的参数拷贝到解码器上下文中。这是最关键的一步！
    // // 它告诉解码器要处理的数据的采样率、声道、格式等信息。
    // AVCALL_CHECK(avcodec_parameters_to_context(codec_context, codec_params));

    // // 打开解码器，准备开始工作
    // AVCALL_CHECK(avcodec_open2(codec_context, codec, nullptr));

    // // 分配内存并循环解码
    // // 用于存放从文件中读取的压缩数据包
    // auto packet = av_packet_alloc();
    // // 用于存放解码后的原始 PCM 数据帧
    // auto frame = av_frame_alloc();

    // std::vector<uint8_t> pcm_data_buffer;

    // // 循环读取文件中的 packet
    // while (av_read_frame(format, packet) >= 0) {
    //     // 确保这个 packet 是我们想处理的音频流的
    //     if (packet->stream_index == audio_stream_index) {
    //         // 1. 发送 packet 到解码器
    //         AVCALL_CHECK(avcodec_send_packet(codec_context, packet));

    //         // 2. 循环接收解码后的 frame
    //         // 一个 packet 可能解码出 0 个、1 个或多个 frame
    //         int ret = 1;
    //         while (ret >= 0) {
    //             ret = avcodec_receive_frame(codec_context, frame);
    //             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    //                 // EAGAIN: 需要更多 packet 才能输出一个 frame
    //                 // EOF: 解码器已经刷新完毕
    //                 break;
    //             } else if (ret < 0) {
    //                 // 解码出错
    //                 break;
    //             }

    //             // 成功得到一个解码后的 AVFrame
    //             // frame 中包含了原始的 PCM 数据

    //             // 获取这个 frame 的数据大小
    //             // get_bytes_per_sample
    //             // 根据采样格式(如S16,FLT)计算每个采样点占多少字节
    //             // 对于 planar格式，它计算所有平面的总大小
    //             int data_size =
    //                 av_get_bytes_per_sample((AVSampleFormat)frame->format);
    //             if (data_size < 0) {
    //                 // 无法计算大小
    //                 break;
    //             }

    //             // 如果是 planar 格式 (如 FLTP)，数据分存在 frame->data[0],
    //             // data[1]... 需要将它们交错合并成一个连续的 buffer
    //             for (int i = 0; i < frame->nb_samples; i++) {
    //                 for (int ch = 0; ch <
    //                 codec_context->ch_layout.nb_channels;
    //                      ch++) {
    //                     // 复制数据到 buffer
    //                     pcm_data_buffer.insert(
    //                         pcm_data_buffer.end(),
    //                         frame->data[ch] + data_size * i,
    //                         frame->data[ch] + data_size * (i + 1));
    //                 }
    //             }
    //             // 注意: 上面的复制方法适用于 planar 格式。
    //             // 如果是 packed 格式 (如 S16)，数据都在 frame->data[0] 里，
    //             // 可以直接用 frame->linesize[0] 获取整个 frame
    //             的数据大小，然后
    //             // memcpy。
    //         }
    //     }
    //     // 释放 packet 的引用，否则会内存泄漏
    //     av_packet_unref(packet);
    // }

    // // 释放
    // av_frame_free(&frame);
    // av_packet_free(&packet);
    // avcodec_free_context(&codec_context);
    // avformat_close_input(&format);
    ice::reset_allocation_counters();
    test();
    ice::print_allocation_stats();

    return 0;
}
