#ifndef SOFTWARESCALE_H
#define SOFTWARESCALE_H

extern "C" {
#include <libswscale/swscale.h>
}

// #define WRITE

#ifdef WRITE
std::ofstream image("image.yuv", std::ios::binary);
#endif

bool software_scale(AVFrame* frame, int width, int height)
{
  SwsContext* sws_ctx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, // Source
                                       width, height, AV_PIX_FMT_YUV420P, // Destination
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);

  if (!sws_ctx) {
    std::cerr << "Error initializing sws context for scaling\n";
    return false;
  }

  AVFrame* scaled_frame = av_frame_alloc();
  if (!scaled_frame) {
    std::cerr << "Could not allocate scaled frame\n";
    sws_freeContext(sws_ctx);
    return false;
  }

  // Allocate buffer for scaled frame
  int num_bytes = av_image_alloc(scaled_frame->data, scaled_frame->linesize, width, height, AV_PIX_FMT_YUV420P, 32);
  if (num_bytes < 0) {
    std::cerr << "Could not allocate scaled frame buffer\n";
    av_frame_free(&scaled_frame);
    sws_freeContext(sws_ctx);
    return false;
  }

  scaled_frame->width = width;
  scaled_frame->height = height;
  scaled_frame->format = AV_PIX_FMT_YUV420P;

  // Perform scaling
  sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, scaled_frame->data, scaled_frame->linesize);

#ifdef WRITE
  for (int plane = 0; plane < 3; ++plane) {
    auto height = scaled_frame->height / (scaled_frame->width / scaled_frame->linesize[plane]);
    for (int i = 0; i < height; ++i) {
      image.write(reinterpret_cast<const char*>(scaled_frame->data[plane] + (i * scaled_frame->linesize[plane])),
                  scaled_frame->linesize[plane]);
    }
  }
#endif

  av_freep(&scaled_frame->data[0]);
  av_frame_free(&scaled_frame);
  sws_freeContext(sws_ctx);

  return true;
}

void scale_incompatible_hwframe(AVCodecContext* dec_ctx, AVFrame* hw_frame, int width, int height)
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

  if (!software_scale(cpu_frame, width, height)) {
    std::cerr << "Error scaling frame\n";
  }


  av_frame_free(&cpu_frame);
}

#endif // SOFTWARESCALE_H
