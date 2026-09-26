#include "decode/VideoDecoder.hpp"

#include "common/Logger.hpp"
#include "decode/FfmpegDecoder.hpp"
#include "decode/MppDecoder.hpp"

std::unique_ptr<VideoDecoder> createDecoder(const std::string &backend, const StreamParams &params, int mpp_split_parse)
{
    if (backend == "mpp" || backend == "auto")
    {
#ifdef HAVE_MPP
        if (params.codec != CodecType::Other)
        {
            std::unique_ptr<VideoDecoder> dec(new MppDecoder(mpp_split_parse));
            if (dec->init(params) == 0)
                return dec;
            LOGW("mpp decoder init failed");
        }
        else
        {
            LOGW("codec not supported by mpp path");
        }
#else
        (void)mpp_split_parse;
        LOGW("built without MPP support");
#endif
        if (backend == "mpp")
            return nullptr;
        LOGW("fallback to ffmpeg software decoder");
    }
    std::unique_ptr<VideoDecoder> dec(new FfmpegDecoder());
    if (dec->init(params) == 0)
        return dec;
    return nullptr;
}
