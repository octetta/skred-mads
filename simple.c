#define _XOPEN_SOURCE 500
//#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "skred_ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#define WT_SIZE 4096
#define PI 3.14159265358979323846

int main() {
    ma_engine engine;
    if (ma_engine_init(NULL, &engine) != MA_SUCCESS) return -1;

    ma_uint32 sr = ma_engine_get_sample_rate(&engine);

    // 1. Create a single Sine Wavetable
    float* sine_buffer = malloc(WT_SIZE * sizeof(float));
    for (int i = 0; i < WT_SIZE; i++) {
        sine_buffer[i] = (float)sin(((double)i / WT_SIZE) * 2.0 * PI);
    }

    // 2. Initialize the Voice
    // Base frequency is calculated so that one loop of the table equals 1Hz
    skred_voice_t voice;
    skred_voice_init(sr, sine_buffer, WT_SIZE, sr, (double)sr/WT_SIZE, &voice);
    
    // 3. Connect the Voice to the Graph
    ma_data_source_node voice_node;
    ma_data_source_node_config node_cfg = ma_data_source_node_config_init(&voice);
    ma_data_source_node_init(ma_engine_get_node_graph(&engine), &node_cfg, NULL, &voice_node);
    ma_node_attach_output_bus(&voice_node, 0, ma_engine_get_endpoint(&engine), 0);

    // 4. Trigger Note (A440)
    skred_voice_set_freq(&voice, 440.0f, 0.0f);
    skred_voice_note_on(&voice);

    printf("Playing 440Hz Sine. Press Enter for next...");
    getchar();
    // seed the random number generator
    srand(time(NULL));
    for (int i=0; i<10; i++) {
      // random midi note from 60 to 80
      int note = (rand() % 20) + 60;
      double freq = (440.0 / 32) * pow(2, ((note - 9) / 12.0));
      printf("Playing midi %d (%g Hz)\n", note, freq);
      skred_voice_set_freq(&voice, freq, 125.0);
      usleep(500 * 1000);
    }
    printf("Press Enter for next...");
    getchar();
    printf("Ramp the volume down to -99dB over 2500ms, and freq to 55Hz over 1250ms, then really quit\n");
    skred_voice_set_vol(&voice, -99.0, 2500.0);
    skred_voice_set_freq(&voice, 55.0, 1250.0);
    sleep(2);

    // Cleanup
    ma_engine_uninit(&engine);
    free(sine_buffer);
    return 0;
}
