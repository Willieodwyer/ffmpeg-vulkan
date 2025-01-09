extern "C" {
#include <libavcodec/avcodec.h>

#include <libswscale/swscale.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libavformat/avformat.h>

#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <fstream>
#include <iostream>

#include "SoftwareScale.h"

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

void process_with_scaling(AVCodecContext* dec_ctx, AVFrame* hw_frame, int target_width, int target_height)
{
  // av_log_set_level(AV_LOG_DEBUG);

  std::string filter = "none";
  if (hw_frame->format == AV_PIX_FMT_VAAPI)
    filter = "scale_vaapi";
  else if (hw_frame->format == AV_PIX_FMT_VULKAN) {
    scale_incompatible_hwframe(dec_ctx, hw_frame, target_width, target_height);
    return;
  }
  else if (hw_frame->format == AV_PIX_FMT_VDPAU) {
    std::cerr << "VDPAU scaling not supported\n";
    return;
  }

  AVFrame* scaled_frame = nullptr;
  AVFilterContext* scale_ctx = nullptr;
  enum AVPixelFormat pix_fmts[] = {(AVPixelFormat)hw_frame->format};

  const AVFilter* buffer_src = avfilter_get_by_name("buffer");
  const AVFilter* buffer_sink = avfilter_get_by_name("buffersink");
  const AVFilter* scale_hardware = avfilter_get_by_name(filter.c_str());

  int ret;
  AVFilterContext *buffer_src_ctx = NULL, *buffer_sink_ctx = NULL;
  AVBufferSrcParameters* par = NULL;

  // Initialize the filter graph
  AVFilterGraph* graph = avfilter_graph_alloc();
  if (!graph) {
    std::cerr << "Could not allocate filter graph\n";
    goto fail;
  }

  par = av_buffersrc_parameters_alloc();
  if (!par) {
    std::cerr << "Could not allocate parameters\n";
    goto fail;
  }

  buffer_src_ctx = avfilter_graph_alloc_filter(graph, buffer_src, "buffersrc");
  if (!buffer_src_ctx) {
    std::cerr << "Could not allocate AVFilterContext\n";
    goto fail;
  }

  par->format = hw_frame->format;
  par->time_base = (AVRational){1, 1};
  par->width = hw_frame->width;
  par->height = hw_frame->height;
  par->hw_frames_ctx = hw_frame->hw_frames_ctx;
  ret = av_buffersrc_parameters_set(buffer_src_ctx, par);
  if (ret < 0) {
    std::cerr << "Could not set parameters\n";
    goto fail;
  }

  ret = avfilter_init_dict(buffer_src_ctx, NULL);
  if (ret < 0)
    goto fail;

  buffer_sink_ctx = avfilter_graph_alloc_filter(graph, buffer_sink, "buffersink");
  if (!buffer_sink_ctx) {
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  // Create and configure scaling filter
  char args[1024];
  snprintf(args, sizeof(args), "w=%d:h=%d:format=%s", target_width, target_height, "nv12");

  if (avfilter_graph_create_filter(&scale_ctx, scale_hardware, "scale", args, nullptr, graph) < 0) {
    std::cerr << "Could not create scaling filter\n";
    goto fail;
  }

  // Set the allowed pixel formats for the sink
  ret = av_opt_set_bin(buffer_sink_ctx, "pix_fmts", (const uint8_t*)pix_fmts, sizeof(pix_fmts), AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    std::cerr << "Could not set output pixel format\n";
    goto fail;
  }

  ret = avfilter_init_str(buffer_sink_ctx, nullptr);
  if (ret < 0) {
    std::cerr << "Could not initialize buffersink\n";
    goto fail;
  }

  // Connect the filters
  if (avfilter_link(buffer_src_ctx, 0, scale_ctx, 0) < 0) {
    std::cerr << "Could not link buffer source to scale filter\n";
    goto fail;
  }

  if (avfilter_link(scale_ctx, 0, buffer_sink_ctx, 0) < 0) {
    std::cerr << "Could not link scale filter to buffer sink\n";
    goto fail;
  }

  // Configure the graph
  if (avfilter_graph_config(graph, nullptr) < 0) {
    std::cerr << "Could not configure filter graph\n";
    goto fail;
  }

  // Push the frame to the filter graph
  if (av_buffersrc_add_frame(buffer_src_ctx, hw_frame) < 0) {
    std::cerr << "Error while feeding the frame to the filter graph\n";
    goto fail;
  }

  // Retrieve the scaled frame
  scaled_frame = av_frame_alloc();
  if (!scaled_frame) {
    std::cerr << "Could not allocate scaled frame\n";
    goto fail;
  }

  while (av_buffersink_get_frame(buffer_sink_ctx, scaled_frame) >= 0) {
    process_hardware_frame(dec_ctx, scaled_frame);
    av_frame_unref(scaled_frame);
  }

// Cleanup
fail:
  if (scaled_frame)
    av_frame_free(&scaled_frame);
  if (graph)
    avfilter_graph_free(&graph);
  if (par)
    av_freep(&par);
}

static int set_hwframe_ctx(AVCodecContext* ctx, AVBufferRef* hw_device_ctx, AVPixelFormat pix_fmt, int width,
                           int height)
{
  AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
  if (!hw_frames_ref) {
    fprintf(stderr, "Failed to create hardware frame context.\n");
    return -1;
  }

  AVHWFramesContext* frames_ctx = (AVHWFramesContext*)(hw_frames_ref->data);
  frames_ctx->format = pix_fmt; // Hardware pixel format
  frames_ctx->sw_format = AV_PIX_FMT_YUV420P; // Software pixel format for transfer
  frames_ctx->width = width;
  frames_ctx->height = height;
  frames_ctx->initial_pool_size = 20;

  if (av_hwframe_ctx_init(hw_frames_ref) < 0) {
    fprintf(stderr, "Failed to initialize hardware frame context.\n");
    av_buffer_unref(&hw_frames_ref);
    return -1;
  }

  ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
  if (!ctx->hw_frames_ctx) {
    av_buffer_unref(&hw_frames_ref);
    return AVERROR(ENOMEM);
  }

  av_buffer_unref(&hw_frames_ref);
  return 0;
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

  if (codec_ctx->hw_device_ctx) {
    int err =
        set_hwframe_ctx(codec_ctx, codec_ctx->hw_device_ctx, codec_ctx->pix_fmt, codecpar->width, codecpar->height);
    if (err < 0) {
      fprintf(stderr, "Failed to set hwframe context.\n");
      return NULL;
    }
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
      // Scale frame if width and height are specified
      if (width > 0 && height > 0) {
        if (!software_scale(frame, width, height)) {
          std::cerr << "Error scaling frame\n";
          return false;
        }
      }
      else {
#ifdef WRITE
        for (int plane = 0; plane < 3; ++plane) {
          auto height = frame->height / (frame->width / frame->linesize[plane]);
          for (int i = 0; i < height; ++i) {
            image.write(reinterpret_cast<const char*>(frame->data[plane] + (i * frame->linesize[plane])),
                        frame->linesize[plane]);
          }
        }
#endif
      }
      break;
    case AV_PIX_FMT_VAAPI:
    case AV_PIX_FMT_VULKAN:
    case AV_PIX_FMT_VDPAU:
      if (width > 0 && height > 0)
        process_with_scaling(dec_ctx, frame, width, height);
      else
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
