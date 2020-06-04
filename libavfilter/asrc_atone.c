/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <fluidsynth.h>
#include <stdlib.h>
#include <unistd.h>

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/lfg.h"
#include "libavutil/random_seed.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"
#include "notedef.h"

typedef struct AtoneContext
{
    const AVClass* class;
    int64_t duration;
    int nb_samples;
    int sample_rate;
    int64_t pts;
    int infinite;

    fluid_settings_t* settings;
    fluid_synth_t* synth;
    fluid_sequencer_t *sequencer1;
    fluid_sequencer_t *sequencer;
    short synth_destination, client_destination;
    unsigned int beat_dur;
    unsigned int beats_pm;
    unsigned int notes_pb;
    unsigned int time_marker;
    unsigned int pattern_size;
    char *sfont;                      ///< soundfont file
    int sfont_id;
    int midi_chan;                   ///< midi channel number
    int velocity;                    ///< velocity of key
    int percussion_velocity;         ///< velocity of key in percussion
    double changerate;              
    
    int *riffs;
    int numriffs;
    int last_note;
    int biv[NPR];
    int framecount;
    char *instrument;
    percussion track;
    char *track_name;
    int numbars;

}AtoneContext;

#define CONTEXT AtoneContext
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#define OPT_GENERIC(name, field, def, min, max, descr, type, deffield, ...) \
    { name, descr, offsetof(CONTEXT, field), AV_OPT_TYPE_ ## type,          \
      { .deffield = def }, min, max, FLAGS, __VA_ARGS__ }

#define OPT_INT(name, field, def, min, max, descr, ...) \
    OPT_GENERIC(name, field, def, min, max, descr, INT, i64, __VA_ARGS__)

#define OPT_DBL(name, field, def, min, max, descr, ...) \
    OPT_GENERIC(name, field, def, min, max, descr, DOUBLE, dbl, __VA_ARGS__)

#define OPT_DUR(name, field, def, min, max, descr, ...) \
    OPT_GENERIC(name, field, def, min, max, descr, DURATION, str, __VA_ARGS__)

#define OPT_STR(name, field, def, min, max, descr, ...) \
    OPT_GENERIC(name, field, def, min, max, descr, STRING, str, __VA_ARGS__)

static const AVOption atone_options[] = {
    OPT_INT("velocity",          velocity,                   80,                                         0, 127,                 "set the velocity of key press",),
    OPT_INT("v",                 velocity,                   80,                                         0, 127,                 "set the velocity of key press",),
    OPT_INT("percussion_velocity",percussion_velocity,       127,                                        0, 127,                 "set the velocity of key press",),
    OPT_INT("sample_rate",       sample_rate,                44100,                                      1, INT_MAX,             "set the sample rate",),
    OPT_INT("r",                 sample_rate,                44100,                                      1, INT_MAX,             "set the sample rate",),
    OPT_DUR("duration",          duration,                   0,                                          0, INT64_MAX,           "set the audio duration",),
    OPT_DUR("d",                 duration,                   0,                                          0, INT64_MAX,           "set the audio duration",),
    OPT_STR("sfont",             sfont,                      "/usr/share/sounds/sf2/FluidR3_GM.sf2",     0, 0,                   "set the soundfont file",),
    OPT_INT("samples_per_frame", nb_samples,                 1024,                                       0, INT_MAX,             "set the number of samples per frame",),
    OPT_INT("MIDI_channel",      midi_chan,                  0,                                          0, 127,                 "set the MIDI Channel",),
    OPT_INT("bpm",               beats_pm,                   100,                                        0, INT_MAX,             "set the beats per minute",),
    OPT_INT("notes_per_beat",    notes_pb,                   4,                                          0, INT_MAX,             "set the notes per beat",),
    OPT_STR("instrument",        instrument,                 "Trumpet",                     0, 0,                   "set the instrument",),
    OPT_STR("percussion",        track_name,                 "Metronome",                                0, 0,                   "set the percussion track",),
    OPT_INT("numbars",           numbars,                    2,                                          0, 8,                   "set the riff bars",),
    {NULL}
};

AVFILTER_DEFINE_CLASS(atone);

static void sequencer_callback(unsigned int time, fluid_event_t *event, fluid_sequencer_t *seq, void* data);
static void instrument_select(int prog_no, unsigned int ticks, AtoneContext* s);
static int find_instrument(const char* instrument);
static void set_percussion_track(AtoneContext *s);

static av_cold int init(AVFilterContext *ctx)
{
    AtoneContext *s = ctx->priv;
    int biv[] = {28, 0, 7, 0, 14, 0, 7, 4};

    /*Initialise the fluidsynth settings object followed by synthesizer*/
    s->settings = new_fluid_settings();
    if (s->settings == NULL){
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth settings");
        return AVERROR_EXTERNAL;
    }
    
    s->synth = new_fluid_synth(s->settings);
    if (s->synth == NULL){
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth synth");
        return AVERROR_EXTERNAL;
    }

    s->sfont_id = fluid_synth_sfload(s->synth, s->sfont, 1);
    if(s->sfont_id == FLUID_FAILED){
        av_log(s, AV_LOG_ERROR, "Loading the Soundfont Failed");
        return AVERROR_EXTERNAL;
    }
    
    if (!(s->riffs = av_malloc(sizeof(riff))))
        return AVERROR(ENOMEM);
    
    s->framecount=0;
    srand(getpid());
    s->sequencer = new_fluid_sequencer2(0);
    /* register the synth with the sequencer */
    s->synth_destination = fluid_sequencer_register_fluidsynth(s->sequencer, s->synth);
    /* register the client name and callback */
    s->client_destination = fluid_sequencer_register_client(s->sequencer, "atone", sequencer_callback, s);

    s->time_marker = fluid_sequencer_get_tick(s->sequencer);
    /*get the beat duration in TICKS     1 quarter note per beat*/
    s->beat_dur = 60000/s->beats_pm;
    /*get change interval in frames/sec*/
    s->changerate = (4*s->beat_dur)*s->sample_rate/s->nb_samples;
    if (s->changerate<1.0)
        s->changerate = 1.0;

    set_percussion_track(s);
    instrument_select(find_instrument(s->instrument), s->time_marker, s);

    s->last_note = 0;
    s->numriffs = sizeof(riff)/(NPR* sizeof(int));
    
    for (int i = 0; i < s->numriffs*NPR ; i++)
        s->riffs[i] = riff[i];
    
    for (int i = 0; i < NPR ; i++)
        s->biv[i] = biv[i];
    
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AtoneContext *s = ctx->priv;

    delete_fluid_sequencer(s->sequencer);
    delete_fluid_synth(s->synth);
    delete_fluid_settings(s->settings);
    av_freep(&s->riffs);
}

static av_cold int config_props(AVFilterLink *outlink)
{
    AtoneContext *s = outlink->src->priv;

    if (s->duration == 0)
        s->infinite = 1;

    s->duration = av_rescale(s->duration, s->sample_rate, AV_TIME_BASE);

    if (s->framecount == INT_MAX)
        s->framecount = 0;

    return 0;
}

static void set_percussion_track(AtoneContext *s){
    int i;

    for (i = 0 ; i < sizeof(percussion_tracks)/sizeof(percussion_tracks[0]) ; i++)
        if (strcmp(percussion_tracks[i], s->track_name) == 0)
            break;
    
    switch (i)
    {
    case 0: s->track = Track_1; break;
    case 1: s->track = Track_2; break;
    case 2: s->track = Track_3; break;
    case 3: s->track = Track_4; break;
    case 4: s->track = Track_5; break;
    case 5: s->track = Track_6; break;
    case 6: s->track = Track_7; break;
    case 7: s->track = Track_8; break;
    case 8: s->track = Track_9; break;
    case 9: s->track = Track_10; break;
    case 10: s->track = Track_11; break;
    default: s->track = Track_12; break;
    }   
}

static int find_instrument(const char* instrument)
{
    for (int i = 0; i < sizeof(GM_instrument_list)/sizeof(GM_instrument_list[0]); i++)
        if (strcmp(GM_instrument_list[i], instrument) == 0)
            return i;   
    
    return 0;
}

static void instrument_select(int prog_no, unsigned int ticks, AtoneContext* s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev,s->synth_destination);
    fluid_event_program_change(ev, s->midi_chan, prog_no);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note on message */
static void schedule_noteon(int chan, short key, unsigned int ticks, int velocity, AtoneContext* s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev,s->synth_destination);
    fluid_event_noteon(ev, chan, key, velocity);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note off message */
static void schedule_noteoff(int chan, short key, unsigned int ticks, AtoneContext* s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_noteoff(ev, chan, key);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a timer event to trigger the callback */
static void schedule_timer_event(AtoneContext* s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->client_destination);
    fluid_event_timer(ev, NULL);
    fluid_sequencer_send_at(s->sequencer, ev, s->time_marker, 1);
    delete_fluid_event(ev);
}

static int pick_riff(AtoneContext* s)
{
    int min, dn, riff, bestriff;

    min = 999;
    for (int i = 2; i >= 0; i--){
        riff = (rand()%RAND_MAX)%s->numriffs;
        if (s->last_note == 0)
            return(riff);
        dn = abs(s->last_note - s->riffs[riff*NPR]);
        if (dn == 0)
            dn = 6;
        if (dn < min){
            bestriff = riff;
            min = dn;
        }
    }
    return bestriff;

}

static int energy_calc(int i, int numbars)
{
    if (3*i < numbars)
        return (100 - (90*i)/numbars);
    else if (3*i > 2*numbars)
        return (40 + (90*i)/numbars);
    return 70;
}

static void play_riff(int riff, int energy, int note_duration, int note_time, AtoneContext* s)
{
    int pnd = 0, next; 
    short pn = 0 ;

    for (int i = 0; i < NPR; i++){
        next = s->riffs[riff*NPR + i];
        if (next != H && next != R && ((energy + s->biv[i]) < rand()%100))
            next = (rand() < RAND_MAX/2)? H : R;
        if (next == H){
            pnd ++;
            continue;
        }
        
        if (pn != R){
            schedule_noteon(s->midi_chan, pn, note_time, s->velocity, s);
            note_time += pnd*note_duration;
            schedule_noteoff(s->midi_chan, pn, note_time, s);
            s->last_note = pn;
        }
        pn = next;
        pnd = 1;
    }

    if (pn != R && pn != H){
        schedule_noteon(s->midi_chan, pn, note_time, s->velocity, s);
        note_time += pnd*note_duration;
        schedule_noteoff(s->midi_chan, pn, note_time, s);
        s->last_note = pn;
    }
           
}

static void play_percussion(AtoneContext *s)
{
    int note_time = s->time_marker;

    for (int i = 0; i < s->track.length; i++)
    {
        /*percussion instruments in channel 10 */ 
        schedule_noteon(9, s->track.note[i].instrument_1, note_time, s->percussion_velocity, s);
        schedule_noteon(9, s->track.note[i].instrument_2, note_time, s->percussion_velocity,s);
        schedule_noteon(9, s->track.note[i].instrument_3, note_time, s->percussion_velocity,s);
        /*Multiply by 4 as quarter note takes 1 beat, Whole note takes 4 beats and so on*/
        note_time += 4*s->beat_dur/s->track.note[i].beat;
        schedule_noteoff(9, s->track.note[i].instrument_1, note_time, s);
        schedule_noteoff(9, s->track.note[i].instrument_2, note_time, s);
        schedule_noteoff(9, s->track.note[i].instrument_3, note_time, s);
    }
}

static void schedule_riff_pattern(AtoneContext* s)
{
    int note_time, note_duration, tempo, rpb, energy, riff;

    note_time = s->time_marker;
    tempo = 1;

    if (tempo > rand()%3)
        tempo--;
    else if(tempo < rand()%3)
        tempo++;
    tempo = tempo%3;
    rpb = 1<<tempo;
    note_duration = 4*s->beat_dur/(NPR*rpb);
    energy = energy_calc((rand()%RAND_MAX)%s->numbars, s->numbars);
    for(int r = 0; r < rpb; r++)
    {
        riff = pick_riff(s);
        play_riff(riff, energy, note_duration, note_time, s);
        
    }  
    
    play_percussion(s);
    s->time_marker += 4*s->beat_dur;   
}

static void sequencer_callback(unsigned int time, fluid_event_t *event, fluid_sequencer_t *seq, void* data)
{
    schedule_timer_event(data);
    schedule_riff_pattern(data);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AtoneContext *s = ctx->priv;
    AVFrame *frame;
    int  nb_samples;

    if (!s->infinite && s->duration <= 0) {
        return AVERROR_EOF;
    } else if (!s->infinite && s->duration < s->nb_samples) {
        nb_samples = s->duration;
    } else {
        nb_samples = s->nb_samples;
    }

    if (!(frame = ff_get_audio_buffer(outlink, nb_samples)))
        return AVERROR(ENOMEM);

    if (s->framecount%((int)s->changerate) == 0){
        schedule_riff_pattern(s);
        schedule_timer_event(s);
    }
    
    fluid_synth_write_float(s->synth, nb_samples, frame->data[0], 0, 2, frame->data[0], 1, 2);
    
    if (!s->infinite)
        s->duration -= nb_samples;

    s->framecount++;
    frame->pts = s->pts;
    s->pts    += nb_samples;
    return ff_filter_frame(outlink, frame);
}

static av_cold int query_formats(AVFilterContext *ctx)
{
    AtoneContext *s = ctx->priv;
    static const int64_t chlayouts[] = { AV_CH_LAYOUT_STEREO, -1 };
    int sample_rates[] = { s->sample_rate, -1 };
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE};
    AVFilterFormats *formats;
    AVFilterChannelLayouts *layouts;
    int ret;

    formats = ff_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    ret = ff_set_common_formats (ctx, formats);
    if (ret < 0)
        return ret;

    layouts = avfilter_make_format64_list(chlayouts);
    if (!layouts)
        return AVERROR(ENOMEM);
    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        return ret;

    formats = ff_make_format_list(sample_rates);
    if (!formats)
        return AVERROR(ENOMEM);
    return ff_set_common_samplerates(ctx, formats);
}

static const AVFilterPad atone_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .request_frame = request_frame,
        .config_props  = config_props,
    },
    { NULL }
};

AVFilter ff_asrc_atone = {
    .name          = "atone",
    .description   = NULL_IF_CONFIG_SMALL("Generate algorithmic riff music."),
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(AtoneContext),
    .inputs        = NULL,
    .outputs       = atone_outputs,
    .priv_class    = &atone_class,
};




