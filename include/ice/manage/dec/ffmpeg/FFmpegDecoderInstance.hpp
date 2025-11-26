#ifndef ICE_FFMPEGDECODERINSTANCE_HPP
#define ICE_FFMPEGDECODERINSTANCE_HPP

#include <ice/manage/dec/IDecoderInstance.hpp>
#include <memory>

#include "ice/manage/AudioFormat.hpp"

namespace ice
{
class FFmpegDecoder;
class FFmpegDecoderInstance : public IDecoderInstance
{
public:
    // 构造需要对应文件路径
    explicit FFmpegDecoderInstance(std::string_view            file_path,
                                   const ice::AudioDataFormat& target_format);

    // 需要声明析构函数-隐藏ffmpeg实现细节
    ~FFmpegDecoderInstance() override;

    // 定位帧位置
    bool seek(size_t pos) override;
    // 读取一块数据
    size_t read(float** buffer, size_t chunksize) override;

    const AudioDataFormat& get_source_format() const override;
    size_t                 get_source_total_frames() const override;

private:
    std::unique_ptr<FFmpegDecoder> ffimpl;
};
}  // namespace ice

#endif  // ICE_FFMPEGDECODERINSTANCE_HPP
