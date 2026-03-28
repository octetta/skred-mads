#include "skred_ds.h"
#include <math.h>
#include <stdlib.h>

#define SKRED_HEADROOM_DB -12.0
#define SKRED_PI 3.14159265358979323846

static double skred_pan_table_l[512];
static double skred_pan_table_r[512];
static int skred_pan_table_initialized = 0;

static void skred_init_pan_table() {
    if (skred_pan_table_initialized) return;
    for (int i = 0; i < 512; i++) {
        double theta = ((double)i / 511.0) * (SKRED_PI * 0.5);
        skred_pan_table_l[i] = cos(theta);
        skred_pan_table_r[i] = sin(theta);
    }
    skred_pan_table_initialized = 1;
}

static double calculate_alpha(float ease_ms, ma_uint32 sample_rate) {
    if (ease_ms <= 1.0f) return 1.0; 
    double time_in_seconds = (double)ease_ms / 1000.0;
    return 1.0 - exp(-4.605 / ((double)sample_rate * time_in_seconds));
}

static double db_to_linear(float db) {
    if (db <= -96.0f) return 0.0;
    return pow(10.0, (db + SKRED_HEADROOM_DB) / 20.0);
}

static ma_result skred_voice_get_data_format(ma_data_source* p_ds, ma_format* pf, ma_uint32* pc, ma_uint32* psr, ma_channel* pcm, size_t mcm) {
    skred_voice_t* p_voice = (skred_voice_t*)p_ds;
    (void)pcm;
    (void)mcm;
    if (pf)  *pf  = ma_format_f32;
    if (pc)  *pc  = 2;
    if (psr) *psr = p_voice->engine_sample_rate;
    return MA_SUCCESS;
}

static ma_result skred_voice_read(ma_data_source* p_ds, void* p_frames_out, ma_uint64 frame_count, ma_uint64* p_frames_read) {
    skred_voice_t* p_voice = (skred_voice_t*)p_ds;
    float* p_out = (float*)p_frames_out;
    ma_uint64 frames_generated = 0;

    if (p_voice->pending_buffer_flag) {
        p_voice->p_buffer = p_voice->pending_buffer;
        p_voice->buffer_frames = p_voice->pending_frames;
        p_voice->loop_start = 0.0;
        p_voice->loop_end = (double)p_voice->pending_frames - 1.0;
        p_voice->loop_mode = p_voice->pending_is_oneshot ? skred_loop_oneshot_t : skred_loop_forward_t;
        p_voice->base_hz = p_voice->pending_is_oneshot ? 1.0 : (double)p_voice->buffer_sample_rate / (double)p_voice->buffer_frames;
        if (p_voice->read_index >= p_voice->loop_end) p_voice->read_index = 0.0;
        p_voice->pending_buffer_flag = 0;
    }

    if (!p_voice->is_playing) {
        if (p_frames_read) *p_frames_read = 0;
        return MA_SUCCESS; 
    }

    double sr_ratio = (double)p_voice->buffer_sample_rate / (double)p_voice->engine_sample_rate;

    for (ma_uint64 i = 0; i < frame_count; ++i) {
        if (p_voice->loop_mode == skred_loop_oneshot_t) {
            if (p_voice->read_index >= p_voice->loop_end || p_voice->read_index < p_voice->loop_start) {
                p_voice->is_playing = 0; break;
            }
        } else {
            while (p_voice->read_index >= p_voice->loop_end) p_voice->read_index -= (p_voice->loop_end - p_voice->loop_start);
            while (p_voice->read_index < p_voice->loop_start) p_voice->read_index += (p_voice->loop_end - p_voice->loop_start);
        }

        double lfo_val = 0.0;
        if (p_voice->p_lfo_buffer) {
            ma_uint32 lfo_idx = (ma_uint32)p_voice->lfo_read_index;
            lfo_val = (double)p_voice->p_lfo_buffer[lfo_idx];
            p_voice->lfo_read_index += (p_voice->lfo_freq * p_voice->lfo_frames) / p_voice->engine_sample_rate;
            while (p_voice->lfo_read_index >= p_voice->lfo_frames) p_voice->lfo_read_index -= p_voice->lfo_frames;
        }

        switch (p_voice->adsr_state) {
            case SKRED_ADSR_ATTACK:
                p_voice->env_val += p_voice->attack_inc;
                if (p_voice->env_val >= 1.0) { p_voice->env_val = 1.0; p_voice->adsr_state = SKRED_ADSR_DECAY; }
                break;
            case SKRED_ADSR_DECAY:
                p_voice->env_val -= p_voice->decay_inc;
                if (p_voice->env_val <= p_voice->sustain_level) { p_voice->env_val = p_voice->sustain_level; p_voice->adsr_state = SKRED_ADSR_SUSTAIN; }
                break;
            case SKRED_ADSR_RELEASE:
                p_voice->env_val -= p_voice->release_inc;
                if (p_voice->env_val <= 0.0) { p_voice->env_val = 0.0; p_voice->adsr_state = SKRED_ADSR_IDLE; p_voice->is_playing = 0; }
                break;
            default: break;
        }

        if (!p_voice->is_playing) break;

        if (p_voice->freq_step != 0.0) {
            p_voice->current_freq += p_voice->freq_step;
            if ((p_voice->freq_step > 0.0 && p_voice->current_freq >= p_voice->target_freq) ||
                (p_voice->freq_step < 0.0 && p_voice->current_freq <= p_voice->target_freq)) {
                p_voice->current_freq = p_voice->target_freq; p_voice->freq_step = 0.0;
            }
        }
        p_voice->current_vol += p_voice->alpha_vol * (p_voice->target_vol - p_voice->current_vol);
        p_voice->current_pan += p_voice->alpha_pan * (p_voice->target_pan - p_voice->current_pan);
        p_voice->current_dir += p_voice->alpha_dir * (p_voice->target_dir - p_voice->current_dir);

        double mod_freq = p_voice->current_freq * (1.0 + lfo_val * p_voice->mod_depth_freq);
        double mod_vol  = p_voice->current_vol * (1.0 + lfo_val * p_voice->mod_depth_vol);
        double mod_pan  = p_voice->current_pan + (lfo_val * p_voice->mod_depth_pan);

        double p_norm = (mod_pan + 1.0) * 0.5;
        int pan_idx = (int)(p_norm * 511.0);
        if (pan_idx < 0) { pan_idx = 0; } 
        if (pan_idx > 511) { pan_idx = 511; }

        ma_uint32 idx0 = (ma_uint32)p_voice->read_index;
        ma_uint32 idx1 = (idx0 + 1 >= (ma_uint32)p_voice->loop_end) ? (ma_uint32)p_voice->loop_start : idx0 + 1;
        double frac = p_voice->read_index - (double)idx0;
        float sample = (float)((1.0 - frac) * p_voice->p_buffer[idx0] + frac * p_voice->p_buffer[idx1]);

        double final_gain = mod_vol * p_voice->env_val;
        p_out[frames_generated * 2]     = sample * (float)(skred_pan_table_l[pan_idx] * final_gain);
        p_out[frames_generated * 2 + 1] = sample * (float)(skred_pan_table_r[pan_idx] * final_gain);
        
        frames_generated++;
        p_voice->read_index += (mod_freq / p_voice->base_hz) * sr_ratio * p_voice->current_dir;
    }

    if (p_frames_read) *p_frames_read = frames_generated;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_skred_voice_vtable = { skred_voice_read, NULL, skred_voice_get_data_format, NULL, NULL, NULL, 0 };

ma_result skred_voice_init(ma_uint32 engine_sample_rate, float* p_buffer, ma_uint32 buffer_frames, ma_uint32 buffer_sample_rate, double base_hz, skred_voice_t* p_voice) {
    skred_init_pan_table();
    ma_data_source_config base_config = ma_data_source_config_init();
    base_config.vtable = &g_skred_voice_vtable;
    ma_data_source_init(&base_config, &p_voice->base);

    p_voice->p_buffer = p_buffer;
    p_voice->buffer_frames = buffer_frames;
    p_voice->buffer_sample_rate = buffer_sample_rate;
    p_voice->engine_sample_rate = engine_sample_rate;
    p_voice->base_hz = base_hz;
    p_voice->pending_buffer_flag = 0;
    p_voice->read_index = 0.0;
    p_voice->loop_start = 0.0;
    p_voice->loop_end = (double)buffer_frames - 1.0;
    p_voice->loop_mode = skred_loop_forward_t;
    p_voice->is_playing = 0;
    p_voice->current_freq = base_hz;
    p_voice->target_freq = base_hz;
    p_voice->freq_step = 0.0;
    p_voice->current_vol = db_to_linear(0.0f);
    p_voice->target_vol = p_voice->current_vol;
    p_voice->alpha_vol = 1.0;
    p_voice->current_pan = 0.0;
    p_voice->target_pan = 0.0;
    p_voice->alpha_pan = 1.0;
    p_voice->current_dir = 1.0;
    p_voice->target_dir = 1.0;
    p_voice->alpha_dir = 1.0;
    p_voice->adsr_state = SKRED_ADSR_IDLE;
    p_voice->env_val = 0.0;
    p_voice->p_lfo_buffer = NULL;
    p_voice->lfo_read_index = 0.0;
    skred_voice_set_adsr(p_voice, 10.0f, 100.0f, 0.8f, 100.0f);
    return MA_SUCCESS;
}

void skred_voice_set_sample(skred_voice_t* p_voice, float* p_new_buffer, ma_uint32 new_frames, int is_oneshot) {
    p_voice->pending_buffer = p_new_buffer;
    p_voice->pending_frames = new_frames;
    p_voice->pending_is_oneshot = is_oneshot;
    p_voice->pending_buffer_flag = 1;
}

void skred_voice_set_freq(skred_voice_t* p_voice, float hz, float ease_ms) {
    p_voice->target_freq = (double)hz;
    if (ease_ms <= 0.0f) { 
        p_voice->current_freq = p_voice->target_freq; 
        p_voice->freq_step = 0.0; 
    } else { 
        p_voice->freq_step = (p_voice->target_freq - p_voice->current_freq) / ((p_voice->engine_sample_rate * ease_ms) / 1000.0); 
    }
}

void skred_voice_set_vol(skred_voice_t* p_voice, float db, float ease_ms) {
    p_voice->target_vol = db_to_linear(db);
    p_voice->alpha_vol = calculate_alpha(ease_ms, p_voice->engine_sample_rate);
}

void skred_voice_set_pan(skred_voice_t* p_voice, float pan, float ease_ms) {
    p_voice->target_pan = (double)pan;
    p_voice->alpha_pan = calculate_alpha(ease_ms, p_voice->engine_sample_rate);
}

void skred_voice_set_dir(skred_voice_t* p_voice, float dir, float ease_ms) {
    p_voice->target_dir = (double)dir;
    p_voice->alpha_dir = calculate_alpha(ease_ms, p_voice->engine_sample_rate);
}

void skred_voice_set_adsr(skred_voice_t* p_voice, float a_ms, float d_ms, float s_level, float r_ms) {
    double sr = (double)p_voice->engine_sample_rate;
    p_voice->attack_inc = (a_ms > 1.0f) ? 1.0 / (sr * (a_ms / 1000.0)) : 1.0;
    p_voice->decay_inc = (d_ms > 1.0f) ? (1.0 - s_level) / (sr * (d_ms / 1000.0)) : 1.0;
    p_voice->sustain_level = (double)s_level;
    p_voice->release_inc = (r_ms > 1.0f) ? 1.0 / (sr * (r_ms / 1000.0)) : 1.0;
}

void skred_voice_set_loop(skred_voice_t* p_voice, double start_idx, double end_idx, skred_loop_mode_t mode) {
    p_voice->loop_start = start_idx;
    p_voice->loop_end = end_idx;
    p_voice->loop_mode = mode;
}

void skred_voice_set_lfo(skred_voice_t* p_voice, float lfo_hz, float df, float dv, float dp) {
    p_voice->lfo_freq = (double)lfo_hz;
    p_voice->mod_depth_freq = (double)df;
    p_voice->mod_depth_vol = (double)dv;
    p_voice->mod_depth_pan = (double)dp;
}

void skred_voice_set_lfo_wave(skred_voice_t* p_voice, float* p_lfo_buffer, ma_uint32 lfo_frames) {
    p_voice->p_lfo_buffer = p_lfo_buffer;
    p_voice->lfo_frames = lfo_frames;
    p_voice->lfo_read_index = 0.0;
}

void skred_voice_note_on(skred_voice_t* p_voice) {
    p_voice->is_playing = 1;
    p_voice->adsr_state = SKRED_ADSR_ATTACK;
    p_voice->env_val = 0.0;
    p_voice->read_index = p_voice->loop_start;
}

void skred_voice_trig(skred_voice_t* p_voice) {
    p_voice->is_playing = 1;
    p_voice->adsr_state = SKRED_ADSR_SUSTAIN; 
    p_voice->env_val = 1.0;
    p_voice->read_index = p_voice->loop_start;
}

void skred_voice_note_off(skred_voice_t* p_voice) {
    if (p_voice->adsr_state != SKRED_ADSR_IDLE) p_voice->adsr_state = SKRED_ADSR_RELEASE;
}

void skred_voice_stop(skred_voice_t* p_voice) {
    p_voice->is_playing = 0;
    p_voice->adsr_state = SKRED_ADSR_IDLE;
    p_voice->env_val = 0.0;
}
