/*
 * terminal_video_player.c
 *
 * 使用 FFmpeg 解码音视频，在终端以 ASCII 字符画播放视频，
 * 可选通过 SDL2 跨平台播放音频（Windows / macOS / Linux）。
 *
 * 依赖：libavformat, libavcodec, libswscale, libswresample, libavutil, SDL2
 * 兼容：FFmpeg 4.x / 5.x / 6.x / 7.x / 8.x（自动适配新旧 channel layout API）
 *
 * 编译（Linux / macOS）：
 *   gcc -O2 terminal_video_player.c -o terminal_video_player \
 *       $(pkg-config --cflags --libs libavformat libavcodec libswscale \
 *                     libswresample libavutil sdl2)
 *
 * 编译（Windows / MSYS2-MinGW64）：
 *   gcc -O2 terminal_video_player.c -o terminal_video_player.exe \
 *       $(pkg-config --cflags --libs libavformat libavcodec libswscale \
 *                     libswresample libavutil sdl2)
 *
 * 用法：
 *   ./terminal_video_player [选项] <视频文件> [输出宽度]
 *
 * 选项：
 *   -a, --audio    启用音频播放（默认不播放音频）
 *   -h, --help     显示帮助信息
 *
 * 示例：
 *   ./terminal_video_player test.mp4            # 仅视频
 *   ./terminal_video_player -a test.mp4 120     # 音视频同播，宽度120
 */

/* ================================================================== */
/*  平台检测与头文件                                                    */
/* ================================================================== */

#if defined(_WIN32) || defined(_WIN64)
#  define PLATFORM_WINDOWS 1
/* 避免 SDL 把 main 重命名为 SDL_main */
#  define SDL_MAIN_HANDLED
#elif defined(__APPLE__)
#  define PLATFORM_MACOS 1
#else
#  define PLATFORM_LINUX 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>

#include <SDL.h>

/* ================================================================== */
/*  FFmpeg API 版本兼容检测                                             */
/*  swr_alloc_set_opts2 在 FFmpeg 5.1 (libswresample 4.10.0) 引入，  */
/*  swr_alloc_set_opts  在 FFmpeg 7.0 (libswresample 5.0) 移除。      */
/*  注意：不能用 #ifdef 检测函数（函数不是预处理宏），必须用版本号。    */
/* ================================================================== */

#include <libswresample/version.h>

#if LIBSWRESAMPLE_VERSION_MAJOR > 4 || \
    (LIBSWRESAMPLE_VERSION_MAJOR == 4 && LIBSWRESAMPLE_VERSION_MINOR >= 10)
#  define HAVE_SWR_ALLOC_SET_OPTS2 1
#else
#  define HAVE_SWR_ALLOC_SET_OPTS2 0
#endif

/* ================================================================== */
/*  常量定义                                                            */
/* ================================================================== */

/* 音频输出参数（统一重采样为此格式，跨平台兼容性最好） */
#define AUDIO_SAMPLE_RATE    44100
#define AUDIO_CHANNELS       2
#define AUDIO_FORMAT         AUDIO_S16SYS   /* 16-bit 有符号，本机字节序 */
#define AUDIO_BYTES_PER_SAMPLE  2           /* S16 = 2 bytes */
#define AUDIO_BYTES_PER_FRAME   (AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_BYTES_PER_SEC     (AUDIO_SAMPLE_RATE * AUDIO_BYTES_PER_FRAME)

/* SDL 音频缓冲区大小（样本数），约 23ms */
#define SDL_AUDIO_SAMPLES    1024

/* 环形缓冲区大小（字节，必须是 2 的幂），约 1.5 秒音频 */
#define RING_BUFFER_CAPACITY (1 << 18)  /* 262144 bytes */

/* A/V 同步阈值（秒） */
#define AV_SYNC_THRESHOLD    0.05   /* 视频比音频快超过此值则等待 */
#define AV_DROP_THRESHOLD    0.10   /* 视频比音频慢超过此值则丢帧 */

/* ================================================================== */
/*  环形缓冲区（线程安全，单生产者单消费者）                            */
/* ================================================================== */

typedef struct {
    uint8_t  *data;
    size_t    mask;       /* capacity - 1，capacity 为 2 的幂 */
    size_t    read_pos;
    size_t    write_pos;
    SDL_mutex *mutex;
} RingBuffer;

static int rb_init(RingBuffer *rb, size_t capacity)
{
    /* capacity 必须是 2 的幂 */
    if ((capacity & (capacity - 1)) != 0) return -1;
    rb->data = (uint8_t *)malloc(capacity);
    if (!rb->data) return -1;
    rb->mask      = capacity - 1;
    rb->read_pos  = 0;
    rb->write_pos = 0;
    rb->mutex     = SDL_CreateMutex();
    if (!rb->mutex) { free(rb->data); rb->data = NULL; return -1; }
    return 0;
}

static void rb_destroy(RingBuffer *rb)
{
    if (rb->mutex) SDL_DestroyMutex(rb->mutex);
    free(rb->data);
    rb->data = NULL;
    rb->mutex = NULL;
}

static size_t rb_write(RingBuffer *rb, const uint8_t *src, size_t len)
{
    SDL_LockMutex(rb->mutex);
    size_t used = (rb->write_pos - rb->read_pos) & rb->mask;
    size_t free_space = rb->mask - used;  /* 留 1 字节区分满/空 */
    size_t n = len < free_space ? len : free_space;
    if (n > 0) {
        size_t first = (rb->mask + 1) - rb->write_pos;  /* capacity - write_pos */
        if (first > n) first = n;
        memcpy(rb->data + rb->write_pos, src, first);
        if (n > first) memcpy(rb->data, src + first, n - first);
        rb->write_pos = (rb->write_pos + n) & rb->mask;
    }
    SDL_UnlockMutex(rb->mutex);
    return n;
}

static size_t rb_read(RingBuffer *rb, uint8_t *dst, size_t len)
{
    SDL_LockMutex(rb->mutex);
    size_t used = (rb->write_pos - rb->read_pos) & rb->mask;
    size_t n = len < used ? len : used;
    if (n > 0) {
        size_t first = (rb->mask + 1) - rb->read_pos;
        if (first > n) first = n;
        memcpy(dst, rb->data + rb->read_pos, first);
        if (n > first) memcpy(dst + first, rb->data, n - first);
        rb->read_pos = (rb->read_pos + n) & rb->mask;
    }
    SDL_UnlockMutex(rb->mutex);
    return n;
}

static size_t rb_available(RingBuffer *rb)
{
    SDL_LockMutex(rb->mutex);
    size_t used = (rb->write_pos - rb->read_pos) & rb->mask;
    SDL_UnlockMutex(rb->mutex);
    return used;
}

/* ================================================================== */
/*  全局音频状态（供 SDL 回调访问）                                     */
/* ================================================================== */

static RingBuffer      g_audio_rb;
static SDL_atomic_t    g_audio_consumed_bytes;  /* 已消费的 PCM 字节总数 */
static volatile int    g_audio_running = 0;

/* 获取当前音频播放时钟（秒） */
static double get_audio_clock(void)
{
    return (double)SDL_AtomicGet(&g_audio_consumed_bytes) / (double)AUDIO_BYTES_PER_SEC;
}

/* ================================================================== */
/*  SDL 音频回调（在独立线程中运行）                                    */
/* ================================================================== */

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    if (!g_audio_running) {
        memset(stream, 0, len);
        return;
    }
    /* 从环形缓冲区读取 */
    size_t got = rb_read(&g_audio_rb, stream, (size_t)len);
    if (got < (size_t)len) {
        /* 数据不足，剩余部分填静音 */
        memset(stream + got, 0, (size_t)len - got);
    }
    /* 累加已消费字节数，作为音频时钟基准 */
    SDL_AtomicAdd(&g_audio_consumed_bytes, len);
}

/* ================================================================== */
/*  终端辅助函数                                                        */
/* ================================================================== */

static void clear_screen(void)  { fputs("\033[2J\033[H", stdout); }
static void hide_cursor(void)   { fputs("\033[?25l", stdout); }
static void show_cursor(void)   { fputs("\033[?25h", stdout); }

static void sleep_us(int64_t us)
{
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

/* ================================================================== */
/*  灰度 -> ASCII 字符映射                                              */
/* ================================================================== */

static const char ASCII_RAMP[] =
    " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
static const int RAMP_LEN = (int)(sizeof(ASCII_RAMP) - 1);

static inline char gray_to_ascii(uint8_t gray)
{
    int idx = (int)gray * RAMP_LEN / 255;
    if (idx < 0) idx = 0;
    if (idx >= RAMP_LEN) idx = RAMP_LEN - 1;
    return ASCII_RAMP[idx];
}

/* ================================================================== */
/*  命令行参数                                                          */
/* ================================================================== */

typedef struct {
    const char *filename;
    int         out_width;
    int         enable_audio;
} AppConfig;

static void print_usage(const char *prog)
{
    printf("用法: %s [选项] <视频文件> [输出宽度]\n", prog);
    printf("\n");
    printf("选项:\n");
    printf("  -a, --audio    启用音频播放（默认不播放音频）\n");
    printf("  -h, --help     显示此帮助信息\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s movie.mp4            仅播放视频（默认宽度100）\n", prog);
    printf("  %s -a movie.mp4 120     音视频同播，输出宽度120字符\n", prog);
}

static int parse_args(int argc, char *argv[], AppConfig *cfg)
{
    cfg->filename     = NULL;
    cfg->out_width    = 100;
    cfg->enable_audio = 0;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--audio") == 0) {
            cfg->enable_audio = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "错误：未知选项 '%s'\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        } else {
            if (positional == 0) {
                cfg->filename = argv[i];
            } else if (positional == 1) {
                cfg->out_width = atoi(argv[i]);
            } else {
                fprintf(stderr, "错误：多余的参数 '%s'\n", argv[i]);
                return -1;
            }
            positional++;
        }
    }

    if (!cfg->filename) {
        fprintf(stderr, "错误：未指定视频文件\n");
        print_usage(argv[0]);
        return -1;
    }
    if (cfg->out_width < 10) cfg->out_width = 10;
    return 0;
}

/* ================================================================== */
/*  初始化 swresample 上下文（兼容 FFmpeg 4.x ~ 8.x）                 */
/* ================================================================== */

static struct SwrContext *init_swr_context(AVCodecContext *audio_codec_ctx)
{
    struct SwrContext *swr_ctx = NULL;

#if HAVE_SWR_ALLOC_SET_OPTS2
    /* FFmpeg 5.1+: 使用新的 AVChannelLayout API */
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    const AVChannelLayout *in_ch_layout;

#if LIBAVCODEC_VERSION_MAJOR >= 59
    in_ch_layout = &audio_codec_ctx->ch_layout;
#else
    /* FFmpeg 4.x 没有 ch_layout 字段，从旧字段构造 */
    AVChannelLayout in_layout;
    if (audio_codec_ctx->channel_layout)
        av_channel_layout_from_mask(&in_layout, audio_codec_ctx->channel_layout);
    else
        av_channel_layout_default(&in_layout, audio_codec_ctx->channels);
    in_ch_layout = &in_layout;
#endif

    int ret = swr_alloc_set_opts2(&swr_ctx,
        &out_ch_layout, AV_SAMPLE_FMT_S16, AUDIO_SAMPLE_RATE,
        in_ch_layout, audio_codec_ctx->sample_fmt, audio_codec_ctx->sample_rate,
        0, NULL);
    if (ret < 0) return NULL;
#else
    /* FFmpeg 4.x / 5.0: 使用旧的 uint64_t channel layout API */
    uint64_t in_ch_layout;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    if (audio_codec_ctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
        audio_codec_ctx->ch_layout.u.mask != 0)
        in_ch_layout = audio_codec_ctx->ch_layout.u.mask;
    else
        in_ch_layout = (uint64_t)av_get_default_channel_layout(audio_codec_ctx->ch_layout.nb_channels);
#else
    in_ch_layout = audio_codec_ctx->channel_layout ?
                   audio_codec_ctx->channel_layout :
                   (uint64_t)av_get_default_channel_layout(audio_codec_ctx->channels);
#endif
    swr_ctx = swr_alloc_set_opts(NULL,
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, AUDIO_SAMPLE_RATE,
        in_ch_layout, audio_codec_ctx->sample_fmt, audio_codec_ctx->sample_rate,
        0, NULL);
    if (!swr_ctx) return NULL;
#endif

    if (swr_init(swr_ctx) < 0) {
        swr_free(&swr_ctx);
        return NULL;
    }
    return swr_ctx;
}

/* ================================================================== */
/*  主程序                                                              */
/* ================================================================== */

int main(int argc, char *argv[])
{
    AppConfig cfg;
    if (parse_args(argc, argv, &cfg) != 0) return 1;

    int out_width  = cfg.out_width;
    int out_height = out_width / 2;  /* 终端字符高:宽≈2:1，修正比例 */

    /* -------------------------------------------------------------- */
    /* 1. 打开输入文件，找视频流 / 音频流                              */
    /* -------------------------------------------------------------- */

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, cfg.filename, NULL, NULL) < 0) {
        fprintf(stderr, "错误：无法打开文件 '%s'\n", cfg.filename);
        return 1;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "错误：无法获取流信息\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        enum AVMediaType type = fmt_ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx == -1)
            video_stream_idx = (int)i;
        else if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx == -1)
            audio_stream_idx = (int)i;
    }
    if (video_stream_idx == -1) {
        fprintf(stderr, "错误：文件中没有视频流\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream *video_stream = fmt_ctx->streams[video_stream_idx];

    /* -------------------------------------------------------------- */
    /* 2. 初始化解码器（视频）                                         */
    /* -------------------------------------------------------------- */

    const AVCodec *video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!video_codec) {
        fprintf(stderr, "错误：不支持的视频编解码器\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    AVCodecContext *video_codec_ctx = avcodec_alloc_context3(video_codec);
    avcodec_parameters_to_context(video_codec_ctx, video_stream->codecpar);
    if (avcodec_open2(video_codec_ctx, video_codec, NULL) < 0) {
        fprintf(stderr, "错误：无法打开视频解码器\n");
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* 3. 初始化音频（解码器 + swresample + SDL）                     */
    /* -------------------------------------------------------------- */

    AVCodecContext *audio_codec_ctx = NULL;
    struct SwrContext *swr_ctx      = NULL;
    SDL_AudioDeviceID audio_dev     = 0;
    int audio_ready                  = 0;

    if (cfg.enable_audio && audio_stream_idx >= 0) {
        AVStream *audio_stream = fmt_ctx->streams[audio_stream_idx];
        const AVCodec *audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (!audio_codec) {
            fprintf(stderr, "[警告] 不支持的音频编解码器，将仅播放视频\n");
        } else {
            audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            avcodec_parameters_to_context(audio_codec_ctx, audio_stream->codecpar);
            if (avcodec_open2(audio_codec_ctx, audio_codec, NULL) < 0) {
                fprintf(stderr, "[警告] 无法打开音频解码器，将仅播放视频\n");
                avcodec_free_context(&audio_codec_ctx);
                audio_codec_ctx = NULL;
            } else {
                /* 初始化 swresample：统一转为 44100Hz / S16 / 立体声 */
                swr_ctx = init_swr_context(audio_codec_ctx);
                if (!swr_ctx) {
                    fprintf(stderr, "[警告] 无法初始化音频重采样，将仅播放视频\n");
                    avcodec_free_context(&audio_codec_ctx);
                    audio_codec_ctx = NULL;
                } else {
                    /* 初始化环形缓冲区 */
                    if (rb_init(&g_audio_rb, RING_BUFFER_CAPACITY) < 0) {
                        fprintf(stderr, "[警告] 无法初始化音频缓冲区，将仅播放视频\n");
                        swr_free(&swr_ctx);
                        avcodec_free_context(&audio_codec_ctx);
                        audio_codec_ctx = NULL;
                    } else {
                        /* 初始化 SDL 音频 */
                        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
                            fprintf(stderr, "[警告] SDL 初始化失败: %s，将仅播放视频\n", SDL_GetError());
                            rb_destroy(&g_audio_rb);
                            swr_free(&swr_ctx);
                            avcodec_free_context(&audio_codec_ctx);
                            audio_codec_ctx = NULL;
                        } else {
                            SDL_AudioSpec wanted, obtained;
                            memset(&wanted, 0, sizeof(wanted));
                            wanted.freq     = AUDIO_SAMPLE_RATE;
                            wanted.format   = AUDIO_FORMAT;
                            wanted.channels = AUDIO_CHANNELS;
                            wanted.samples  = SDL_AUDIO_SAMPLES;
                            wanted.callback = audio_callback;
                            wanted.userdata = NULL;

                            audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
                            if (audio_dev == 0) {
                                fprintf(stderr, "[警告] 无法打开音频设备: %s，将仅播放视频\n", SDL_GetError());
                                rb_destroy(&g_audio_rb);
                                swr_free(&swr_ctx);
                                avcodec_free_context(&audio_codec_ctx);
                                audio_codec_ctx = NULL;
                                SDL_Quit();
                            } else {
                                SDL_AtomicSet(&g_audio_consumed_bytes, 0);
                                g_audio_running = 1;
                                SDL_PauseAudioDevice(audio_dev, 0);  /* 开始播放 */
                                audio_ready = 1;
                                fprintf(stderr, "[信息] 音频已启用: %d Hz, %d 通道\n",
                                        obtained.freq, obtained.channels);
                            }
                        }
                    }
                }
            }
        }
    } else if (cfg.enable_audio && audio_stream_idx < 0) {
        fprintf(stderr, "[警告] 文件中没有音频流，将仅播放视频\n");
    }

    /* -------------------------------------------------------------- */
    /* 4. 初始化 sws 缩放/格式转换（视频 → GRAY8）                    */
    /* -------------------------------------------------------------- */

    struct SwsContext *sws_ctx = sws_getContext(
        video_codec_ctx->width, video_codec_ctx->height,
        video_codec_ctx->pix_fmt,
        out_width, out_height,
        AV_PIX_FMT_GRAY8,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    AVFrame *gray_frame = av_frame_alloc();
    gray_frame->format = AV_PIX_FMT_GRAY8;
    gray_frame->width  = out_width;
    gray_frame->height = out_height;
    av_frame_get_buffer(gray_frame, 0);

    /* -------------------------------------------------------------- */
    /* 5. 计算视频帧率                                                 */
    /* -------------------------------------------------------------- */

    double fps = av_q2d(video_stream->avg_frame_rate);
    if (fps <= 0.0) fps = av_q2d(video_stream->r_frame_rate);
    if (fps <= 0.0) fps = 25.0;
    int64_t frame_interval_us = (int64_t)(1000000.0 / fps);

    fprintf(stderr, "[信息] 视频: %dx%d, 帧率: %.2f fps\n",
            video_codec_ctx->width, video_codec_ctx->height, fps);
    fprintf(stderr, "[信息] 输出: %dx%d 字符, 按 Ctrl+C 退出\n\n",
            out_width, out_height);

    /* -------------------------------------------------------------- */
    /* 6. 解码循环                                                     */
    /* -------------------------------------------------------------- */

    AVPacket *packet       = av_packet_alloc();
    AVFrame  *video_frame  = av_frame_alloc();
    AVFrame  *audio_frame  = audio_ready ? av_frame_alloc() : NULL;

    /* 音频重采样输出缓冲区 */
    uint8_t *audio_out_buf      = NULL;
    int      audio_out_buf_size = 0;

    int64_t start_time      = av_gettime_relative();
    int     video_frame_count = 0;
    int     dropped_frames    = 0;

    hide_cursor();
    clear_screen();

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        /* -------------------- 视频包 -------------------- */
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(video_codec_ctx, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }
            while (avcodec_receive_frame(video_codec_ctx, video_frame) == 0) {
                /* 计算当前帧的 PTS（秒） */
                double frame_pts;
                int64_t pts = video_frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) {
                    frame_pts = (double)video_frame_count / fps;
                } else {
                    frame_pts = pts * av_q2d(video_stream->time_base);
                }

                /* A/V 同步：以音频时钟为主时钟 */
                if (audio_ready) {
                    double audio_clock = get_audio_clock();
                    double diff = frame_pts - audio_clock;

                    if (diff > AV_SYNC_THRESHOLD) {
                        /* 视频比音频快，等待 */
                        int64_t wait_us_val = (int64_t)(diff * 1000000);
                        if (wait_us_val > 100000) wait_us_val = 100000;  /* 最多等 100ms */
                        sleep_us(wait_us_val);
                    } else if (diff < -AV_DROP_THRESHOLD) {
                        /* 视频比音频慢太多，丢帧 */
                        dropped_frames++;
                        av_frame_unref(video_frame);
                        continue;
                    }
                } else {
                    /* 无音频模式：按帧率控制 */
                    int64_t expected = start_time + (int64_t)video_frame_count * frame_interval_us;
                    int64_t now      = av_gettime_relative();
                    sleep_us(expected - now);
                }

                /* 缩放并转灰度 */
                sws_scale(sws_ctx,
                          (const uint8_t *const *)video_frame->data,
                          video_frame->linesize,
                          0, video_codec_ctx->height,
                          gray_frame->data, gray_frame->linesize);

                /* 光标归位并输出 ASCII 帧 */
                fputs("\033[H", stdout);
                uint8_t *row = gray_frame->data[0];
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        putchar(gray_to_ascii(row[x]));
                    }
                    if (y < out_height - 1) putchar('\n');
                    row += gray_frame->linesize[0];
                }
                fflush(stdout);

                video_frame_count++;
                av_frame_unref(video_frame);
            }
        }
        /* -------------------- 音频包 -------------------- */
        else if (audio_ready && packet->stream_index == audio_stream_idx) {
            if (avcodec_send_packet(audio_codec_ctx, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }
            while (avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
                /* 计算重采样输出所需缓冲区大小 */
                int out_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                int needed = av_samples_get_buffer_size(NULL, AUDIO_CHANNELS,
                                                         out_samples, AV_SAMPLE_FMT_S16, 1);
                if (needed > audio_out_buf_size) {
                    uint8_t *tmp = (uint8_t *)realloc(audio_out_buf, needed);
                    if (!tmp) {
                        fprintf(stderr, "[警告] 音频缓冲区分配失败，跳过音频帧\n");
                        av_frame_unref(audio_frame);
                        continue;
                    }
                    audio_out_buf = tmp;
                    audio_out_buf_size = needed;
                }

                /* 重采样 */
                int converted = swr_convert(swr_ctx,
                    &audio_out_buf, out_samples,
                    (const uint8_t **)audio_frame->data, audio_frame->nb_samples);

                if (converted > 0) {
                    int bytes = converted * AUDIO_BYTES_PER_FRAME;
                    /* 写入环形缓冲区，若满则短暂等待后重试 */
                    int total_written = 0;
                    while (total_written < bytes) {
                        size_t w = rb_write(&g_audio_rb,
                                             audio_out_buf + total_written,
                                             (size_t)(bytes - total_written));
                        if (w == 0) {
                            SDL_Delay(5);  /* 缓冲区满，等 5ms */
                        }
                        total_written += (int)w;
                    }
                }
                av_frame_unref(audio_frame);
            }
        }

        av_packet_unref(packet);
    }

    /* -------------------------------------------------------------- */
    /* 7. 冲刷解码器缓冲区                                              */
    /* -------------------------------------------------------------- */

    /* 视频冲刷 */
    avcodec_send_packet(video_codec_ctx, NULL);
    while (avcodec_receive_frame(video_codec_ctx, video_frame) == 0) {
        double frame_pts = (double)video_frame_count / fps;

        if (audio_ready) {
            double diff = frame_pts - get_audio_clock();
            if (diff > AV_SYNC_THRESHOLD) sleep_us((int64_t)(diff * 1000000));
            else if (diff < -AV_DROP_THRESHOLD) { dropped_frames++; av_frame_unref(video_frame); continue; }
        } else {
            int64_t expected = start_time + (int64_t)video_frame_count * frame_interval_us;
            sleep_us(expected - av_gettime_relative());
        }

        sws_scale(sws_ctx, (const uint8_t *const *)video_frame->data,
                  video_frame->linesize, 0, video_codec_ctx->height,
                  gray_frame->data, gray_frame->linesize);
        fputs("\033[H", stdout);
        uint8_t *row = gray_frame->data[0];
        for (int y = 0; y < out_height; y++) {
            for (int x = 0; x < out_width; x++) putchar(gray_to_ascii(row[x]));
            if (y < out_height - 1) putchar('\n');
            row += gray_frame->linesize[0];
        }
        fflush(stdout);
        video_frame_count++;
        av_frame_unref(video_frame);
    }

    /* 音频冲刷 */
    if (audio_ready) {
        avcodec_send_packet(audio_codec_ctx, NULL);
        while (avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
            int out_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
            int needed = av_samples_get_buffer_size(NULL, AUDIO_CHANNELS, out_samples, AV_SAMPLE_FMT_S16, 1);
            if (needed > audio_out_buf_size) {
                audio_out_buf = (uint8_t *)realloc(audio_out_buf, needed);
                audio_out_buf_size = needed;
            }
            int converted = swr_convert(swr_ctx, &audio_out_buf, out_samples,
                                         (const uint8_t **)audio_frame->data, audio_frame->nb_samples);
            if (converted > 0) {
                int bytes = converted * AUDIO_BYTES_PER_FRAME;
                int written = 0;
                while (written < bytes) {
                    size_t w = rb_write(&g_audio_rb, audio_out_buf + written, bytes - written);
                    if (w == 0) SDL_Delay(5);
                    written += (int)w;
                }
            }
            av_frame_unref(audio_frame);
        }
        /* 等待环形缓冲区中的剩余音频播放完毕 */
        while (rb_available(&g_audio_rb) > 0) SDL_Delay(10);
        SDL_Delay(100);  /* 再等一点，让 SDL 内部缓冲区播完 */
    }

    /* -------------------------------------------------------------- */
    /* 8. 清理                                                          */
    /* -------------------------------------------------------------- */

    show_cursor();
    printf("\n\n[完成] 共播放 %d 帧", video_frame_count);
    if (audio_ready) printf("，丢帧 %d", dropped_frames);
    printf("\n");

    free(audio_out_buf);
    if (audio_frame) av_frame_free(&audio_frame);
    av_frame_free(&video_frame);
    av_frame_free(&gray_frame);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);

    if (audio_ready) {
        g_audio_running = 0;
        SDL_PauseAudioDevice(audio_dev, 1);
        SDL_CloseAudioDevice(audio_dev);
        SDL_Quit();
        rb_destroy(&g_audio_rb);
        swr_free(&swr_ctx);
    }
    if (audio_codec_ctx) avcodec_free_context(&audio_codec_ctx);
    avcodec_free_context(&video_codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}
