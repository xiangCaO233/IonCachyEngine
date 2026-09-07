#include <ice/manage/dec/CachyDecoder.hpp>
#include <iostream>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
}
/// @brief 未接入构建目标的历史示例入口占位，不是独立测试程序。
/// 当前没有解码行为，也没有提供可供调用方消费的返回值。
/// 可运行的用例入口在 main.cpp，实时变速验证位于 tests/。
/// FFmpeg 解码生命周期以 manage/dec/ffmpeg 的正式实现为准。
/// @warning 不应调用此占位入口或将它作为解码验证成功的依据。
int __main()
{
    // 旧的注释代码缺少有效输入及完整清理路径，不能用作可执行示例。
}
