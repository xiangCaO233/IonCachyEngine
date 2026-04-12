#include <cmath>
#include <cstring>
#include <ice/core/effect/GraphicEqualizer.hpp>

namespace ice
{
GraphicEqualizer::GraphicEqualizer(
    const std::vector<double>& center_frequencies)
{
    for ( double freq : center_frequencies ) {
        bands.emplace_back(freq);
    }
}

// 接收浮点倍率，转换为dB
void GraphicEqualizer::set_band_gain_ratio(size_t band_index, float ratio)
{
    // 防止 ratio <= 0 的情况
    if ( band_index < bands.size() && ratio > 0.0001f ) {
        // 增益(dB) = 20 * log10(倍率)
        bands[band_index].gain_db = 20.0 * std::log10(ratio);
        needs_update              = true;
    }
}

void GraphicEqualizer::set_band_gain_db(size_t band_index, float db)
{
    if ( band_index < bands.size() ) {
        bands[band_index].gain_db = db;
        needs_update              = true;
    }
}

// 这是核心处理函数
void GraphicEqualizer::apply_effect(AudioBuffer&       output,
                                    const AudioBuffer& input)
{
    // 确保输入和输出缓冲区是兼容的
    if ( output.num_frames() != input.num_frames() ||
         output.num_channels() != input.num_channels() ) {
        // 直接返回
        return;
    }

    // 将输入拷贝到输出
    for ( uint16_t ch = 0; ch < input.num_channels(); ++ch ) {
        std::memcpy(output.raw_ptrs()[ch],
                    input.raw_ptrs()[ch],
                    input.num_frames() * sizeof(float));
    }

    const auto& fmt = output.afmt;

    // 检查是否需要更新滤波器
    if ( needs_update || fmt.samplerate != sample_rate ||
         fmt.channels != filter_chains.size() ) {
        sample_rate = fmt.samplerate;
        // 为每个声道创建一套滤波器链
        filter_chains.resize(fmt.channels);
        for ( auto& chain : filter_chains ) {
            chain.resize(bands.size());
        }
        update_filters();
        needs_update = false;
    }

    // 对每个声道，依次通过滤波器链进行处理
    for ( uint16_t ch = 0; ch < fmt.channels; ++ch ) {
        float* channel_data = output.raw_ptrs()[ch];
        for ( auto& filter : filter_chains[ch] ) {
            filter.process(channel_data, output.num_frames());
        }
    }
}

void GraphicEqualizer::update_filters()
{
    for ( uint16_t ch = 0; ch < filter_chains.size(); ++ch ) {
        for ( size_t i = 0; i < bands.size(); ++i ) {
            const auto& band = bands[i];
            filter_chains[ch][i].set_peaking(
                sample_rate, band.center_freq_hz, band.q_factor, band.gain_db);
        }
    }
}

double GraphicEqualizer::get_total_magnitude_response(double frequency) const
{
    if ( filter_chains.empty() || filter_chains[0].empty() ) return 1.0;

    double total_mag = 1.0;
    // 使用第一个声道的滤波器链来计算响应 (假设全声道 EQ 一致)
    for ( const auto& filter : filter_chains[0] ) {
        total_mag *= filter.get_magnitude_response(frequency, sample_rate);
    }
    return total_mag;
}

}  // namespace ice
