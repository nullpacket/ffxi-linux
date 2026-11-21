// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/atrac3.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace xi::audio
{

bool decodeAtrac3(dat::SeWave& w, bool jointStereo)
{
    if (w.format != 3 || w.atrac3.empty())
    {
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3);
    if (!codec)
    {
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    av_channel_layout_default(&ctx->ch_layout, 2);
    ctx->sample_rate = static_cast<int>(w.sampleRate > 0 ? w.sampleRate : 44100);
    ctx->block_align = 384; // every frame is 384 bytes here (mono ones doubled)
    // WAV-style extradata (ffmpeg atrac3.c, 14 bytes): ?=1, samples per channel, coding mode x2, frame factor 1.
    std::array<uint8_t, 14> extra{ 1, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
    extra[6] = extra[8] = jointStereo ? 1 : 0;
    ctx->extradata      = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(ctx->extradata, extra.data(), extra.size());
    ctx->extradata_size = static_cast<int>(extra.size());
    bool ok             = avcodec_open2(ctx, codec, nullptr) == 0;
    AVPacket* pkt       = av_packet_alloc();
    AVFrame*  frame     = av_frame_alloc();
    const uint32_t outCh = w.channels; // mono: keep the left channel
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<std::size_t>(w.frames) * outCh);
    int errors = 0;
    for (std::size_t at = 0; ok && at + 384 <= w.atrac3.size(); at += 384)
    {
        if (av_new_packet(pkt, 384) < 0)
        {
            break;
        }
        std::memcpy(pkt->data, w.atrac3.data() + at, 384);
        if (avcodec_send_packet(ctx, pkt) < 0)
        {
            ++errors;
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0)
        {
            // atrac3 outputs planar float.
            const auto* l = reinterpret_cast<const float*>(frame->extended_data[0]);
            const auto* r = reinterpret_cast<const float*>(frame->extended_data[frame->ch_layout.nb_channels > 1 ? 1 : 0]);
            for (int i = 0; i < frame->nb_samples; ++i)
            {
                pcm.push_back(static_cast<int16_t>(std::clamp(std::lround(l[i] * 32767.0f), -32768L, 32767L)));
                if (outCh == 2)
                {
                    pcm.push_back(static_cast<int16_t>(std::clamp(std::lround(r[i] * 32767.0f), -32768L, 32767L)));
                }
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (!ok || pcm.empty() || errors > 2)
    {
        spdlog::debug("atrac3: id {} failed ({} errors)", w.id, errors);
        return false;
    }
    if (w.frames > 0 && pcm.size() > static_cast<std::size_t>(w.frames) * outCh)
    {
        pcm.resize(static_cast<std::size_t>(w.frames) * outCh);
    }
    w.pcm = std::move(pcm);
    return true;
}

} // namespace xi::audio
