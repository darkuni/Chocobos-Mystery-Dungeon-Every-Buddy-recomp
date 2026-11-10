/* libSceVideodec2: H.264 (and HEVC) access units decoded by FFmpeg into the game's NV12 frame
 * buffers, for CRIWARE's Sofdec movies (cri_ware_unity.prx). Structures, sizes, error codes and the
 * memory estimate follow shadPS4's videodec2 (GPL-2.0-or-later). The picture info the game reads
 * sits right after the NV12 picture in its frame buffer, as on the console. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define ERR_API_FAIL ((int32_t)0x811D0100)
#define ERR_STRUCT_SIZE ((int32_t)0x811D0101)
#define ERR_ARGUMENT_POINTER ((int32_t)0x811D0102)
#define ERR_DECODER_INSTANCE ((int32_t)0x811D0103)
#define ERR_FRAME_BUFFER_SIZE ((int32_t)0x811D0106)
#define ERR_ACCESS_UNIT_SIZE ((int32_t)0x811D010D)
#define ERR_ACCESS_UNIT_POINTER ((int32_t)0x811D010E)
#define ERR_CODEC_TYPE ((int32_t)0x811D0204)
#define CODEC_AVC 1u
#define CODEC_HEVC 974921u
#define MINIMUM_MEMORY (16u << 20)
#define AVC_INFO_SIZE 0x78
#define HEVC_INFO_SIZE 0xB8

typedef struct {
    uint64_t this_size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_frame_width, max_frame_height, max_dpb_frame_count;
    uint32_t decode_pipeline_depth;
    void *compute_queue;
    uint64_t cpu_affinity_mask;
    int32_t cpu_thread_priority;
    uint8_t optimize_progressive_video, check_memory_type, reserved0, reserved1;
    void *extra_config_info;
} ConfigInfo;
typedef struct {
    uint64_t this_size, cpu_memory_size; void *cpu_memory;
    uint64_t gpu_memory_size; void *gpu_memory;
    uint64_t cpu_gpu_memory_size; void *cpu_gpu_memory;
    uint64_t max_frame_buffer_size;
    uint32_t frame_buffer_alignment, reserved0;
} MemoryInfo;
typedef struct { uint64_t this_size; void *au_data; uint64_t au_size, pts, dts, attached; } InputData;
typedef struct {
    uint64_t this_size;
    uint8_t is_valid, is_error_frame, picture_count;
    uint32_t codec_type, frame_width, frame_pitch, frame_height;
    void *frame_buffer;
    uint64_t frame_buffer_size;
    uint32_t frame_format, frame_pitch_in_bytes;
} OutputInfo;
typedef struct { uint64_t this_size; void *frame_buffer; uint64_t frame_buffer_size; uint8_t is_accepted; } FrameBuffer;
typedef struct { uint64_t this_size, cpu_gpu_memory_size; void *cpu_gpu_memory; } ComputeMemoryInfo;
typedef struct { uint64_t this_size; uint16_t pipe, queue; uint8_t check_memory_type, reserved0; uint16_t reserved1; } ComputeConfigInfo;
_Static_assert(sizeof(ConfigInfo) == 0x48 && sizeof(MemoryInfo) == 0x48 && sizeof(InputData) == 0x30 &&
               sizeof(OutputInfo) == 0x38 && sizeof(FrameBuffer) == 0x20 && sizeof(ComputeMemoryInfo) == 0x18 &&
               sizeof(ComputeConfigInfo) == 0x10, "Videodec2 layouts");

typedef struct Decoder {
    AVCodecContext *codec;
    struct SwsContext *sws;
    AVFrame *queue[16];
    size_t head, count;
    uint32_t codec_type;
} Decoder;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static ABI int32_t query_compute(ComputeMemoryInfo *info) {
    if (!info) return ERR_ARGUMENT_POINTER;
    if (info->this_size != sizeof(*info)) return ERR_STRUCT_SIZE;
    info->cpu_gpu_memory = NULL;
    info->cpu_gpu_memory_size = MINIMUM_MEMORY;
    return 0;
}
static ABI int32_t allocate_compute(const ComputeConfigInfo *config, const ComputeMemoryInfo *memory, void **queue) {
    if (!config || !memory || !queue) return ERR_ARGUMENT_POINTER;
    if (config->this_size != sizeof(*config) || memory->this_size != sizeof(*memory)) return ERR_STRUCT_SIZE;
    *queue = memory->cpu_gpu_memory; /* the console hands out a pointer into this memory too */
    return 0;
}
static ABI int32_t release_compute(void *queue) { (void)queue; return 0; }

static ABI int32_t query_decoder(const ConfigInfo *config, MemoryInfo *memory) {
    if (!config || !memory) return ERR_ARGUMENT_POINTER;
    if (config->this_size != sizeof(*config) || memory->this_size != sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->codec_type != CODEC_AVC && config->codec_type != CODEC_HEVC) return ERR_CODEC_TYPE;
    int32_t w = config->max_frame_width, h = config->max_frame_height;
    if (w <= 0 || h <= 0) { w = config->max_level >= 150 ? 3840 : 1920; h = config->max_level >= 150 ? 2160 : 1080; }
    uint64_t frame = (uint64_t)align_up((uint32_t)w, 64) * align_up((uint32_t)h, 16) * 3 / 2;
    memory->cpu_memory = memory->gpu_memory = memory->cpu_gpu_memory = NULL;
    memory->cpu_memory_size = memory->gpu_memory_size = memory->cpu_gpu_memory_size = MINIMUM_MEMORY;
    memory->max_frame_buffer_size = ((frame + 255) & ~(uint64_t)255) + 0x4000;
    memory->frame_buffer_alignment = 0x100;
    return 0;
}
static ABI int32_t create_decoder(const ConfigInfo *config, const MemoryInfo *memory, Decoder **out) {
    if (!config || !memory || !out) return ERR_ARGUMENT_POINTER;
    if (config->this_size != sizeof(*config) || memory->this_size != sizeof(*memory)) return ERR_STRUCT_SIZE;
    if (config->codec_type != CODEC_AVC && config->codec_type != CODEC_HEVC) return ERR_CODEC_TYPE;
    const AVCodec *codec = avcodec_find_decoder(config->codec_type == CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    Decoder *d = calloc(1, sizeof(*d));
    if (!codec || !d) { free(d); return ERR_API_FAIL; }
    d->codec_type = config->codec_type;
    d->codec = avcodec_alloc_context3(codec);
    if (!d->codec) { free(d); return ERR_API_FAIL; }
    d->codec->width = config->max_frame_width;
    d->codec->height = config->max_frame_height;
    d->codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    if (avcodec_open2(d->codec, codec, NULL) < 0) { avcodec_free_context(&d->codec); free(d); return ERR_API_FAIL; }
    printf("Runtime: Videodec2 decoder %s up to %dx%d\n", config->codec_type == CODEC_HEVC ? "HEVC" : "H.264",
           config->max_frame_width, config->max_frame_height);
    *out = d;
    return 0;
}
static void clear_queue(Decoder *d) {
    while (d->count) { av_frame_free(&d->queue[d->head]); d->head = (d->head + 1) % 16; --d->count; }
    d->head = 0;
}
static ABI int32_t delete_decoder(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    clear_queue(d);
    if (d->sws) sws_freeContext(d->sws);
    avcodec_free_context(&d->codec);
    free(d);
    return 0;
}
static void drain(Decoder *d) {
    while (d->count < 16) {
        AVFrame *f = av_frame_alloc();
        if (!f) return;
        if (avcodec_receive_frame(d->codec, f)) { av_frame_free(&f); return; }
        d->queue[(d->head + d->count) % 16] = f;
        ++d->count;
    }
}
/* One queued picture into the game's buffer: NV12, pitch 64-aligned, height 16-aligned, picture
 * info right after it. */
static int32_t output(Decoder *d, FrameBuffer *buffer, OutputInfo *info) {
    if (!d->count) return 0;
    AVFrame *f = d->queue[d->head];
    d->head = (d->head + 1) % 16; --d->count;
    AVFrame *nv12 = f;
    if (f->format != AV_PIX_FMT_NV12) {
        nv12 = av_frame_alloc();
        if (!nv12) { av_frame_free(&f); return ERR_API_FAIL; }
        nv12->format = AV_PIX_FMT_NV12; nv12->width = f->width; nv12->height = f->height;
        if (av_frame_get_buffer(nv12, 0)) { av_frame_free(&nv12); av_frame_free(&f); return ERR_API_FAIL; }
        d->sws = sws_getCachedContext(d->sws, f->width, f->height, (enum AVPixelFormat)f->format, f->width, f->height, AV_PIX_FMT_NV12,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!d->sws) { av_frame_free(&nv12); av_frame_free(&f); return ERR_API_FAIL; }
        sws_scale(d->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, nv12->data, nv12->linesize);
    }
    uint32_t width = (uint32_t)f->width, height = (uint32_t)f->height;
    uint32_t pitch = align_up(width, 64), aligned_h = align_up(height, 16);
    uint64_t picture = (uint64_t)pitch * aligned_h * 3 / 2;
    uint64_t info_size = d->codec_type == CODEC_HEVC ? HEVC_INFO_SIZE : AVC_INFO_SIZE;
    int32_t r = 0;
    if (!buffer->frame_buffer || picture + info_size > buffer->frame_buffer_size) r = ERR_FRAME_BUFFER_SIZE;
    else {
        uint8_t *luma = buffer->frame_buffer, *chroma = luma + (uint64_t)pitch * aligned_h;
        for (uint32_t y = 0; y < aligned_h; ++y) {
            uint32_t sy = y < height ? y : height - 1;
            memcpy(luma + (uint64_t)y * pitch, nv12->data[0] + (uint64_t)sy * nv12->linesize[0], width);
        }
        for (uint32_t y = 0; y < aligned_h / 2; ++y) {
            uint32_t sy = y < height / 2 ? y : height / 2 - 1;
            memcpy(chroma + (uint64_t)y * pitch, nv12->data[1] + (uint64_t)sy * nv12->linesize[1], width);
        }
        buffer->is_accepted = 1;
        info->is_valid = 1; info->is_error_frame = 0; info->picture_count = 1;
        info->codec_type = d->codec_type;
        info->frame_width = align_up(width, 16); info->frame_pitch = pitch; info->frame_height = aligned_h;
        info->frame_buffer = buffer->frame_buffer;
        info->frame_buffer_size = picture;
        if (info->this_size == sizeof(OutputInfo)) { info->frame_format = 0; info->frame_pitch_in_bytes = pitch; }
        uint8_t *pi = luma + picture;
        memset(pi, 0, info_size);
        uint64_t v = info_size; memcpy(pi, &v, 8);
        pi[8] = 1;                                               /* is_valid */
        int64_t pts = f->pts, dts = f->pkt_dts; uint64_t attached = (uint64_t)(uintptr_t)f->opaque;
        memcpy(pi + 16, &pts, 8); memcpy(pi + 24, &dts, 8); memcpy(pi + 32, &attached, 8);
        if (d->codec_type == CODEC_AVC) {
            /* frame_cropping_flag and offsets: the picture is padded to the pitch/height above. */
            uint32_t right = pitch - width, bottom = aligned_h - height;
            pi[53] = (right || bottom) ? 1 : 0;
            memcpy(pi + 60, &right, 4); memcpy(pi + 68, &bottom, 4);
        }
    }
    if (nv12 != f) av_frame_free(&nv12);
    av_frame_free(&f);
    return r;
}
static ABI int32_t decode(Decoder *d, const InputData *input, FrameBuffer *buffer, OutputInfo *info) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!input || !buffer || !info) return ERR_ARGUMENT_POINTER;
    if (input->this_size != sizeof(*input) || buffer->this_size != sizeof(*buffer)) return ERR_STRUCT_SIZE;
    buffer->is_accepted = 0; info->is_valid = 0; info->picture_count = 0;
    if (!input->au_data) return ERR_ACCESS_UNIT_POINTER;
    if (!input->au_size) return ERR_ACCESS_UNIT_SIZE;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return ERR_API_FAIL;
    packet->data = input->au_data;
    packet->size = (int)input->au_size;
    packet->pts = (int64_t)input->pts;
    packet->dts = (int64_t)input->dts;
    packet->opaque = (void *)(uintptr_t)input->attached;
    int ret = avcodec_send_packet(d->codec, packet);
    if (ret == AVERROR_EOF) { avcodec_flush_buffers(d->codec); ret = avcodec_send_packet(d->codec, packet); }
    if (ret == AVERROR(EAGAIN)) { drain(d); ret = avcodec_send_packet(d->codec, packet); }
    av_packet_free(&packet);
    if (ret < 0) return ERR_API_FAIL;
    drain(d);
    return output(d, buffer, info);
}
static ABI int32_t flush(Decoder *d, FrameBuffer *buffer, OutputInfo *info) {
    if (!d) return ERR_DECODER_INSTANCE;
    if (!buffer || !info) return ERR_ARGUMENT_POINTER;
    if (buffer->this_size != sizeof(*buffer) || (info->this_size | 8) != sizeof(OutputInfo)) return ERR_STRUCT_SIZE;
    buffer->is_accepted = 0; info->is_valid = 0; info->picture_count = 0;
    if (!d->count) { avcodec_send_packet(d->codec, NULL); drain(d); }
    return output(d, buffer, info);
}
static ABI int32_t reset(Decoder *d) {
    if (!d) return ERR_DECODER_INSTANCE;
    clear_queue(d);
    avcodec_flush_buffers(d->codec);
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceVideodec2QueryComputeMemoryInfo", query_compute}, {"sceVideodec2AllocateComputeQueue", allocate_compute},
    {"sceVideodec2ReleaseComputeQueue", release_compute}, {"sceVideodec2QueryDecoderMemoryInfo", query_decoder},
    {"sceVideodec2CreateDecoder", create_decoder}, {"sceVideodec2DeleteDecoder", delete_decoder},
    {"sceVideodec2Decode", decode}, {"sceVideodec2Flush", flush}, {"sceVideodec2Reset", reset},
};
uintptr_t runtime_videodec2_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
