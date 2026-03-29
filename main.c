#include "miniaudio.h"
#include "skred_ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include "uedit.h"

#define WT_SIZE 4096
#define VMAX 4
#define WMAX 4
#define WZED WMAX
#define PI (3.14159265358979323846)

// Handle CTRL-C to restore terminal from raw mode safely
void handle_sigint(int sig) {
    (void)sig;
    uedit_disable_raw_mode();
    printf("\nExiting Skred...\n");
    exit(0);
}

// Filter Node
typedef struct {
    ma_node_base base;
    double b0, b1, b2, a1, a2;
    double x1[2], x2[2], y1[2], y2[2];
    float cutoff, resonance;
} skred_filter_node;

static void filter_node_process(ma_node* p_node, const float** pp_frames_in, ma_uint32* p_frame_count_in, float** pp_frames_out, ma_uint32* p_frame_count_out) {
    skred_filter_node* p_f = (skred_filter_node*)p_node;
    const float* p_in = pp_frames_in[0];
    float* p_out = pp_frames_out[0];
    (void)p_frame_count_in; 

    for (ma_uint32 i = 0; i < *p_frame_count_out; i++) {
        for (int c = 0; c < 2; c++) {
            // 1e-18 DC offset kills denormals instantly under -ffast-math
            double x0 = p_in[i * 2 + c] + 1e-18; 
            double y0 = p_f->b0 * x0 + p_f->b1 * p_f->x1[c] + p_f->b2 * p_f->x2[c] - p_f->a1 * p_f->y1[c] - p_f->a2 * p_f->y2[c];
            p_f->x2[c] = p_f->x1[c]; p_f->x1[c] = x0;
            p_f->y2[c] = p_f->y1[c]; p_f->y1[c] = y0;
            p_out[i * 2 + c] = (float)y0;
        }
    }
}

static ma_node_vtable g_filter_vtable = { filter_node_process, NULL, 1, 1, 0 };

void update_filter(skred_filter_node* p_f, float sr) {
    /* Clamp resonance to prevent filter blowout */
    if (p_f->resonance < 0.1f) p_f->resonance = 0.1f;
    if (p_f->resonance > 20.0f) p_f->resonance = 20.0f;

    double omega = 2.0 * PI * p_f->cutoff / sr;
    double sn = sin(omega), cs = cos(omega), alpha = sn / (2.0 * p_f->resonance);
    double a0 = 1.0 + alpha;
    p_f->b0 = ((1.0 - cs) / 2.0) / a0;
    p_f->b1 = (1.0 - cs) / a0;
    p_f->b2 = ((1.0 - cs) / 2.0) / a0;
    p_f->a1 = (-2.0 * cs) / a0;
    p_f->a2 = (1.0 - alpha) / a0;
}

// Delay Node
#define DELAY_MAX_SAMPLES 88200 
typedef struct {
    ma_node_base base;
    float* buffer;
    ma_uint32 write_ptr;
    ma_uint32 delay_frames;
    float feedback, mix;
} skred_delay_node;

static void delay_node_process(ma_node* p_node, const float** pp_frames_in, ma_uint32* p_frame_count_in, float** pp_frames_out, ma_uint32* p_frame_count_out) {
    skred_delay_node* p_d = (skred_delay_node*)p_node;
    const float* p_in = pp_frames_in[0];
    float* p_out = pp_frames_out[0];
    (void)p_frame_count_in;

    for (ma_uint32 i = 0; i < *p_frame_count_out; i++) {
        for (int c = 0; c < 2; c++) {
            ma_uint32 read_ptr = (p_d->write_ptr + DELAY_MAX_SAMPLES - p_d->delay_frames) % DELAY_MAX_SAMPLES;
            float delayed = p_d->buffer[read_ptr * 2 + c];
            float input = p_in[i * 2 + c];
            p_d->buffer[p_d->write_ptr * 2 + c] = input + (delayed * p_d->feedback);
            p_out[i * 2 + c] = (input * (1.0f - p_d->mix)) + (delayed * p_d->mix);
        }
        p_d->write_ptr = (p_d->write_ptr + 1) % DELAY_MAX_SAMPLES;
    }
}
static ma_node_vtable g_delay_vtable = { delay_node_process, NULL, 1, 1, 0 };

void usage(void) {
  printf("v <voice#>      : point to a voice (0 to %d)\n", VMAX-1);
  printf("quit            : exit REPL\n");
  printf("on              : perform note-on for voice\n");
  printf("off             : perform note-off\n");
  printf("stop            : stop voice\n");
  printf("trig            : (re)start sample\n");
  printf("wave <#>        : 0=sine 1=sqr 2=saw 3=kick\n");
  printf("freq <hz> <n>   : goto frequency over 'n' msec\n");
  printf("midi <note> <n> : goto midi-note # over 'n' msec\n");
  printf("adsr <attack-ms> <decay-ms> <sustain-level> <release-ms>\n");
  printf("lfo <hz> <FM-depth> <AM-depth> <pan-mod-depth>\n");
  printf("lfo_wave <#>    : same wave has\n");
  printf("route [delay|filter|<blank>]  : choose where voice is routed\n");
  printf("feed <amount>   : (delay route) how much feedback in delay\n");
  printf("mix <amount>    : (delay route) how much mix in delay\n");
  printf("cut <hz>        : (filter route) cut-off frequency\n");
  printf("res <amount>    : (filter route) resonance amount (.707 is 'normal')\n");
}

double midi2hz(double midi_note, double cents) {
    const double reference_pitch = 440.0;
    const float reference_note = 69;
    double fractional_note = (double)midi_note + (cents / 100.0);
    return reference_pitch * pow(2.0, (fractional_note - reference_note) / 12.0);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    ma_engine engine;
    ma_engine_config engineConfig = ma_engine_config_init();
    if (ma_engine_init(&engineConfig, &engine) != MA_SUCCESS) return -1;
    ma_uint32 sr = ma_engine_get_sample_rate(&engine);
    ma_uint32 channels = 2;

    float *wd[WMAX+1];
    ma_uint32 ws[WMAX+1];

    // build simple waveforms
    for (int n = 0; n < 3; n++) {
        wd[n] = malloc(WT_SIZE * sizeof(float));
        ws[n] = WT_SIZE;
        for (int i = 0; i < WT_SIZE; i++) {
            float t = (float)i / WT_SIZE;
            if (n == 0) wd[n][i] = (float)sin(t * 2.0 * PI); // SINE
            else if (n == 1) wd[n][i] = (t < 0.5) ? 1.0f : -1.0f; // SQUARE
            else wd[n][i] = 2.0f * t - 1.0f; // SAW
        }
    }
    // make an "empty" waveform
    wd[WMAX] = calloc(WT_SIZE, sizeof(float)); ws[WMAX] = WT_SIZE;
    
    // build a simple kick drum sample (2 seconds)
    #define DRUM 3
    ma_uint64 drum_frames = sr * 2;
    wd[DRUM] = calloc(drum_frames, sizeof(float));
    ws[DRUM] = (ma_uint32)drum_frames;
    double ph = 0.0;
    for (ma_uint64 i = 0; i < drum_frames; i++) {
        double t = (double)i / sr;
        wd[DRUM][i] = (float)(sin(ph * 2.0 * PI) * exp(-t * 5.0));
        ph += (60.0 + (100.0 * exp(-t * 10.0))) / sr;
    }

    // Effects
    ma_node_config effect_cfg = ma_node_config_init();
    effect_cfg.inputBusCount = 1;
    effect_cfg.outputBusCount = 1;
    effect_cfg.pInputChannels = &channels;
    effect_cfg.pOutputChannels = &channels;

    skred_filter_node f_node = {0};
    effect_cfg.vtable = &g_filter_vtable;
    ma_node_init(ma_engine_get_node_graph(&engine), &effect_cfg, NULL, &f_node);
    f_node.cutoff = 2000.0f;
    f_node.resonance = 0.707f;
    update_filter(&f_node, (float)sr);
    ma_node_attach_output_bus(&f_node, 0, ma_engine_get_endpoint(&engine), 0);

    skred_delay_node d_node = {0};
    d_node.buffer = calloc(DELAY_MAX_SAMPLES * 2, sizeof(float));
    d_node.delay_frames = sr / 2;
    d_node.feedback = 0.5f;
    d_node.mix = 0.3f;
    effect_cfg.vtable = &g_delay_vtable;
    ma_node_init(ma_engine_get_node_graph(&engine), &effect_cfg, NULL, &d_node);
    ma_node_attach_output_bus(&d_node, 0, ma_engine_get_endpoint(&engine), 0);

    // Voices
    skred_voice_t v[VMAX];
    ma_data_source_node v_node[VMAX];
    int wave[VMAX];
    for (int i = 0; i < VMAX; i++) {
        skred_voice_init(sr, wd[WZED], ws[WZED], sr, (double)sr/WT_SIZE, &v[i]);
        ma_data_source_node_config vn_cfg = ma_data_source_node_config_init(&v[i]);
        ma_data_source_node_init(ma_engine_get_node_graph(&engine), &vn_cfg, NULL, &v_node[i]);
        ma_node_attach_output_bus(&v_node[i], 0, ma_engine_get_endpoint(&engine), 0);
        skred_voice_set_adsr(&v[i], 10.0f, 100.0f, 0.7f, 300.0f);
        wave[i] = WZED;
    }

    printf("# skred-mads REPL v0.0.0 (? for commands)\n");
    char line[256], cmd[32];
    float f1[VMAX], f2[VMAX], f3[VMAX], f4[VMAX];
    int tmp, voice = 0;
    while (1) {
        char ps[128];
        sprintf(ps, "v %d (%g,%g,%g,%g))> ", voice, f1[voice], f2[voice], f3[voice], f4[voice]);
        int r = uedit(ps, line, sizeof(line));
        if (r < 0) break;
        if (r == 0) continue;
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (strcmp(cmd, "quit") == 0) break;
        if (strcmp(cmd, "?") == 0) { usage(); continue; }
        if (strcmp(cmd, "v") == 0 && sscanf(line, "%*s %d", &tmp)) { if(tmp>=0 && tmp<VMAX) voice=tmp; }
        else if (strcmp(cmd, "on") == 0) skred_voice_note_on(&v[voice]);
        else if (strcmp(cmd, "off") == 0) skred_voice_note_off(&v[voice]);
        else if (strcmp(cmd, "stop") == 0) skred_voice_stop(&v[voice]);
        else if (strcmp(cmd, "trig") == 0) skred_voice_trig(&v[voice]);
        else if (strcmp(cmd, "wave") == 0 && sscanf(line, "%*s %d", &tmp)) {
            if (tmp>=0 && tmp<=WMAX && tmp!=wave[voice]) {
                wave[voice]=tmp;
                skred_voice_set_sample(&v[voice], wd[tmp], ws[tmp], (tmp==DRUM));
            }
        }
        else if (strcmp(cmd, "freq") == 0 && sscanf(line, "%*s %f %f", &f1[voice], &f2[voice])) skred_voice_set_freq(&v[voice], f1[voice], f2[voice]);
        else if (strcmp(cmd, "midi") == 0 && sscanf(line, "%*s %f %f", &f1[voice], &f2[voice])) skred_voice_set_freq(&v[voice], midi2hz(f1[voice], 0), f2[voice]);
        else if (strcmp(cmd, "vol") == 0 && sscanf(line, "%*s %f %f", &f1[voice], &f2[voice])) skred_voice_set_vol(&v[voice], f1[voice], f2[voice]);
        else if (strcmp(cmd, "pan") == 0 && sscanf(line, "%*s %f %f", &f1[voice], &f2[voice])) skred_voice_set_pan(&v[voice], f1[voice], f2[voice]);
        else if (strcmp(cmd, "dir") == 0 && sscanf(line, "%*s %f %f", &f1[voice], &f2[voice])) skred_voice_set_dir(&v[voice], f1[voice], f2[voice]);
        else if (strcmp(cmd, "adsr") == 0 && sscanf(line, "%*s %f %f %f %f", &f1[voice], &f2[voice], &f3[voice], &f4[voice])) skred_voice_set_adsr(&v[voice], f1[voice], f2[voice], f3[voice], f4[voice]);
        else if (strcmp(cmd, "lfo") == 0 && sscanf(line, "%*s %f %f %f %f", &f1[voice], &f2[voice], &f3[voice], &f4[voice])) skred_voice_set_lfo(&v[voice], f1[voice], f2[voice], f3[voice], f4[voice]);
        else if (strcmp(cmd, "lfo_wave") == 0 && sscanf(line, "%*s %d", &tmp)) { if(tmp>=0 && tmp<WMAX) skred_voice_set_lfo_wave(&v[voice], wd[tmp], ws[tmp]); }
        else if (strcmp(cmd, "cut") == 0 && sscanf(line, "%*s %f", &f1[voice])) {
          f_node.cutoff = f1[voice];
          update_filter(&f_node, (float)sr);
          }
        else if (strcmp(cmd, "res") == 0 && sscanf(line, "%*s %f", &f1[voice])) {
          f_node.resonance = f1[voice];
          update_filter(&f_node, (float)sr);
          }
        else if (strcmp(cmd, "feed") == 0 && sscanf(line, "%*s %f", &f1[voice])) { d_node.feedback = f1[voice]; }
        else if (strcmp(cmd, "mix") == 0 && sscanf(line, "%*s %f", &f1[voice])) { d_node.mix = f1[voice]; }
        else if (strcmp(cmd, "route") == 0) {
            char target[32];
            sscanf(line, "%*s %31s", target);
            ma_node_detach_output_bus(&v_node[voice], 0);
            if (strcmp(target, "filter") == 0) ma_node_attach_output_bus(&v_node[voice], 0, &f_node, 0);
            else if (strcmp(target, "delay") == 0) ma_node_attach_output_bus(&v_node[voice], 0, &d_node, 0);
            else ma_node_attach_output_bus(&v_node[voice], 0, ma_engine_get_endpoint(&engine), 0);
        }
    }
    ma_engine_uninit(&engine);
    for (int i=0; i<=WMAX; i++) free(wd[i]);
    free(d_node.buffer);
    return 0;
}
