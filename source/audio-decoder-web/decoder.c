/*
 * Audio decoder of the browser player: AC3, E-AC3, MP2 and MP3 frames to 16-bit PCM,
 * one or two channels, with FFmpeg's libavcodec (LGPL 2.1 or later, built without GPL parts).
 * Built to WebAssembly by build.sh; used by packages/ui/src/components/player/audioDecoder.ts.
 */
#include <emscripten/emscripten.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  AVCodecContext *context;
  AVCodecParserContext *parser;
  AVPacket *packet;
  AVFrame *frame;
  int16_t *output;
  int capacity; /* samples per channel the output holds */
  int count;    /* samples per channel in the output */
  int channels; /* of the output: 1 or 2 */
  int rate;
} Decoder;

static float sample_at(const AVFrame *frame, int channel, int index) {
  int channels = frame->ch_layout.nb_channels;
  switch (frame->format) {
  case AV_SAMPLE_FMT_FLTP: return ((const float *)frame->extended_data[channel])[index];
  case AV_SAMPLE_FMT_FLT: return ((const float *)frame->extended_data[0])[index * channels + channel];
  case AV_SAMPLE_FMT_S16P: return ((const int16_t *)frame->extended_data[channel])[index] / 32768.0f;
  case AV_SAMPLE_FMT_S16: return ((const int16_t *)frame->extended_data[0])[index * channels + channel] / 32768.0f;
  case AV_SAMPLE_FMT_S32P: return ((const int32_t *)frame->extended_data[channel])[index] / 2147483648.0f;
  case AV_SAMPLE_FMT_S32: return ((const int32_t *)frame->extended_data[0])[index * channels + channel] / 2147483648.0f;
  default: return 0;
  }
}

/* How much of a channel goes to the left and to the right speaker. */
static void weights(const AVChannelLayout *layout, int channel, float *left, float *right) {
  enum AVChannel name = av_channel_layout_channel_from_index(layout, channel);
  *left = 0;
  *right = 0;
  switch (name) {
  case AV_CHAN_FRONT_LEFT: *left = 1; break;
  case AV_CHAN_FRONT_RIGHT: *right = 1; break;
  case AV_CHAN_FRONT_CENTER: *left = *right = 0.7071f; break;
  case AV_CHAN_LOW_FREQUENCY: break;
  case AV_CHAN_BACK_LEFT:
  case AV_CHAN_SIDE_LEFT:
  case AV_CHAN_FRONT_LEFT_OF_CENTER:
  case AV_CHAN_WIDE_LEFT: *left = 0.7071f; break;
  case AV_CHAN_BACK_RIGHT:
  case AV_CHAN_SIDE_RIGHT:
  case AV_CHAN_FRONT_RIGHT_OF_CENTER:
  case AV_CHAN_WIDE_RIGHT: *right = 0.7071f; break;
  default: *left = *right = 0.5f; break;
  }
}

static int16_t to_pcm(float value) {
  if (value > 1) value = 1;
  if (value < -1) value = -1;
  return (int16_t)(value * 32767.0f);
}

/* Adds the samples of a decoded frame to the output, mixed down to stereo when needed. */
static int keep(Decoder *decoder, const AVFrame *frame) {
  int channels = frame->ch_layout.nb_channels;
  if (channels < 1) return 0;
  int needed = decoder->count + frame->nb_samples;
  if (needed > decoder->capacity) {
    int capacity = needed * 2;
    int16_t *output = realloc(decoder->output, (size_t)capacity * 2 * sizeof(int16_t));
    if (!output) return AVERROR(ENOMEM);
    decoder->output = output;
    decoder->capacity = capacity;
  }
  decoder->rate = frame->sample_rate;
  int16_t *out = decoder->output + decoder->count * decoder->channels;
  for (int i = 0; i < frame->nb_samples; i++) {
    if (decoder->channels == 1) {
      out[i] = to_pcm(sample_at(frame, 0, i));
    } else if (channels == 1) {
      int16_t value = to_pcm(sample_at(frame, 0, i));
      out[i * 2] = out[i * 2 + 1] = value;
    } else if (channels == 2) {
      out[i * 2] = to_pcm(sample_at(frame, 0, i));
      out[i * 2 + 1] = to_pcm(sample_at(frame, 1, i));
    } else {
      float left = 0, right = 0;
      for (int c = 0; c < channels; c++) {
        float l, r;
        weights(&frame->ch_layout, c, &l, &r);
        float value = sample_at(frame, c, i);
        left += l * value;
        right += r * value;
      }
      /* The front ones at full, the others added: keep the sum from clipping. */
      out[i * 2] = to_pcm(left * 0.6f);
      out[i * 2 + 1] = to_pcm(right * 0.6f);
    }
  }
  decoder->count = needed;
  return 0;
}

static int receive(Decoder *decoder) {
  for (;;) {
    int result = avcodec_receive_frame(decoder->context, decoder->frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
    if (result < 0) return result;
    result = keep(decoder, decoder->frame);
    av_frame_unref(decoder->frame);
    if (result < 0) return result;
  }
}

/*
 * A decoder for "ac3", "eac3", "mp2" or "mp3"; `channels` of the output, 1 or 2. With
 * `whole_frames`, every call gives whole frames, as MKV and MP4 files keep them: no parser.
 */
EMSCRIPTEN_KEEPALIVE Decoder *decoder_open(const char *name, int channels, int whole_frames) {
  const char *codec_name = strcmp(name, "mp2") == 0 ? "mp2float"
                           : strcmp(name, "mp3") == 0 ? "mp3float"
                                                      : name;
  const AVCodec *codec = avcodec_find_decoder_by_name(codec_name);
  if (!codec) return NULL;
  Decoder *decoder = calloc(1, sizeof(Decoder));
  if (!decoder) return NULL;
  decoder->channels = channels == 1 ? 1 : 2;
  decoder->context = avcodec_alloc_context3(codec);
  decoder->parser = whole_frames ? NULL : av_parser_init(codec->id);
  decoder->packet = av_packet_alloc();
  decoder->frame = av_frame_alloc();
  if (!decoder->context || !decoder->packet || !decoder->frame ||
      avcodec_open2(decoder->context, codec, NULL) < 0) {
    free(decoder);
    return NULL;
  }
  return decoder;
}

/*
 * Decodes the bytes of one or more frames. Returns the samples per channel now in the output
 * (decoder_output), or a negative number on an error. The output starts empty at each call.
 */
EMSCRIPTEN_KEEPALIVE int decoder_decode(Decoder *decoder, const uint8_t *data, int size) {
  decoder->count = 0;
  while (size > 0) {
    uint8_t *frame_data = NULL;
    int frame_size = 0;
    int used = decoder->parser
                   ? av_parser_parse2(decoder->parser, decoder->context, &frame_data, &frame_size,
                                      data, size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0)
                   : size;
    if (!decoder->parser) {
      frame_data = (uint8_t *)data;
      frame_size = size;
    }
    if (used < 0) return used;
    data += used;
    size -= used;
    if (frame_size > 0) {
      decoder->packet->data = frame_data;
      decoder->packet->size = frame_size;
      int result = avcodec_send_packet(decoder->context, decoder->packet);
      /* A broken frame is left out: the next ones still play. */
      if (result >= 0) result = receive(decoder);
      if (result < 0 && result != AVERROR_INVALIDDATA) return result;
    }
    if (used == 0 && frame_size == 0) break;
  }
  return decoder->count;
}

EMSCRIPTEN_KEEPALIVE int16_t *decoder_output(Decoder *decoder) { return decoder->output; }
EMSCRIPTEN_KEEPALIVE int decoder_channels(Decoder *decoder) { return decoder->channels; }
EMSCRIPTEN_KEEPALIVE int decoder_rate(Decoder *decoder) { return decoder->rate; }

/* Forgets what is held, after a jump. */
EMSCRIPTEN_KEEPALIVE void decoder_reset(Decoder *decoder) {
  avcodec_flush_buffers(decoder->context);
  if (decoder->parser) {
    av_parser_close(decoder->parser);
    decoder->parser = av_parser_init(decoder->context->codec_id);
  }
}

EMSCRIPTEN_KEEPALIVE void decoder_close(Decoder *decoder) {
  if (!decoder) return;
  av_parser_close(decoder->parser);
  avcodec_free_context(&decoder->context);
  av_packet_free(&decoder->packet);
  av_frame_free(&decoder->frame);
  free(decoder->output);
  free(decoder);
}
