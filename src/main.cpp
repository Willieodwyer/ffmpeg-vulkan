extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <fstream>
#include <iostream>

std::ofstream image("image.yuv", std::ios::binary);

void process_hw_frame(AVCodecContext* dec_ctx, AVFrame* hw_frame)
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

    image.flush();
  }
  else {
    std::cerr << "Frame is not in NV12 format!" << std::endl;
  }

  av_frame_free(&cpu_frame);
}

void decode(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt)
{
  int ret = avcodec_send_packet(dec_ctx, pkt);
  if (ret < 0) {
    std::cerr << "Error sending a packet for decoding\n";
    return;
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      return;

    if (ret < 0) {
      std::cerr << "Error during decoding\n";
      return;
    }

    std::cout << "Decoded frame at " << frame->pts << "\n";

    process_hw_frame(dec_ctx, frame);
  }
}

static bool SupportedPixelFormat(const enum AVPixelFormat format) { return AVPixelFormat::AV_PIX_FMT_VULKAN == format; }

static enum AVPixelFormat GetSupportedPixelFormat(AVCodecContext* s, const enum AVPixelFormat* pix_fmts)
{
  const enum AVPixelFormat* p;

  for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);

    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
      /* We support all memory formats using swscale */
      break;
    }

    if (SupportedPixelFormat(*p)) {
      /* We support this format */
      break;
    }
  }

  if (*p == AV_PIX_FMT_NONE) {
    printf("Couldn't find a supported pixel format:\n");
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
      printf("    %s\n", av_get_pix_fmt_name(*p));
    }
  }

  return *p;
}


static AVCodecContext* OpenVulkanVideoStream(AVFormatContext* fmt_ctx, int stream_idx)
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

  AVCodecContext* context;
  context = avcodec_alloc_context3(NULL);
  if (!context) {
    printf("avcodec_alloc_context3 failed");
    return NULL;
  }

  int result = avcodec_parameters_to_context(context, fmt_ctx->streams[stream_idx]->codecpar);
  if (result < 0) {
    char error_str[1024];
    av_make_error_string(error_str, sizeof(error_str), result);
    printf("avcodec_parameters_to_context failed: %s\n", error_str);
    avcodec_free_context(&context);
    return NULL;
  }
  context->pkt_timebase = fmt_ctx->streams[stream_idx]->time_base;

  /* Look for supported hardware accelerated configurations */
  int i = 0;
  const AVCodecHWConfig* config;
  while ((config = avcodec_get_hw_config(decoder, i++)) != NULL) {
    printf("Found %s hardware acceleration with pixel format %s\n", av_hwdevice_get_type_name(config->device_type),
           av_get_pix_fmt_name(config->pix_fmt));

    if (config->device_type != AV_HWDEVICE_TYPE_VULKAN ||
        !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      continue;
    }

    {
      result = av_hwdevice_ctx_create(&context->hw_device_ctx, config->device_type, NULL, NULL, 0);
      if (result < 0) {
        char error_str[1024];
        av_make_error_string(error_str, sizeof(error_str), result);
        printf("Couldn't create %s hardware device context: %s", av_hwdevice_get_type_name(config->device_type),
               error_str);
      }
      else {
        printf(" -- Using %s hardware acceleration with pixel format %s\n", av_hwdevice_get_type_name(config->device_type),
               av_get_pix_fmt_name(config->pix_fmt));
      }
    }
  }

  /* Allow supported hardware accelerated pixel formats */
  context->get_format = GetSupportedPixelFormat;

  if (codecpar->codec_id == AV_CODEC_ID_VVC) {
    context->strict_std_compliance = -2;

    /* Enable threaded decoding, VVC decode is slow */
    context->thread_count = 4;
    context->thread_type = (FF_THREAD_FRAME | FF_THREAD_SLICE);
  }

  result = avcodec_open2(context, decoder, NULL);
  if (result < 0) {
    char error_str[1024];
    av_make_error_string(error_str, sizeof(error_str), result);
    printf("Couldn't open codec %s: %s", avcodec_get_name(context->codec_id), error_str);
    avcodec_free_context(&context);
    return NULL;
  }

  return context;
}

int main(int argc, char* argv[])
{
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

  AVCodecContext* dec_ctx = OpenVulkanVideoStream(fmt_ctx, video_stream_index);
  if (!dec_ctx) {
    std::cerr << "Failed to open vulkan decoder" << std::endl;
    return -1;
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index)
      decode(dec_ctx, frame, pkt);


    av_packet_unref(pkt);
  }

  decode(dec_ctx, frame, nullptr); // Flush the decoder

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt_ctx);

  image.close();

  std::cout << "Decoding finished\n";
  return 0;
}