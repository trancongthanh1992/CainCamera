//
// Created by CainHuang on 2020-01-09.
//

#include "NdkVideoEncoder.h"
#include <sys/system_properties.h>
#include <AVFormatter.h>

NdkVideoEncoder::NdkVideoEncoder(const std::shared_ptr<AVMediaMuxer> &mediaMuxer) : NdkMediaEncoder(mediaMuxer) {
    mWeakMuxer = mediaMuxer;
    mWidth = 0;
    mHeight = 0;
    mBitrate = VIDEO_BIT_RATE;
    mFrameRate = 0;
    mMimeType = VIDEO_MIME_AVC;

    // SDK 版本号
    char sdk[10] = {0};
    __system_property_get("ro.build.version.sdk",sdk);
    mSDKInt = atoi(sdk);

    // 设备型号
    char phoneType[20] = {0};
    __system_property_get("ro.product.model",phoneType);
    mPhoneType = (char*)malloc(20);
    memmove(mPhoneType, phoneType, 20);

    // 获取CPU型号
    char cpu[20] = {0};
    __system_property_get("ro.hardware",cpu);
    mCpu = (char*)malloc(20);
    memmove(mCpu, cpu, 20);

    LOGD("NdkVideoEncoder - current devices message: phone: %s, cpu:%s, sdk version: %d", mPhoneType, mCpu, mSDKInt);
}

NdkVideoEncoder::~NdkVideoEncoder() {
    release();
}

/**
 * 设置视频参数
 * @param width
 * @param height
 * @param bitrate
 * @param frameRate
 */
void NdkVideoEncoder::setVideoParams(int width, int height, int bitrate, int frameRate) {
    mWidth = width;
    mHeight = height;
    mBitrate = bitrate;
    mFrameRate = frameRate;
    if (mBitrate <= 0) {
        if (mWidth * mHeight >= 1280 * 720) {
            mBitrate = VIDEO_BIT_RATE;
        } else {
            mBitrate = VIDEO_BIT_RATE_LOW;
        }
    }
}

/**
 * 准备编码器
 * @return
 */
int NdkVideoEncoder::openEncoder() {
    // 创建编码器
    mMediaCodec = AMediaCodec_createEncoderByType(mMimeType);

    // 设置编码参数
    AMediaFormat *mediaFormat = AMediaFormat_new();
    AMediaFormat_setString(mediaFormat, AMEDIAFORMAT_KEY_MIME, mMimeType);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, mWidth);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, mHeight);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_BIT_RATE, mBitrate);
    AMediaFormat_setInt32(mediaFormat, "max-bitrate", mBitrate * 2);
    AMediaFormat_setInt32(mediaFormat, "bitrate-mode", BITRATE_MODE_VBR);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_FRAME_RATE, mFrameRate);
    if (mCpu[0] == 'm' && mCpu[1] == 't') {
        AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420Planar);
    } else {
        AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420SemiPlanar);
    }
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);  // GOP 1s
    int profile = 0;
    int level = 0;
    if (!strcmp(VIDEO_MIME_AVC, mMimeType)) {
        profile = AVCProfileHigh;
        level = AVCLevel31;
        if (mWidth * mHeight >= 1920 * 1080) {
            level = AVCLevel4;
        }
    } else if (!strcmp(VIDEO_MIME_HEVC, mMimeType)) {
        profile = HEVCProfileMain;
        level = HEVCHighTierLevel31;
        if (mWidth * mHeight >= 1920 * 1080) {
            level = HEVCHighTierLevel4;
        }
    }
    AMediaFormat_setInt32(mediaFormat, "profile", profile);
    AMediaFormat_setInt32(mediaFormat, "level", level);

    // 配置编码器
    media_status_t ret = AMediaCodec_configure(mMediaCodec, mediaFormat, nullptr, nullptr,
                                               AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    // 释放MediaFormat对象
    AMediaFormat_delete(mediaFormat);

    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder - AMediaCodec_configure error: %d", ret);
        return ret;
    }

    // 打开编码器
    ret = AMediaCodec_start(mMediaCodec);
    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder -AMediaCodec_start error: %d", ret);
        return ret;
    }

    // 刷新缓冲区
    ret = AMediaCodec_flush(mMediaCodec);
    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder -AMediaCodec_start error: %d", ret);
        return ret;
    }
    mFrameIndex = 0;
    mStartTimeStamp = 0;
    mPresentationTimeUs = 0;

    // 创建媒体流
    auto mediaMuxer = mWeakMuxer.lock();
    AVCodecID codecID = AV_CODEC_ID_NONE;
    if (!strcmp(VIDEO_MIME_AVC, mMimeType)) {
        codecID = AV_CODEC_ID_H264;
    } else if (!strcmp(VIDEO_MIME_HEVC, mMimeType)) {
        codecID = AV_CODEC_ID_HEVC;
    }
    if (codecID == AV_CODEC_ID_NONE) {
        return 0;
    }
    pStream = mediaMuxer->createStream(codecID);
    if (pStream != nullptr) {
        mStreamIndex = pStream->index;
        pStream->time_base = (AVRational){1, mFrameRate};
        pStream->codecpar->width = mWidth;
        pStream->codecpar->height = mHeight;
        pStream->codecpar->bit_rate = mBitrate;
        pStream->codecpar->level = level;
        pStream->codecpar->profile = profile;
        pStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        pStream->codecpar->codec_id = codecID;
        pStream->codecpar->codec_tag = 0;
    }

    return AMEDIA_OK;
}

/**
 * 关闭编码器
 * @return
 */
int NdkVideoEncoder::closeEncoder() {
    if (!mMediaCodec) {
        return 0;
    }
    int ret;
    ret = AMediaCodec_flush(mMediaCodec);
    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder - AMediaCodec_flush error: %d", ret);
        return ret;
    }

    ret = AMediaCodec_stop(mMediaCodec);
    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder - AMediaCodec_stop", ret);
        return ret;
    }

    ret = AMediaCodec_delete(mMediaCodec);
    if (ret != AMEDIA_OK) {
        LOGE("NdkVideoEncoder - AMediaCodec_delete error: %d", ret);
        return ret;
    }
    mMediaCodec = nullptr;
    return 0;
}

/**
 * 释放所有资源
 */
void NdkVideoEncoder::release() {
    NdkMediaEncoder::release();
    if (mMediaCodec != nullptr) {
        AMediaCodec_stop(mMediaCodec);
        AMediaCodec_delete(mMediaCodec);
        mMediaCodec = nullptr;
    }
    if (mPhoneType) {
        free(mPhoneType);
        mPhoneType = nullptr;
    }
    if (mCpu) {
        free(mCpu);
        mCpu = nullptr;
    }
}

/**
 * 编码媒体数据
 * @param mediaData
 * @param gotFrame
 * @return
 */
int NdkVideoEncoder::encodeMediaData(AVMediaData *mediaData, int *gotFrame) {
    int ret, gotFrameLocal;
    if (!gotFrame) {
        gotFrame = &gotFrameLocal;
    }
    *gotFrame = 0;

    // 将媒体数据送去编码
    ret = sendFrame(mediaData);
    if (ret != 0) {
        return ret;
    }

    // 初始化一个数据包
    AVPacket packet;
    av_init_packet(&packet);
    // 接收编码后的数据包
    ret = receiveEncodePacket(&packet, gotFrame);
    if (ret >= 0 && *gotFrame == 1) {
        writePacket(&packet);
    }
    av_packet_unref(&packet);
    return 0;
}

/**
 * 编码媒体数据
 * @param frame
 * @param gotFame
 * @return
 */
int NdkVideoEncoder::encodeFrame(AVFrame *frame, int *gotFame) {
    return 0;
}

/**
 * 计算时间戳
 */
uint64_t NdkVideoEncoder::calculatePresentationTime() {
    mPresentationTimeUs = (uint64_t)(mFrameIndex * 1000000L / mFrameRate + 132L);
    mFrameIndex++;
    return mPresentationTimeUs;
}

/**
 * 计算编码时长
 * @param bufferInfo
 */
void NdkVideoEncoder::calculateDuration(AMediaCodecBufferInfo bufferInfo) {
    if (mStartTimeStamp == 0) {
        mStartTimeStamp = bufferInfo.presentationTimeUs;
        mDuration = 0;
    } else {
        mDuration = bufferInfo.presentationTimeUs - mStartTimeStamp;
    }
}

/**
 * 将媒体数据送去编码
 * @param mediaData
 * @return
 */
int NdkVideoEncoder::sendFrame(AVMediaData *mediaData) {
    if (mediaData->getType() != MediaVideo || mediaData->image == nullptr || mediaData->length <= 0) {
        return -1;
    }

    ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(mMediaCodec, -1);
    size_t bufSize = 0;
    if (inputIndex >= 0) {
        uint8_t *buffer = AMediaCodec_getInputBuffer(mMediaCodec, inputIndex, &bufSize);
        if (buffer != nullptr && bufSize >= mediaData->length) {

        } else {
            memset(buffer, 0, bufSize);
        }
        if (mediaData->length <= 0) {
            AMediaCodec_queueInputBuffer(mMediaCodec, inputIndex, 0, 0, (uint64_t)mPresentationTimeUs, 0);
        } else {
            // MTK cpu 采用 yuv420p编码，其他cpu采用yuv420sp编码
            if (mCpu[0] == 'm' && mCpu[1] == 't') {
                // 如果输入格式不为yuv420p，则需要转换成yuv420p
                if (mediaData->pixelFormat != PIXEL_FORMAT_YUV420P) {
                    auto yuvData = convertToYuvData(mediaData);
                    mediaData->free();
                    fillVideoData(mediaData, yuvData, yuvData->width, yuvData->height);
                    delete yuvData;
                }
                // 将数据复制到输入缓冲区中
                memmove(buffer, mediaData->image, mediaData->length);
            } else {
                if (mediaData->pixelFormat == PIXEL_FORMAT_YUV420P) {
                    I420toYUV420SemiPlanar(mediaData->image, 0, buffer, mediaData->width, mediaData->height);
                } else if (mediaData->pixelFormat == PIXEL_FORMAT_NV12) {
                    memmove(buffer, mediaData->image, mediaData->length);
                } else if (mediaData->pixelFormat == PIXEL_FORMAT_NV21) {
                    NV12toNV21(mediaData->image, 0, buffer, mediaData->width, mediaData->height);
                } else {
                    // 先转成yuv420p
                    auto yuvData = convertToYuvData(mediaData);
                    mediaData->free();
                    fillVideoData(mediaData, yuvData, yuvData->width, yuvData->height);
                    delete yuvData;
                    // yuv420p转成yuv420sp
                    I420toYUV420SemiPlanar(mediaData->image, 0, buffer, mediaData->width, mediaData->height);
                }
            }

            // 获得当前时间戳
            if (mediaData->pts > 0) {
                mPresentationTimeUs = (uint64_t)(mediaData->pts * 1000);
            } else {
                calculatePresentationTime();
            }
            AMediaCodec_queueInputBuffer(mMediaCodec, (size_t)inputIndex, 0, (size_t)mediaData->length, (uint64_t)mPresentationTimeUs, 0);
            LOGD("NdkVideoEncoder - encode yuv data: presentationTimeUs: %ld, s: %f, media data pts: %ld", mPresentationTimeUs, (mPresentationTimeUs / 1000000.0), mediaData->pts);
        }
    }
    return 0;
}

/**
 * 接收编码后的数据包
 * @param packet
 * @param gotFrame
 * @return
 */
int NdkVideoEncoder::receiveEncodePacket(AVPacket *packet, int *gotFrame) {
    int gotFrameLocal;
    if (!gotFrame) {
        gotFrame = &gotFrameLocal;
    }
    *gotFrame = 0;

    AMediaCodecBufferInfo bufferInfo;
    ssize_t encodeStatus = AMediaCodec_dequeueOutputBuffer(mMediaCodec, &bufferInfo, VIDEO_ENCODE_TIMEOUT);
    size_t outputSize = 0;
    LOGI("outputIndex : %d", encodeStatus);
    if (encodeStatus != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        if (encodeStatus <= -20000) {
            LOGE("AMEDIA_DRM_ERROR_BASE");
            return -1;
        }
        if (encodeStatus <= -10000) {
            LOGE("AMEDIA_ERROR_BASE");
            return -1;
        }

        if (encodeStatus == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // outputBuffers = codec.getOutputBuffers();
        } else if (encodeStatus == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            LOGD("AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED");
        } else if (encodeStatus >= 0) {
            uint8_t* encodeData = AMediaCodec_getOutputBuffer(mMediaCodec, encodeStatus, &outputSize);
            if (encodeData) {
                // 将编码数据写入复用器中
                if ((bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    LOGD("ignoring BUFFER_FLAG_CODEC_CONFIG");
                    // 创建并配置FFmpeg媒体流信息
                    if (pStream != nullptr) {
                        if (bufferInfo.size > 0) {
                            pStream->codecpar->extradata = (uint8_t *) av_mallocz(
                                    (size_t) (bufferInfo.size + FF_INPUT_BUFFER_PADDING_SIZE));
                            memcpy(pStream->codecpar->extradata, encodeData,
                                   (size_t) bufferInfo.size);
                            pStream->codecpar->extradata_size = bufferInfo.size;
                        }
                    }
                } else {
                    // 转成AVPacket并写入文件
                    calculateDuration(bufferInfo);
                    int64_t pts = bufferInfo.presentationTimeUs >= mPresentationTimeUs ? bufferInfo.presentationTimeUs : mPresentationTimeUs;

                    // 设置数据包参数
                    if (packet != nullptr) {
                        packet->stream_index = mStreamIndex;
                        packet->data = encodeData;
                        packet->size = bufferInfo.size;
                        packet->duration = 0;
                        packet->pts = rescalePts(pts, (AVRational) {1, mFrameRate});
                        packet->dts = packet->pts;
                        packet->pos = -1;

                        // 计算编码后的pts
                        av_packet_rescale_ts(packet, (AVRational) {1, mFrameRate},
                                             pStream->time_base);

                        LOGD("NdkVideoEncoder - encode h264 data: %ld, buffer.presentationUs: %ld",
                             packet->pts, bufferInfo.presentationTimeUs);

                        // 是否关键帧
                        if (bufferInfo.flags == BUFFER_FLAG_KEY_FRAME) {
                            packet->flags |= AV_PKT_FLAG_KEY;
                        }
                    }
                    *gotFrame = 1;
                }
            }
            // 释放编码缓冲区
            AMediaCodec_releaseOutputBuffer(mMediaCodec, encodeStatus, false);
        }
    }
    return 0;
}

AVMediaType NdkVideoEncoder::getMediaType() {
    return AVMEDIA_TYPE_VIDEO;
}