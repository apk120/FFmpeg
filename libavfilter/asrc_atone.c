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
#include "libavutil/common.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"
#include "notedef.h"

typedef struct AtoneContext
{
    const AVClass *class;
    int64_t duration;
    int nb_samples;
    int sample_rate;
    int64_t pts;
    int infinite;

    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_sequencer_t *sequencer;
    short synth_destination, client_destination;
    unsigned int beat_dur;
    unsigned int beats_pm;
    unsigned int time_marker;
    char *sfont;                      ///< soundfont file
    int velocity;                    ///< velocity of key
    int percussion_velocity;         ///< velocity of key in percussion
    double changerate;              
   
    int *riffs;
    int numriffs;
    int last_note;
    int framecount;
    char *instrument;
    percussion track;
    char *track_name;
    int numbars;
    int64_t seed;
    AVLFG r;
    int i;

    char *axiom;
    char *rule1;
    char *rule2;
    char *prevgen;
    char *nextgen;
    lsys *system;
    int generations;
    int lstate;
    int max;

    int ca_cells[32];
    int ca_nextgen[32];
    int *ca_neighbours;
    int *ca_keys;
    int *ca_8keys[8];
    int *ca_ruleset;
    int *note_map;
    int *scale;
    char *ca_boundary;
    int ca_rule;
    int ca_ruletype;
    int height;
    int ca_nsize;
    void (*ca_generate)(int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand);
    char *scale_name;
    int last_bass_note;
    int last_lead_note;
    void (*schedule_pattern)(void *t);
    char *algorithm;
    void (*ca_bass)(void *t);
    void (*ca_chords)(void *t);
    void (*ca_lead)(void *t);
    char *ca_bass_name;
    char *ca_chords_name;
    char *ca_lead_name;
    char *chords_instr;
    char *bass_instr;
    char *lead_instr;
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
    OPT_INT("percussion_velocity",percussion_velocity,       80,                                         0, 127,                 "set the velocity of key press",),
    OPT_INT("sample_rate",       sample_rate,                44100,                                      1, INT_MAX,             "set the sample rate",),
    OPT_INT("r",                 sample_rate,                44100,                                      1, INT_MAX,             "set the sample rate",),
    OPT_DUR("duration",          duration,                   0,                                          0, INT64_MAX,           "set the audio duration",),
    OPT_DUR("d",                 duration,                   0,                                          0, INT64_MAX,           "set the audio duration",),
    OPT_STR("sfont",             sfont,                      "/usr/share/sounds/sf2/FluidR3_GM.sf2",     0, 0,                   "set the soundfont file",),
    OPT_INT("samples_per_frame", nb_samples,                 1024,                                       0, INT_MAX,             "set the number of samples per frame",),
    OPT_INT("bpm",               beats_pm,                   100,                                        0, INT_MAX,             "set the beats per minute",),
    OPT_STR("instrument",        instrument,                 "Acoustic-Grand",                           0, 0,                   "set the instrument",),
    OPT_STR("percussion",        track_name,                 "Metronome",                                0, 0,                   "set the percussion track",),
    OPT_INT("numbars",           numbars,                    2,                                          0, 8,                   "set the riff bars",),
    OPT_STR("axiom",             axiom,                      "{FppFmmX}",                                0, 0,                   "set the axiom for 0L system",),
    OPT_STR("rule1",             rule1,                      "XtoF{ppppFmmmmX}{mmFppp}",                 0, 0,                   "set the rule1 for 0L system",),
    OPT_STR("rule2",             rule2,                      "Fto{ppppFmmmFpppF}",                       0, 0,                   "set the rule2 for 0L system",),
    OPT_INT("gen",               generations,                3,                                          0, INT_MAX,             "set the number of generations for 0L system",),
    OPT_INT("ruletype",          ca_ruletype,                31,                                         0, INT_MAX,             "set the rule type of cellular automaton",),
    OPT_INT("rule",              ca_rule,                    32679,                                      0, INT_MAX,             "set the rule of cellular automaton",),
    OPT_INT("height",            height,                      20,                                        10, 25,                 "set the height of cellular automaton",),
    OPT_STR("boundary",          ca_boundary,                "cyclic",                                   0, 0,                   "set the boundary type of cellular automaton",),
    OPT_STR("scale",             scale_name,                 "C_major",                                  0, 0,                   "set the name of scale",),
    OPT_STR("algo",              algorithm,                  "ca",                                       0, 0,                   "set the name of algorithm",),
    OPT_STR("bass",              ca_bass_name,               "lowest_notes",                             0, 0,                   "set the name of bass algorithm for cellular automaton",),
    OPT_STR("chords",            ca_chords_name,             "eighth",                               0, 0,                   "set the name of chords algorithm for cellular automaton",),
    OPT_STR("lead",              ca_lead_name,               "upper_whole",                               0, 0,                   "set the name of lead algorithm for cellular automaton",),
    OPT_STR("bass_instrument",   bass_instr,                 "Acoustic-Grand",                           0, 0,                   "set the name of bass instrument for cellular automaton",),
    OPT_STR("chords_instrument", chords_instr,               "Acoustic-Grand",                           0, 0,                   "set the name of chords instrument for cellular automaton",),
    OPT_STR("lead_instrument",   lead_instr,                 "Acoustic-Grand",                           0, 0,                   "set the name of lead instrument for cellular automaton",),
    {NULL}
};

AVFILTER_DEFINE_CLASS(atone);

enum {riffnL, ca_bass, ca_lead, ca_chords};

static int get_scale (AtoneContext *s) 
{
    int s_size, x[7];

    switch (s->scale_name[0]) {
    case 'C': x[0] = C3; break;
    case 'D': x[0] = D3; break;
    case 'E': x[0] = E3; break;
    case 'F': x[0] = F3; break;
    case 'G': x[0] = G3; break;
    case 'A': x[0] = A3; break;
    case 'B': x[0] = B3; break;
    default: x[0] = C3; break;
    }

    switch (s->scale_name[1]) {
    case 'b': x[0] -= 1; break;
    case 's': x[0] += 1; break;
    }

    if (strcmp(s->scale_name+2, "major") == 0 || strcmp(s->scale_name+3, "major") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(major_increment); i++)
           x[i+1] = x[i] + major_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "n_minor") == 0 || strcmp(s->scale_name+3,"n_minor") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(natural_minor_increment); i++)
            x[i+1] = x[i] + natural_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "m_minor") == 0 || strcmp(s->scale_name+3,"m_minor") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(melodic_minor_increment); i++)
            x[i+1] = x[i] + melodic_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "h_minor") == 0 || strcmp(s->scale_name+3,"h_minor") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(harmonic_minor_increment); i++)
            x[i+1] = x[i] + harmonic_minor_increment[i];
        s_size = 7;
    }
    else if (strcmp(s->scale_name+2, "p_major") == 0 || strcmp(s->scale_name+3,"p_major") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(major_pentatonic_increment); i++)
            x[i+1] = x[i] + major_pentatonic_increment[i];
        s_size = 5;
    }
    else if (strcmp(s->scale_name+2, "p_minor") == 0 || strcmp(s->scale_name+3,"p_minor") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(minor_pentatonic_increment); i++)
            x[i+1] = x[i] + minor_pentatonic_increment[i];
        s_size = 5;
    }
    else if (strcmp(s->scale_name+2, "blues") == 0 || strcmp(s->scale_name+3,"blues") == 0){
        for (int i = 0; i < FF_ARRAY_ELEMS(blues_increment); i++)
            x[i+1] = x[i] + blues_increment[i];
        s_size = 6;
    }
    else {
        av_log(s, AV_LOG_WARNING, "scale %s "
               "not found! defaulting to a major scale\n", s->scale_name);
        for (int i = 0; i < FF_ARRAY_ELEMS(major_increment); i++)
            s->scale[i+1] = s->scale[i] + major_increment[i];
        s_size = 7;
    } 

    s->scale = av_malloc(s_size * sizeof(int));  
    memcpy(s->scale, x, s_size * sizeof(int));

    return s_size;
}

static int find_instrument(char *instrument, AtoneContext *s)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(GM_instrument_list); i++)
        if (strcmp(GM_instrument_list[i], instrument) == 0)
            return i;   
    
    av_log(s, AV_LOG_WARNING, "instrument %s "
               "not found! defaulting to Acoustic-Grand\n", instrument);
    return 0;
}

static void instrument_select(int prog_no, unsigned int ticks, int channel, AtoneContext *s)
{
    fluid_event_t *ev = new_fluid_event();
    
    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_program_change(ev, channel, prog_no);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note on message */
static void schedule_noteon(int chan, short key, unsigned int ticks, int velocity, AtoneContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_noteon(ev, chan, key, velocity);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a note off message */
static void schedule_noteoff(int chan, short key, unsigned int ticks, AtoneContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->synth_destination);
    fluid_event_noteoff(ev, chan, key);
    fluid_sequencer_send_at(s->sequencer, ev, ticks, 1);
    delete_fluid_event(ev);
}

/* schedule a timer event to trigger the callback */
static void schedule_timer_event(AtoneContext *s)
{
    fluid_event_t *ev = new_fluid_event();

    fluid_event_set_source(ev, -1);
    fluid_event_set_dest(ev, s->client_destination);
    fluid_event_timer(ev, NULL);
    fluid_sequencer_send_at(s->sequencer, ev, s->time_marker, 1);
    delete_fluid_event(ev);
}

/*Determine the closest riff to the previous riff within three tries to make the transition between riffs smoother*/
static int pick_riff(AtoneContext *s)
{
    int min, dn, riff, bestriff = 0;
    unsigned rand = av_lfg_get(&s->r) / 2;
    
    min = 999;
    for (int i = 2; i >= 0; i--) {
        riff = rand % s->numriffs;
        if (s->last_note == 0)
            return(riff);
        dn = abs(s->last_note - s->riffs[riff * NPR]);
        if (dn == 0)
            dn = 6;
        if (dn < min) {
            bestriff = riff;
            min = dn;
        }
    }

    return bestriff;
}

/*Determine the energy of the player which will affect the number of rests and holding tones*/
static int energy_calc(int i, int numbars)
{
    if (3 * i < numbars)
        return (100 - (90 * i)/numbars);
    else if (3 * i > 2 * numbars)
        return (40 + (90 * i)/numbars);
    return 70;
}

static void play_riff(int riff, int energy, int note_duration, int note_time, AtoneContext *s)
{
    int pnd = 0, next; 
    short pn = 0 ;
    /*Beat importance values chosen such that off beat values are more likely to be skipped than on beat*/
    int biv[] = {28, 0, 7, 0, 14, 0, 7, 4};
    unsigned rand; 

    for (int i = 0; i < NPR; i++)
    {
        rand = av_lfg_get(&s->r) / 2;
        next = s->riffs[riff * NPR + i];
        if (next != H && next != R && ((energy + biv[i]) < rand % 100))
            next = (rand < RAND_MAX / 2) ? H : R;
        if (next == H) {
            pnd ++;
            continue;
        }
        
        if (pn != R) {
            schedule_noteon(0, pn, note_time, s->velocity, s);
            note_time += pnd*note_duration;
            schedule_noteoff(0, pn, note_time, s);
            s->last_note = pn;
        }
        pn = next;
        pnd = 1;
    }

    if (pn != R && pn != H) {
        schedule_noteon(0, pn, note_time, s->velocity, s);
        note_time += pnd * note_duration;
        schedule_noteoff(0, pn, note_time, s);
        s->last_note = pn;
    }
           
}

static int find_percussion_track(char *s)
{
    int i;

    for (i = 0 ; i < FF_ARRAY_ELEMS(percussion_tracks) ; i++)
        if (strcmp(percussion_tracks[i], s) == 0)
            break;
    
   return i;   
}

static void play_percussion(AtoneContext *s)
{
    int note_time = s->time_marker;

    switch (s->i) {
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
    
    for (int i = 0; i < s->track.length; i++) {
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

/*Determine the pattern, tempo (to play as 8th, 16th or 32nd notes) and add the riffs to sequencer
Reference: http://peterlangston.com/Papers/amc.pdf */
static void schedule_riff_pattern(void *t)
{
    AtoneContext *s = t;
    int note_time, note_duration, tempo, rpb, energy, riff;
    unsigned rand = av_lfg_get(&s->r) / 2;

    note_time = s->time_marker;
    tempo = 1;
    
    if (tempo > rand % 3)
        tempo--;
    else if (tempo < rand % 3)
        tempo++;
    tempo = tempo % 3;
    rpb = 1 << tempo;
    note_duration = 4 * s->beat_dur / (NPR * rpb);
    energy = energy_calc(rand % s->numbars, s->numbars);
    for  (int r = 0; r < rpb; r++) {
        riff = pick_riff(s);
        play_riff(riff, energy, note_duration, note_time, s);
        
    }  
    
    play_percussion(s);
    s->time_marker += 4 * s->beat_dur;   
}

/*Schedule 0L system pattern: decode symbols as : 
F -> increase note duration by factor of 2
X -> rest note
p -> move up in scale by one note
m -> move down in scale by one note
{ -> push current state
} -> set note state to initial value
Reference : https://link.springer.com/chapter/10.1007%2F978-3-540-32003-6_56
*/
static void schedule_0L_pattern(AtoneContext *s)
{
    int note_state = s->height / 2, dur_state = 1, sys_state = 0, size;
    char c;
    
    for (int i = 0; i < s->generations; i++){
        int j = 0, length = 0;
        char c;

        while (s->prevgen[j] != '\0') {
            c = s->prevgen[j];
            if (c == s->rule1[0]) {
                memcpy(s->nextgen + length, s->rule1 + 3, strlen(s->rule1) - 3);
                length += strlen(s->rule1) - 3;
            }
                
            else if (c == s->rule2[0]) {
                memcpy(s->nextgen + length, s->rule2 + 3, strlen(s->rule2) - 3);
                length += strlen(s->rule2) - 3;
            }
                
            else
            {
                memcpy(s->nextgen+length, s->prevgen+j, 1);
                length +=1;
            }
            j++;
        }
        s->nextgen[strlen(s->nextgen)] = '\0';
        memcpy(s->prevgen, s->nextgen, strlen(s->nextgen) + 1);
        strcpy(s->nextgen, "");
    }

    for (int i = 0 ; i < strlen(s->prevgen) ; i++) {
        c = s->prevgen[i]; 
        switch(c){
            case 'F': dur_state *= 2 ;break;
            case 'p': note_state++; if (note_state >= size) note_state -= size/2; break;
            case 'm': note_state--; if (note_state < 0) note_state += size/2;break;
            case '{': s->system[sys_state].note = s->note_map[note_state]; s->system[sys_state].dur = dur_state; sys_state++; break;
            case '}': note_state = 0; dur_state = 1; break;
            case 'X': s->system[sys_state].note = R; s->system[sys_state].dur = dur_state; sys_state++; break;
        }
    }
    
    s->max = sys_state;
}

static void schedule_L_pattern(void *t)
{
    AtoneContext *s = t;
    int note_time = s->time_marker, sum, state;
    
    sum = 0;
    state = s->lstate; 
    while (sum < 8)
    {
        sum += s->system[state].dur;
        state++;
    }
    
    if (state < s->max)
        for (int i = s->lstate; i < state ; i++)
        {
            if (s->system[i].note == R)
            {
                note_time += 4 * s->beat_dur * s->system[i].dur / 8;
            }
            else
            {
                schedule_noteon(0, s->system[i].note, note_time, s->velocity, s);
                note_time += 4 * s->beat_dur*s->system[i].dur / 8;
                schedule_noteoff(0, s->system[i].note, note_time, s);
            }
            
        }
    s->lstate += state;
    play_percussion(s);
    s->time_marker += 4*s->beat_dur;
}

/*---Cellular Automaton---*/
static void multiple_notes(int note_time, int start, int length, int on, int *notes, AtoneContext *s)
{
    if (on == 1){
        for (int i = start; i < length; i ++)
                schedule_noteon(ca_chords, notes[i], note_time, 2 * s->velocity / 3, s);
    }
    else {
        for (int i = start; i < length; i ++)
                schedule_noteoff(ca_chords, notes[i], note_time, s);
    }
    
}

static void cyclic_generate (int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand)
{
    for (int i = 0; i < 32; i++) {
        int c = 0;

        for (int j = 0; j < size; j++){
            c +=  curr[(i + nbor[j]+ 32) % 32] << j;
        }
        next[i] = ruleset[c];
    }
    
    memcpy(curr, next, 32 * sizeof(int));
    memcpy(keys, &curr[16 - height / 2], height * sizeof(int));
}

/*Keep the ratio of 0 and 1 in cell array of cellular automaton same as in rule to simulate infinite boudary */
static void infinite_generate (int *curr, int *next, int *keys, int *nbor, int *ruleset, int size, int height, AVLFG *rand)
{
    float rp = 0.0;

    for (int i = 0; i < (1 << size); i++)
        rp += ruleset[i] * 1.0 / (1 << size);
    for (int i = 0; i < 32; i++){
        int c = 0;

        for (int j = 0; j < size; j++){
            if ((i + nbor[j]) >= size || (i + nbor[j]) < 0){
                float x = av_lfg_get(rand) * 0.5 / INT_MAX;

                if (x > rp){
                    c += 1 << j;
                }
            }
            else
                c +=  curr[i + nbor[j]] << j;
        }
        
        next[i] = ruleset[c];
    }
    memcpy(curr, next, 32 * sizeof(int));
    memcpy(keys, &curr[16 - height / 2], height * sizeof(int));
}

static void ca_bass_lowest_notes (void *t) 
{
    AtoneContext *s = t;
    int note_time = s->time_marker;

    for (int j = 0; j < 8; j++) {
        int i = 0;

        while (i < s->height / 3) {
            if (s->ca_8keys[j][i] == 1) {
                s->last_bass_note = i;
                break;
            }
            i++;
        }
        schedule_noteon (ca_bass, s->note_map[s->last_bass_note % s->height], note_time, 3 * s->velocity / 4, s);
        note_time += 4 * s->beat_dur / 8;
        schedule_noteoff(ca_bass, s->note_map[s->last_note%s->height], note_time, s);
    }
}

/*Each note obtained is played as a 1/8 note
Random number obtained is % (2 * i + 1) to increase bias towards upper notes*/
static void ca_bass_lower_eighth (void *t) 
{
    AtoneContext *s = t;
    int note_time = s->time_marker, note;

    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = FFMAX(0, s->last_bass_note - 3); i < FFMIN(s->last_bass_note + 3, s->height / 2); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (2 * i + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_bass_note = note;
            schedule_noteon (ca_bass, s->note_map[s->last_bass_note % s->height], note_time, 2 * s->velocity / 3, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(ca_bass, s->note_map[s->last_note%s->height], note_time, s);
        }
    }
}

static void ca_chords_eighth (void *t) 
{
    enum {ON, OFF};
    AtoneContext *s = t;
    int note_time = s->time_marker, note, notes[3];
    
    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = 0; i < s->height; i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_keys[i]) % (2 * i + 1);

            if ((s->ca_8keys[j][i] % s->height) == 1 && (s->ca_8keys[j][i + 2] % s->height) == 1 && (s->ca_8keys[j][i + 4] % s->height) == 1) {
                if (max < rand) {
                    max = rand;
                    note  = i;
                }
            }
        }
        if (max > 0) {
            s->last_note = note;
            for (int i = 0; i < 3; i++)
                notes[i] = s->note_map[(s->last_note + 2 * i) % s->height];
            multiple_notes (note_time, 0, 3, ON, notes, s);
            note_time += 4 * s->beat_dur / 8;
            multiple_notes (note_time, 0, 3, OFF, notes, s);
        }
    }
}

static void ca_chords_whole (void *t) 
{
    enum {ON, OFF};
    AtoneContext *s = t;
    int note_time = s->time_marker, note[8], notes[3], k;
    
    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        note[j] = 0;
        for (int i = 0; i < s->height; i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (2 * i + 1);

            if ((s->ca_8keys[j][i] % s->height) == 1 && (s->ca_8keys[j][i + 2] % s->height) == 1 && (s->ca_8keys[j][i + 4] % s->height) == 1) {
                if (max < rand) {
                    max = rand;
                    k  = i;
                }
            }
        }
        if (max > 0)
            note[j] = k;
    }
    k = 0;
    while (k < 8) {
        int j = 0;
        if (note[k] > 0) {
            s->last_note = note[k];
            for (int i = 0; i < 3; i++)
                notes[i] = s->note_map[(s->last_note + 2 * i) % s->height];
            multiple_notes (note_time, 0, 3, ON, notes, s);
            note_time += 4 * s->beat_dur / 8;
            while ( k + j < 8) {
                if (note[k+j] > 0 && note[k + j] == note[k]) {
                    note_time += 4 * s->beat_dur/8;
                    j++;
                }
                else 
                    break;      
            }
            multiple_notes (note_time, 0, 3, OFF, notes, s);
        }
        k += j + 1;
    }
}

static void ca_lead_upper_whole (void *t) 
{
    AtoneContext *s = t;
    int note_time = s->time_marker, note[8], k;
  
    for (int j = 0; j < 8; j++) {
        unsigned max = 0; 

        note[j] = 0;
        for (int i = FFMAX(s->last_lead_note - 3, s->height/3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * i + 1);

            if (max < rand) {
                max = rand;
                k = i;
            }
        }
        if (max > 0)
            note[j] = k;
    }
    k = 0;
    while(k < 8){
        int j = 0;
        if (note[k] > 0) {
            s->last_lead_note = note[k];
            schedule_noteon (ca_lead, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            while ( k + j < 8) {
                if (note[k+j] > 0 && note[k + j] == note[k]) {
                    note_time += 4 * s->beat_dur / 8;
                    j++;
                }
                else 
                    break;      
            }  
            schedule_noteoff(ca_lead, s->note_map[s->last_lead_note % s->height], note_time, s);
        }
        k += j+1;
    }  
}

static void ca_lead_upper_eighth (void *t) 
{
    AtoneContext *s = t;
    int note_time = s->time_marker, note;
    
    for (int j = 0; j < 8; j++) {
        unsigned max = 0;
        
        for (int i = FFMAX(s->last_lead_note - 3, s->height/3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_keys[i]) % (5 * i + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_lead_note = note;
            schedule_noteon (ca_lead, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(ca_lead, s->note_map[s->last_lead_note % s->height], note_time, s);
        }
    }
}

static void ca_lead_lower_eighth (void *t) 
{
    AtoneContext *s = t;
    int note_time = s->time_marker, note;
    
    
    for (int j = 0; j < 8; j++) {
        unsigned max = 0;

        for (int i = FFMAX(s->last_lead_note - 3, s->height / 3); i < FFMIN(s->last_lead_note + 3, s->height); i++) {
            unsigned rand = (av_lfg_get(&s->r) * s->ca_8keys[j][i]) % (5 * FFABS(s->height - i) + 1);

            if (max < rand) {
                max = rand;
                note = i;
            }
        }
        if (max > 0) {
            s->last_lead_note = note;
            schedule_noteon (ca_lead, s->note_map[s->last_lead_note % s->height], note_time, s->velocity, s);
            note_time += 4 * s->beat_dur / 8;
            schedule_noteoff(ca_lead, s->note_map[s->last_lead_note%s->height], note_time, s);
        }
    }
    
}

static void schedule_ca_pattern(void *t)
{
    AtoneContext *s = t;
    
    for (int i = 0; i < 8; i++)
        s->ca_generate(s->ca_cells, s->ca_nextgen, s->ca_8keys[i], s->ca_neighbours, s->ca_ruleset, s->ca_nsize, s->height, &s->r);
    s->ca_bass(s);
    s->ca_chords(s);
    s->ca_lead(s);
    play_percussion(s);
    s->time_marker += 4 * s->beat_dur;
    
}

static void sequencer_callback(unsigned int time, fluid_event_t *event, fluid_sequencer_t *seq, void *data)
{
    AtoneContext *s = data;

    schedule_timer_event(data);
    s->schedule_pattern(data);
}

static av_cold int init(AVFilterContext *ctx)
{
    AtoneContext *s = ctx->priv;
    int sfont_id, copy, i = 1, j = 1, s_size;
    
    /*Initialise the fluidsynth settings object followed by synthesizer*/
    s->settings = new_fluid_settings();
    if (s->settings == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth settings");
        return AVERROR_EXTERNAL;
    }
    s->synth = new_fluid_synth(s->settings);
    if (s->synth == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to create the fluidsynth synth");
        return AVERROR_EXTERNAL;
    }
    sfont_id = fluid_synth_sfload(s->synth, s->sfont, 1);
    if (sfont_id == FLUID_FAILED) {
        av_log(s, AV_LOG_ERROR, "Loading the Soundfont Failed");
        return AVERROR_EXTERNAL;
    }
    if (!(s->riffs = av_malloc(sizeof(riff))))
        return AVERROR(ENOMEM);
    
    s->prevgen = av_malloc(sizeof(char) * L_MAX_LENGTH);
    s->nextgen = av_malloc(sizeof(char) * L_MAX_LENGTH);
    s->system = av_malloc(sizeof(lsys) * L_MAX_LENGTH);
    strcpy(s->prevgen, s->axiom);

    s->framecount = 0;
    s->sequencer = new_fluid_sequencer2(0);
    /* register the synth with the sequencer */
    s->synth_destination = fluid_sequencer_register_fluidsynth(s->sequencer, s->synth);
    /* register the client name and callback */
    s->client_destination = fluid_sequencer_register_client(s->sequencer, "atone", sequencer_callback, s);
    s->time_marker = fluid_sequencer_get_tick(s->sequencer);
    /*get the beat duration in TICKS     1 quarter note per beat*/
    s->beat_dur = 60000/s->beats_pm;
    /*get change interval in frames/sec*/
    s->changerate = (4 * s->beat_dur) * s->sample_rate / s->nb_samples;
    if (s->changerate < 1.0)
        s->changerate = 1.0;

    s->lstate = 0;
    s->max = 0;
    s->last_note = 0;
    s->last_bass_note = 0;
    s->last_lead_note = s->height / 2;
    s->numriffs = sizeof(riff)/(NPR * sizeof(int));
    s->seed = av_get_random_seed();
    av_lfg_init(&s->r, s->seed);

    for (int i = 0; i < s->numriffs * NPR ; i++)
        s->riffs[i] = riff[i];

    s->ca_nsize = 0;
    copy = s->ca_ruletype;
    while (copy > 0) {
        if (copy % 2 == 1)
            s->ca_nsize++;
        copy = copy >> 1;
    }
    s->ca_neighbours = av_malloc(sizeof(int) * s->ca_nsize);
    s->ca_ruleset = av_malloc(sizeof(int) * (1 << s->ca_nsize));
    s->ca_keys = av_malloc(sizeof(int) * s->height);
    for (int k = 0; k < 8; k++)
        s->ca_8keys[k] = av_malloc(s->height * sizeof(int));
    s->note_map = av_malloc(sizeof(int) * s->height);
    s_size = get_scale(s);
    copy = s->ca_ruletype;
    /*The neighbouring cells on which cells of next generation is determined as
    in http://tones.wolfram.com/about/how-it-works*/
    while (copy > 0) {
        if (copy % 2 == 1){
            if (i % 2 == 0)
                s->ca_neighbours[(s->ca_nsize - 1) / 2 + j / 2] = -1 * (i/2);
            else
                s->ca_neighbours[(s->ca_nsize - 1) / 2 - j / 2] = (i/2);
            j++;
        }
        copy = copy >> 1;
        i++;
    }
    copy = s->ca_rule;
    i = 0;
    while (i != (1 << s->ca_nsize)) {
        s->ca_ruleset[i++] = copy % 2;
        copy = copy >> 1; 
    }

    /*In cellular automaton, the middle portion(s->height) is mapped to a scale 
    The lower and upper octaves are mapped by subtracting and adding 12 respectively*/
    j = s_size/2 - (s->height+1)/4;
    for (i = 0; i < s->height; i++) {
        if (j < 0)
            s->note_map[i] = s->scale[s_size + j % s_size] - 12 * (int)((j * -1.0) / s_size + 1) ;
        else
            s->note_map[i] = s->scale[j % s_size] + 12 * (int)((j * 1.0) / s_size);
        j++;    
    }   
    for (i = 0; i < 32; i++)
        s->ca_cells[i] = av_lfg_get(&s->r) % 2;

    if (strcmp(s->ca_boundary, "infinite") == 0)
        s->ca_generate = infinite_generate;
    else
        s->ca_generate = cyclic_generate;

    if (strcmp(s->ca_bass_name, "lower_part") == 0)
        s->ca_bass = ca_bass_lower_eighth;
    else if (strcmp(s->ca_bass_name, "lowest_notes") == 0)
        s->ca_bass = ca_bass_lowest_notes;
    else {
        av_log(s, AV_LOG_WARNING, "bass algorithm %s "
               "not found! defaulting to a lowest notes\n", s->ca_bass_name);
        s->ca_bass = ca_bass_lowest_notes;    
    }   
    
    if (strcmp(s->ca_chords_name, "whole") == 0)
        s->ca_chords = ca_chords_whole;
    else if (strcmp(s->ca_chords_name, "eighth") == 0)
        s->ca_chords = ca_chords_eighth;
    else {
        av_log(s, AV_LOG_WARNING, "chords algorithm %s "
               "not found! defaulting to a eighth notes\n", s->ca_chords_name);
        s->ca_chords = ca_chords_eighth;
    }

    if (strcmp(s->ca_lead_name, "upper_eighth") == 0)
        s->ca_lead = ca_lead_upper_eighth;
    else if (strcmp(s->ca_lead_name, "lower_eighth") == 0)
        s->ca_lead = ca_lead_lower_eighth;
    else if (strcmp(s->ca_lead_name, "upper_whole") == 0)
        s->ca_lead = ca_lead_upper_whole;
    else {
        av_log(s, AV_LOG_WARNING, "lead algorithm %s "
               "not found! defaulting to a upper eighth notes\n", s->ca_lead_name);
        s->ca_lead = ca_lead_upper_eighth;
    }

    if (strcmp(s->algorithm, "riff") == 0)
        s->schedule_pattern = schedule_riff_pattern;
    else if (strcmp(s->algorithm, "Lsystem") == 0) {
        schedule_0L_pattern(s);
        s->schedule_pattern = schedule_L_pattern;
    }
    else
        s->schedule_pattern = schedule_ca_pattern;
    
    s->i = find_percussion_track(s->track_name);
    instrument_select(find_instrument(s->instrument, s), s->time_marker, riffnL, s);
    instrument_select(find_instrument(s->bass_instr, s), s->time_marker, ca_bass, s);
    instrument_select(find_instrument(s->chords_instr, s), s->time_marker, ca_chords, s);
    instrument_select(find_instrument(s->lead_instr, s), s->time_marker, ca_lead, s);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AtoneContext *s = ctx->priv;

    delete_fluid_sequencer(s->sequencer);
    delete_fluid_synth(s->synth);
    delete_fluid_settings(s->settings);
    av_freep(&s->riffs);
    av_freep(&s->prevgen);
    av_freep(&s->nextgen);
    av_freep(&s->system);
    av_freep(&s->ca_ruleset);
    av_freep(&s->ca_neighbours);
    av_freep(&s->ca_keys);
    for (int k = 0; k < 8; k++)
        av_freep(&s->ca_8keys[k]);
    av_freep(&s->note_map);
    av_freep(&s->scale);
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

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
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

    if (s->framecount%((int)s->changerate) == 0) {
        s->schedule_pattern(s);
        schedule_timer_event(s);
    }
    
    fluid_synth_write_float(s->synth, nb_samples, frame->data[0], 0, 2, frame->data[0], 1, 2);
    
    if (!s->infinite)
        s->duration -= nb_samples;
    
    s->framecount++;
    frame->pts = s->pts;
    s->pts += nb_samples;
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
        .config_props  = config_props,
    },
    { NULL }
};

AVFilter ff_asrc_atone = {
    .name          = "atone",
    .description   = NULL_IF_CONFIG_SMALL("Generate algorithmic music."),
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    .priv_size     = sizeof(AtoneContext),
    .inputs        = NULL,
    .outputs       = atone_outputs,
    .priv_class    = &atone_class,
};


