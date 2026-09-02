/*
 * terminal_video_player.c
 *
 * 使用 FFmpeg 解码视频，并在终端以 ASCII 字符画的形式播放。
 *
 * 依赖：libavformat, libavcodec, libswscale, libavutil
 *
 * 编译：
 *   gcc -O2 terminal_video_player.c -o terminal_video_player \
 *       $(pkg-config --cflags --libs libavformat libavcodec libswscale libavutil)
 *
 * 用法：
 *   ./terminal_video_player <视频文件> [输出宽度]
 *
 * 示例：
 *   ./terminal_video_player test.mp4
 *   ./terminal_video_player test.mp4 120
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>

/* ------------------------------------------------------------------ */
/* 终端工具                                                            */
/* ------------------------------------------------------------------ */

/* 清屏并把光标移到左上角 */
static void clear_screen(void)
{
    /* ESC[2J 清屏，ESC[H 光标归位 */
    fputs("\033[2J\033[H", stdout);
}

/* 隐藏 / 恢复光标 */
static void hide_cursor(void) { fputs("\033[?25l", stdout); }
static void show_cursor(void) { fputs("\033[?25h", stdout); }

/* ------------------------------------------------------------------ */
/* 灰度 -> ASCII 字符映射                                              */
/* ------------------------------------------------------------------ */

/*
 * 从暗到亮的字符梯度。
 * 字符在终端里占的"墨水量"越多，看起来越亮。
 */
static const char ASCII_RAMP[] =
    " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

static const int RAMP_LEN = (int)(sizeof(ASCII_RAMP) - 1);

static inline char gray_to_ascii(uint8_t gray)
{
    /* gray: 0(黑) ~ 255(白) */
    int idx = (int)gray * RAMP_LEN / 255;
    if (idx < 0) idx = 0;
    if (idx >= RAMP_LEN) idx = RAMP_LEN - 1;
    return ASCII_RAMP[idx];
}

/* ------------------------------------------------------------------ */
/* 高精度睡眠（微秒）                                                  */
/* ------------------------------------------------------------------ */

static void sleep_us(int64_t us)
{
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* 主程序                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <视频文件> [输出宽度]\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int out_width  = (argc >= 3) ? atoi(argv[2]) : 100;
    if (out_width < 10) out_width = 10;

    /*
     * 终端字符通常高度 > 宽度（大约 2:1），
     * 所以输出高度取宽度的一半，避免画面被拉长。
     */
    int out_height = out_width / 2;

    /* -------------------------------------------------------------- */
    /* 1. 打开输入文件，找视频流                                      */
    /* -------------------------------------------------------------- */

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "错误：无法打开文件 '%s'\n", filename);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "错误：无法获取流信息\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* 找第一个视频流 */
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = (int)i;
            break;
        }
    }
    if (video_stream_idx == -1) {
        fprintf(stderr, "错误：文件中没有视频流\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVStream *video_stream = fmt_ctx->streams[video_stream_idx];
    AVCodecParameters *codecpar = video_stream->codecpar;

    /* -------------------------------------------------------------- */
    /* 2. 初始化解码器                                                */
    /* -------------------------------------------------------------- */

    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "错误：不支持的编解码器\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "错误：无法分配解码器上下文\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "错误：无法复制编解码器参数\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "错误：无法打开解码器\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* 3. 初始化 sws 缩放/格式转换                                    */
    /*    把解码后的帧转成 GRAY8（单通道灰度）                       */
    /* -------------------------------------------------------------- */

    struct SwsContext *sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height,
        codec_ctx->pix_fmt,
        out_width, out_height,
        AV_PIX_FMT_GRAY8,
        SWS_BILINEAR,
        NULL, NULL, NULL
    );
    if (!sws_ctx) {
        fprintf(stderr, "错误：无法初始化 sws 上下文\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* 分配灰度帧缓冲区 */
    AVFrame *gray_frame = av_frame_alloc();
    if (!gray_frame) {
        fprintf(stderr, "错误：无法分配帧\n");
        sws_freeContext(sws_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    gray_frame->format = AV_PIX_FMT_GRAY8;
    gray_frame->width  = out_width;
    gray_frame->height = out_height;
    if (av_frame_get_buffer(gray_frame, 0) < 0) {
        fprintf(stderr, "错误：无法分配帧缓冲区\n");
        av_frame_free(&gray_frame);
        sws_freeContext(sws_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* 4. 计算帧率（用于控制播放速度）                                */
    /* -------------------------------------------------------------- */

    double fps = av_q2d(video_stream->avg_frame_rate);
    if (fps <= 0.0) {
        /* 某些容器 avg_frame_rate 为 0，回退到 r_frame_rate */
        fps = av_q2d(video_stream->r_frame_rate);
    }
    if (fps <= 0.0) fps = 25.0;  /* 兜底 */

    int64_t frame_interval_us = (int64_t)(1000000.0 / fps);

    fprintf(stderr, "[信息] 视频: %dx%d, 帧率: %.2f fps\n",
            codec_ctx->width, codec_ctx->height, fps);
    fprintf(stderr, "[信息] 输出: %dx%d 字符, 按 Ctrl+C 退出\n\n",
            out_width, out_height);

    /* -------------------------------------------------------------- */
    /* 5. 解码循环                                                    */
    /* -------------------------------------------------------------- */

    AVPacket *packet = av_packet_alloc();
    AVFrame  *frame  = av_frame_alloc();
    if (!packet || !frame) {
        fprintf(stderr, "错误：无法分配 packet/frame\n");
        av_frame_free(&gray_frame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        sws_freeContext(sws_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    hide_cursor();
    clear_screen();

    int64_t start_time = av_gettime_relative();
    int frame_count = 0;

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index != video_stream_idx) {
            av_packet_unref(packet);
            continue;
        }

        /* 发送 packet 到解码器 */
        int ret = avcodec_send_packet(codec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) continue;

        /* 接收解码后的帧（一个 packet 可能产生多帧） */
        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            /* 缩放到目标尺寸并转灰度 */
            sws_scale(sws_ctx,
                      (const uint8_t *const *)frame->data,
                      frame->linesize,
                      0, codec_ctx->height,
                      gray_frame->data,
                      gray_frame->linesize);

            /* 光标归位（不整屏清，减少闪烁） */
            fputs("\033[H", stdout);

            /* 逐行输出 ASCII */
            uint8_t *row = gray_frame->data[0];
            for (int y = 0; y < out_height; y++) {
                for (int x = 0; x < out_width; x++) {
                    putchar(gray_to_ascii(row[x]));
                }
                /* 行尾换行，但最后一行不换（避免滚动） */
                if (y < out_height - 1) putchar('\n');
                row += gray_frame->linesize[0];
            }
            fflush(stdout);

            frame_count++;

            /* -------------------------------------------------- */
            /* 帧率控制：按已播放帧数计算应到时间，差值即需睡眠  */
            /* -------------------------------------------------- */
            int64_t expected = start_time + (int64_t)frame_count * frame_interval_us;
            int64_t now      = av_gettime_relative();
            sleep_us(expected - now);

            av_frame_unref(frame);
        }
    }

    /* 冲刷解码器中剩余的帧 */
    avcodec_send_packet(codec_ctx, NULL);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        sws_scale(sws_ctx,
                  (const uint8_t *const *)frame->data,
                  frame->linesize,
                  0, codec_ctx->height,
                  gray_frame->data,
                  gray_frame->linesize);

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
        frame_count++;

        int64_t expected = start_time + (int64_t)frame_count * frame_interval_us;
        int64_t now      = av_gettime_relative();
        sleep_us(expected - now);

        av_frame_unref(frame);
    }

    /* -------------------------------------------------------------- */
    /* 6. 清理                                                        */
    /* -------------------------------------------------------------- */

    show_cursor();
    printf("\n\n[完成] 共播放 %d 帧\n", frame_count);

    av_frame_free(&gray_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return 0;
}
