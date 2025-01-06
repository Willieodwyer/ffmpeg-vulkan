extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <fstream>
#include <iostream>

#define WRITE

#ifdef WRITE
std::ofstream image("image.yuv", std::ios::binary);
#endif

void process_hardware_frame(AVCodecContext* dec_ctx, AVFrame* hw_frame)
{
  AVFrame* cpu_frame = av_frame_alloc();
  if (!cpu_frame) {
    std::cerr << "Could not allocate CPU frame\n";
    return;
  }

  // Transfer data from GPU (hw_frame) to CPU (cpu_frame)
  if (av_hwframe_transfer_data(cpu_frame, hw_frame, 0) < 0) {
    std::cerr << "Error transferring the frame data to CPU\n";
    av_frame_free(&cpu_frame);
    return;
  }

#ifdef WRITE
  // Do something with the CPU frame here
  if (cpu_frame->format == AV_PIX_FMT_NV12) {
    // Write the Y (luma) plane
    for (int i = 0; i < cpu_frame->height; ++i) {
      image.write(reinterpret_cast<const char*>(cpu_frame->data[0] + (i * cpu_frame->linesize[0])), cpu_frame->width);
    }

    // Write the UV (chroma) interleaved plane
    for (int i = 0; i < cpu_frame->height / 2; ++i) {
      for (int j = 0; j < cpu_frame->linesize[1]; j += 2)
        image.write((const char*)(cpu_frame->data[1] + (i * cpu_frame->linesize[0]) + j), 1);
    }

    for (int i = 0; i < cpu_frame->height / 2; ++i) {
      for (int j = 1; j < cpu_frame->linesize[1]; j += 2)
        image.write((const char*)(cpu_frame->data[1] + (i * cpu_frame->linesize[0]) + j), 1);
    }
  }
  else {
    std::cerr << "Frame is not in NV12 format!" << std::endl;
  }
#endif

  av_frame_free(&cpu_frame);
}

static int set_hwframe_ctx(AVCodecContext* ctx, AVBufferRef* hw_device_ctx, AVPixelFormat pix_fmt)
{
  AVBufferRef* hw_frames_ref;
  AVHWFramesContext* frames_ctx = NULL;
  int err = 0;

  if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
    fprintf(stderr, "Failed to create hardware frame context.\n");
    return -1;
  }
  frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
  frames_ctx->format = pix_fmt;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = 1920;
  frames_ctx->height = 1080;
  frames_ctx->initial_pool_size = 20;
  if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
    char error_str[1024];
    av_make_error_string(error_str, sizeof(error_str), err);
    fprintf(stderr,
            "Failed to initialize VAAPI frame context."
            "Error code: %s\n",
            error_str);
    av_buffer_unref(&hw_frames_ref);
    return err;
  }
  ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
  if (!ctx->hw_frames_ctx)
    err = AVERROR(ENOMEM);

  av_buffer_unref(&hw_frames_ref);
  return err;
}

static AVCodecContext* OpenVideoStream(AVFormatContext* fmt_ctx, int stream_idx, AVHWDeviceType device_type)
{
  AVCodecParameters* codecpar = fmt_ctx->streams[stream_idx]->codecpar;
  const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
  if (!decoder) {
    std::cerr << "Decoder not found\n";
    return nullptr;
  }

  // Print codec information
  std::cout << "Video codec: " << avcodec_get_name(codecpar->codec_id) << std::endl;
  std::cout << "Width: " << codecpar->width << " Height: " << codecpar->height << std::endl;
  std::cout << "Bitrate: " << codecpar->bit_rate << std::endl;

  AVCodecContext* codec_ctx;
  codec_ctx = avcodec_alloc_context3(NULL);
  if (!codec_ctx) {
    printf("avcodec_alloc_context3 failed");
    return NULL;
  }

  int result = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_idx]->codecpar);
  if (result < 0) {
    char error_str[1024];
    av_make_error_string(error_str, sizeof(error_str), result);
    printf("avcodec_parameters_to_context failed: %s\n", error_str);
    avcodec_free_context(&codec_ctx);
    return NULL;
  }
  codec_ctx->pkt_timebase = fmt_ctx->streams[stream_idx]->time_base;

  if (device_type != AV_HWDEVICE_TYPE_NONE) {
    /* Look for supported hardware accelerated configurations */
    int i = 0;
    const AVCodecHWConfig* accel_config = nullptr;
    {
      const AVCodecHWConfig* config = nullptr;
      while ((config = avcodec_get_hw_config(decoder, i++)) != NULL) {
        printf("Found %s hardware acceleration with pixel format %s\n", av_hwdevice_get_type_name(config->device_type),
               av_get_pix_fmt_name(config->pix_fmt));

        if (config->device_type != device_type || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
          continue;
        }
        accel_config = config;
      }
    }

    if (!accel_config) {
      std::cerr << "Unable to locate hw acceleration type: " << av_hwdevice_get_type_name(device_type) << std::endl;
      return nullptr;
    }

    result = av_hwdevice_ctx_create(&codec_ctx->hw_device_ctx, accel_config->device_type, NULL, NULL, 0);
    if (result < 0) {
      char error_str[1024];
      av_make_error_string(error_str, sizeof(error_str), result);
      printf("Couldn't create %s hardware device context: %s", av_hwdevice_get_type_name(accel_config->device_type),
             error_str);
    }
    else {
      printf(" -- Using %s hardware acceleration with pixel format %s\n",
             av_hwdevice_get_type_name(accel_config->device_type), av_get_pix_fmt_name(accel_config->pix_fmt));
    }

    codec_ctx->pix_fmt = accel_config->pix_fmt;
  }

  if (codecpar->codec_id == AV_CODEC_ID_VVC) {
    codec_ctx->strict_std_compliance = -2;

    /* Enable threaded decoding, VVC decode is slow */
    codec_ctx->thread_count = 4;
    codec_ctx->thread_type = (FF_THREAD_FRAME | FF_THREAD_SLICE);
  }
  else
    codec_ctx->thread_count = 1;

  int err = set_hwframe_ctx(codec_ctx, codec_ctx->hw_device_ctx, codec_ctx->pix_fmt);
  if (err < 0) {
    fprintf(stderr, "Failed to set hwframe context.\n");
    return NULL;
  }

  result = avcodec_open2(codec_ctx, decoder, NULL);
  if (result < 0) {
    char error_str[1024];
    av_make_error_string(error_str, sizeof(error_str), result);
    printf("Couldn't open codec %s: %s", avcodec_get_name(codec_ctx->codec_id), error_str);
    avcodec_free_context(&codec_ctx);
    return NULL;
  }

  return codec_ctx;
}

bool DecodeFrame(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt, int width, int height)
{
  int ret = avcodec_send_packet(dec_ctx, pkt);
  if (ret < 0) {
    std::cerr << "Error sending a packet for decoding\n";
    return false;
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      continue;
    }

    if (ret < 0) {
      std::cerr << "Error during decoding\n";
      return false;
    }

    // std::cout << "Decoded frame at " << frame->pts << "\n";
    switch (frame->format) {
    case AV_PIX_FMT_YUV420P:
#ifdef WRITE
      for (int plane = 0; plane < 3; ++plane) {
        auto height = frame->height / (frame->width / frame->linesize[plane]);
        for (int i = 0; i < height; ++i) {
          image.write(reinterpret_cast<const char*>(frame->data[plane] + (i * frame->linesize[plane])),
                      frame->linesize[plane]);
        }
      }
#endif
      break;
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_VULKAN:
    case AV_PIX_FMT_VDPAU:
      process_hardware_frame(dec_ctx, frame);
      break;
    default:
      std::cerr << "Unknown pixel format " << av_get_pix_fmt_name((AVPixelFormat)frame->format) << std::endl;
      return false;
    }

#ifdef WRITE
    image.flush();
#endif
  }

  return true;
}


int main(int argc, char* argv[])
{
  AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VULKAN;
  if (argc >= 2) {
    if (strcmp(argv[1], "vulkan") == 0)
      hw_type = AV_HWDEVICE_TYPE_VULKAN;
    if (strcmp(argv[1], "vaapi") == 0)
      hw_type = AV_HWDEVICE_TYPE_VAAPI;
    if (strcmp(argv[1], "vdpau") == 0)
      hw_type = AV_HWDEVICE_TYPE_VDPAU;
    if (strcmp(argv[1], "none") == 0)
      hw_type = AV_HWDEVICE_TYPE_NONE;
  }

  int width = 0, height = 0;
  if (argc >= 4) {
    width = std::stoi(argv[2]);
    height = std::stoi(argv[3]);
  }

  auto str = av_hwdevice_get_type_name(hw_type);
  std::cout << "Using hw acceleration: " << (str ? str : "none") << std::endl;

  const char* filename = "input.mp4";

  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) != 0) {
    std::cerr << "Could not open source file\n";
    return -1;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    std::cerr << "Could not find stream information\n";
    return -1;
  }

  // Display some basic information about the file and streams
  std::cout << "Container format: " << fmt_ctx->iformat->name << std::endl;
  std::cout << "Duration: " << fmt_ctx->duration << " microseconds" << std::endl;
  std::cout << "Number of streams: " << fmt_ctx->nb_streams << std::endl;

  int video_stream_index = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      break;
    }
  }

  if (video_stream_index == -1) {
    std::cerr << "Could not find a video stream\n";
    return -1;
  }

  AVCodecContext* dec_ctx = OpenVideoStream(fmt_ctx, video_stream_index, hw_type);
  if (!dec_ctx) {
    std::cerr << "Failed to open decoder" << std::endl;
    return -1;
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  auto start = std::chrono::high_resolution_clock::now();
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      if (!DecodeFrame(dec_ctx, frame, pkt, width, height)) {
        av_packet_unref(pkt);
        break;
      }
      av_packet_unref(pkt);
    }
  }

  DecodeFrame(dec_ctx, frame, nullptr, width, height); // Flush the decoder

  auto finish = std::chrono::high_resolution_clock::now();
  std::cout << "Time taken: " << std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count()
            << " milliseconds\n";

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt_ctx);

#ifdef WRITE
  image.close();
#endif

  std::cout << "Decoding finished\n";
  return 0;
}
