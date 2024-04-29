#include "coreaudio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>

// Since we have our own buffer, we should only need 2 buffers for AudioQueue
#define AQ_BUF_COUNT 2
// Anything smaller than this causes distortion
// TODO: Adjust based on input format & target latency
#define AQ_BUF_SIZE 8820 // 44100 * 4 * 50 ms
// Buffer for data we've received from the network
// 100 - 150 ms might be enough
#define SRC_BUF_SIZE 35280 // 44100 * 4 * 200 ms

static struct coreaudio_output_data {
  AudioStreamBasicDescription format;
  AudioQueueRef queue;
  AudioQueueBufferRef buffers[AQ_BUF_COUNT];
  char source_buffer[SRC_BUF_SIZE];
  unsigned int source_buffer_start;
  unsigned int source_buffer_end;
} ca_data;

// Unlike other outputs where we can continuously write data of arbitrary size,
// CoreAudio expects only a few buffers to be used and data to be added into
// those buffers via a callback.
// The "coreaudio_output_send" function coopies the data into the "source buffer"
// which this callback copy it again int the AudioQueue buffer.
// TODO: Deal with thread safety
static void audio_queue_output_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer) {
  if (verbosity) {
    unsigned int data_in_buffer = SRC_BUF_SIZE + ca_data.source_buffer_end - ca_data.source_buffer_start;
    if (data_in_buffer >= SRC_BUF_SIZE) data_in_buffer -= SRC_BUF_SIZE;
    printf("callback; data in buffer: %u (%u, %u)\n", data_in_buffer, ca_data.source_buffer_start, ca_data.source_buffer_end);
  }

  // Copy any audio we have into the buffer
  unsigned int copied_size = 0;
  unsigned int wanted_size = buffer->mAudioDataBytesCapacity;

  // TODO: Refactor to remove duplication
  // Part 1 - current position to the end of the buffer
  {
    unsigned int buffer_end = ca_data.source_buffer_start < ca_data.source_buffer_end ? ca_data.source_buffer_end : SRC_BUF_SIZE;
    unsigned int available_size = buffer_end - ca_data.source_buffer_start;
    unsigned int copy_size = available_size > wanted_size ? wanted_size : available_size;
    memcpy(buffer->mAudioData + copied_size, ca_data.source_buffer + ca_data.source_buffer_start, copy_size);
    ca_data.source_buffer_start += copy_size;
    wanted_size -= copy_size;
    copied_size += copy_size;
    if (verbosity) printf("copy A: %u\n", copy_size);
  }

  // If we're at the end of the ring buffer, reset the read position to the start
  if (ca_data.source_buffer_start == SRC_BUF_SIZE) {
    ca_data.source_buffer_start = 0;
    // But source_buffer_end is at the end as well, it means we have no data left, reset source_buffer_end as well
    if (ca_data.source_buffer_end == SRC_BUF_SIZE) {
      ca_data.source_buffer_end = 0;
    }
  }

  // Part 2 - copy from the beginning of the buffer if we have more data there
  if (wanted_size > 0 && ca_data.source_buffer_start < ca_data.source_buffer_end) {
    unsigned int available_size = ca_data.source_buffer_end - ca_data.source_buffer_start;
    unsigned int copy_size = available_size > wanted_size ? wanted_size : available_size;
    memcpy(buffer->mAudioData + copied_size, ca_data.source_buffer + ca_data.source_buffer_start, copy_size);
    ca_data.source_buffer_start += copy_size;
    wanted_size -= copy_size;
    copied_size += copy_size;
    if (verbosity) printf("copy B: %u\n", copy_size);
  }

  // When we queue a buffer in CoreAudio, it internally sets the timestamp to that after the previous buffer
  // If we don't have enough data, CoreAudio's timestamp will move foward without any data.
  // When we feed it data, it will go into the timeslot that's already passed and won't be played.
  // To prevent that from happening when we don't have enough data, fill the gap with silence.
  if (wanted_size > 0) {
    memset(buffer->mAudioData + copied_size, 0, wanted_size);
    if (verbosity) printf("fill: %u\n", wanted_size);
  }

  buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
  AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);

  if (verbosity) {
    unsigned int data_in_buffer = SRC_BUF_SIZE + ca_data.source_buffer_end - ca_data.source_buffer_start;
    if (data_in_buffer >= SRC_BUF_SIZE) data_in_buffer -= SRC_BUF_SIZE;
    if (verbosity) printf("callback end; data in buffer: %u (%u, %u)\n", data_in_buffer, ca_data.source_buffer_start, ca_data.source_buffer_end);
  }
}

int coreaudio_output_init(unsigned int max_latency_ms)
{
  memset(&ca_data, 0, sizeof(ca_data));
  ca_data.format.mSampleRate = 44100.0;
  ca_data.format.mFormatID = kAudioFormatLinearPCM;
  ca_data.format.mFormatFlags = kAudioFormatFlagIsSignedInteger;
  ca_data.format.mBitsPerChannel = 16;
  ca_data.format.mChannelsPerFrame = 2;
  ca_data.format.mBytesPerPacket = 2 * ca_data.format.mChannelsPerFrame;
  ca_data.format.mFramesPerPacket = 1;
  ca_data.format.mBytesPerFrame = 2 * ca_data.format.mChannelsPerFrame;

  if (AudioQueueNewOutput(&ca_data.format, audio_queue_output_callback, NULL, NULL, NULL, 0, &ca_data.queue) != 0) {
    fprintf(stderr, "Failed to create output\n");
    return 1;
  }

  for (unsigned int i = 0; i < AQ_BUF_COUNT; ++i) {
    if (AudioQueueAllocateBuffer(ca_data.queue, AQ_BUF_SIZE, &ca_data.buffers[i]) != 0) {
      fprintf(stderr, "Failed to allocate buffer\n");
      return 1;
    }
    // This will cause silence to be sent to CoreAudio for AQ_BUF_COUNT * AQ_BUF_SIZE,
    // acting as a buffer waiting for us to have enough data from the network.
    audio_queue_output_callback(NULL, ca_data.queue, ca_data.buffers[i]);
  }

  if (AudioQueueStart(ca_data.queue, NULL) !=0) {
    fprintf(stderr, "Failed to start audio queue\n");
    return 1;
  }

  return 0;
}

int coreaudio_output_send(receiver_data_t *data)
{
  // TODO: Adjust format/channel config based on input

  // Copy data from the network into our buffer
  unsigned int copied_size = 0;
  while (data->audio_size - copied_size > 0) {
    // Note: We ignore the case where source_buffer_end < source_buffer_start and just overwite the data
    // TODO: We should copy only data we have space for and discard the rest
    if (ca_data.source_buffer_end == SRC_BUF_SIZE) {
      ca_data.source_buffer_end = 0;
    }
    unsigned int left_size = data->audio_size - copied_size;
    unsigned int buffer_available = SRC_BUF_SIZE - ca_data.source_buffer_end;
    unsigned int copy_size = left_size < buffer_available ? left_size : buffer_available;
    memcpy(ca_data.source_buffer + ca_data.source_buffer_end, data->audio + copied_size, copy_size);
    copied_size += copy_size;
    ca_data.source_buffer_end += copy_size;
  }

  if (verbosity) {
    unsigned int data_in_buffer = SRC_BUF_SIZE + ca_data.source_buffer_end - ca_data.source_buffer_start;
    if (data_in_buffer >= SRC_BUF_SIZE) data_in_buffer -= SRC_BUF_SIZE;
    printf("received audio: %u, data in buffer: %u (%u, %u)\n", data->audio_size, data_in_buffer, ca_data.source_buffer_start, ca_data.source_buffer_end);
  }

  return 0;
}

