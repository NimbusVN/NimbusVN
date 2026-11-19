/* -*- C++ -*-
 *
 *  ONScripterLabel_event.cpp - Event handler of ONScripter
 *
 *  Copyright (c) 2001-2009 Ogapee. All rights reserved.
 *  (original ONScripter, of which this is a fork).
 *
 *  ogapee@aqua.dti2.ne.jp
 *
 *  Copyright (c) 2007-2010 "Uncle" Mion Sonozaki
 *
 *  UncleMion@gmail.com
 *
 *  Copyright (c) 2023 Galladite
 *
 *  galladite@yandex.com
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>
 *  or write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// Modified by Mion, November 2009, to update from
// Ogapee's 20091115 release source code.

#include "ONScripterLabel.h"
#include "ScriptParser.h"
#ifdef LINUX
#include <sys/types.h>
#include <sys/wait.h>
#endif
#ifdef WIN32
#include <windows.h>
#endif

#define ONS_TIMER_EVENT    (1)
#define ONS_SOUND_EVENT    (2)
#define ONS_CDAUDIO_EVENT  (3)
#define ONS_SEQMUSIC_EVENT (4)
#define ONS_WAVE_EVENT     (5)
#define ONS_MUSIC_EVENT    (6)
#define ONS_BREAK_EVENT    (7)
#define ONS_ANIM_EVENT     (8)

// This sets up the fade event flag for use in bgm fadeout and fadein.
#define BGM_FADEOUT 0
#define BGM_FADEIN  1
#define ONS_BGMFADE_EVENT    (9)

#define EDIT_MODE_PREFIX "[EDIT MODE]  "
#define EDIT_SELECT_STRING "Music vol (m)  SE vol (s)  Voice vol (v)  Numeric variable (n)  Exit (Esc)"
#define EDIT_VOLUME_STRING "Music vol (m)  SE vol (s)  Voice vol (v)  Exit (Esc)"

static uint32_t timer_id = 0;
static uint32_t break_id = 0;
uint32_t timer_cdaudio_id = 0;
uint32_t anim_timer_id = 0;

uint32_t timer_bgmfade_id = 0;
uint32_t timer_silentmovie_id = 0;

#ifdef MACOSX
uint32_t timer_seqmusic_id = 0;
#endif

void clearTimer(uint32_t &timer_id){
    if (timer_id != 0) {
        ons::remove_timer(timer_id);
        timer_id = 0;
    }
}

/* **************************************** *
 * Timer callbacks
 * **************************************** */
static uint32_t animCallback(uint32_t /*interval*/, void* /*param*/)
{
    clearTimer(anim_timer_id);
    ons::push_user(ONS_ANIM_EVENT);
    return 0;
}

static uint32_t breakCallback(uint32_t /*interval*/, void* /*param*/)
{
    clearTimer(break_id);
    ons::push_user(ONS_BREAK_EVENT);
    return 0;
}

static uint32_t timerCallback(uint32_t /*interval*/, void* /*param*/)
{
    clearTimer(timer_id);
    ons::push_user(ONS_TIMER_EVENT);
    return 0;
}

uint32_t bgmfadeCallback(uint32_t interval, void *param)
{
    ons::push_user(ONS_BGMFADE_EVENT, (param == NULL) ? BGM_FADEOUT : BGM_FADEIN);
    return interval;
}

extern "C" void waveCallback( int channel )
{
    ons::push_user(ONS_WAVE_EVENT, channel);
}

/* Non-looping BGM reached its end: let the script loop/restore it. */
extern "C" void bgmCallback()
{
    ons::push_user(ONS_SOUND_EVENT);
}

/* **************************************** *
 * OS Dependent Input Translation
 * **************************************** */
#ifndef IPODLINUX
struct keychk {
    bool set;
    Uint16 unicode;
    keychk(): set(false), unicode(0) {};
};
static keychk unikey[KEY_LAST+1];
#endif

Keysym ONScripterLabel::transKey(Keysym key, bool isdown)
{
#ifdef IPODLINUX
    switch(key.sym){
      case KEY_m:      key.sym = KEY_UP;      break; /* Menu               */
      case KEY_d:      key.sym = KEY_DOWN;    break; /* Play/Pause         */
      case KEY_f:      key.sym = KEY_RIGHT;   break; /* Fast forward       */
      case KEY_w:      key.sym = KEY_LEFT;    break; /* Rewind             */
      case KEY_RETURN: key.sym = KEY_RETURN;  break; /* Action             */
      case KEY_h:      key.sym = KEY_ESCAPE;  break; /* Hold               */
      case KEY_r:      key.sym = KEY_UNKNOWN; break; /* Wheel clockwise    */
      case KEY_l:      key.sym = KEY_UNKNOWN; break; /* Wheel ctrclockwise */
      default: break;
    }
#else
    //printf("got key: %d (unicode %d)\n", event.key.keysym.sym, event.key.keysym.unicode);

    // check against unicode
    if (isdown) { // unicode field only available for keydown; save for keyup
        unikey[key.sym].unicode = key.unicode;
        unikey[key.sym].set = true;
    }
    else if (unikey[key.sym].set)
        key.unicode = unikey[key.sym].unicode;

    //account for switched-around keys in some layouts
    if ((key.unicode & 0xFF80) == 0){
        // ASCII
        if ((key.unicode >= '0') && (key.unicode <= '9'))
            key.sym = KeyCode(KEY_0 + (int)key.unicode - '0');
        else if ((key.unicode >= 'a') && (key.unicode <= 'z'))
            key.sym = KeyCode(KEY_a + (int)key.unicode - 'a');
        else if ((key.unicode >= 'A') && (key.unicode <= 'Z'))
            key.sym = KeyCode(KEY_a + (int)key.unicode - 'A');
        else if (key.unicode == ',')
            key.sym = KEY_COMMA;
    }
#endif

    return key;
}

void ONScripterLabel::flushEventSub( InputEvent &event )
{
    if ( event.type != EventType::User ) return;

    int code = event.user.code;

    //event related to streaming media
    if ( code == ONS_SOUND_EVENT ){
        if ( music_play_loop_flag ||
             (cd_play_loop_flag && !cdaudio_flag ) ){
            stopBGM( true );
            if (music_file_name)
                playSound(music_file_name, SOUND_OGG_STREAMING|SOUND_MP3, true);
            else
                playCDAudio();
        }
        else{
            stopBGM( false );
        }
    }

// Mion: integrating insani's fadeout code & adding fadein, support for non-mp3 bgm
// Simply sets the volume of the music being played lower and lower until it's 0,
// and until the requisite fadeout time has passed.  [Seung Park, 20060621]
    else if ((code == ONS_BGMFADE_EVENT) &&
             (event.user.code2 == BGM_FADEOUT)){
        Uint32 cur_fade_duration = mp3fadeout_duration;
        if (skip_mode & (SKIP_NORMAL | SKIP_TO_EOP | SKIP_TO_WAIT) ||
            ctrl_pressed_status) {
            cur_fade_duration = 0;
            setCurMusicVolume( 0 );
        }
        Uint32 tmp = ons::ticks_ms() - mp3fade_start;
        if ( tmp < cur_fade_duration ) {
            tmp = cur_fade_duration - tmp;
            tmp *= music_volume;
            tmp /= cur_fade_duration;

            setCurMusicVolume( tmp );
        } else {
            clearTimer( timer_bgmfade_id );

            event_mode &= ~WAIT_TIMER_MODE;
            stopBGM( false );
            //set break event to return to script processing
            clearTimer(break_id);

            ons::push_user(ONS_BREAK_EVENT);
        }
    }

    else if ((code == ONS_BGMFADE_EVENT) &&
             (event.user.code2 == BGM_FADEIN)){
        Uint32 cur_fade_duration = mp3fadein_duration;
        if (skip_mode & (SKIP_NORMAL | SKIP_TO_EOP | SKIP_TO_WAIT) ||
            ctrl_pressed_status) {
            cur_fade_duration = 0;
            setCurMusicVolume( music_volume );
        }
        Uint32 tmp = ons::ticks_ms() - mp3fade_start;
        if ( tmp < cur_fade_duration ) {
            tmp *= music_volume;
            tmp /= cur_fade_duration;

            setCurMusicVolume( tmp );
        } else {
            clearTimer( timer_bgmfade_id );

            event_mode &= ~WAIT_TIMER_MODE;
            //set break event to return to script processing
            clearTimer(break_id);
            ons::push_user(ONS_BREAK_EVENT);
        }
    }

    else if ( code == ONS_CDAUDIO_EVENT ){
        if ( cd_play_loop_flag ){
            stopBGM( true );
            playCDAudio();
        }
        else{
            stopBGM( false );
        }
    }
    else if ( code == ONS_WAVE_EVENT ){ // for processing btntime2 and automode correctly
        int ch = event.user.code2;
        if ( ch >= 0 && ch < ONS_MIX_CHANNELS+ONS_MIX_EXTRA_CHANNELS && wave_sample[ch] ){
            if ( (ch >= ONS_MIX_CHANNELS) || (ch == 0) ||
                 !channel_preloaded[ch] ) {
                //don't free preloaded channels, _except_:
                //always free voice channel, for now - could be
                //messy for bgmdownmode and/or voice-waiting FIXME
                wave_sample[ch] = NULL;
            }
            if (ch == 0) {
                channel_preloaded[ch] = false;
                if (bgmdownmode_flag)
                    setCurMusicVolume( music_volume );
            }
        }
    }
}

void ONScripterLabel::flushEvent()
{
    InputEvent event;
    while( input_.poll( event ) )
        flushEventSub( event );

    /* Async movie frames are driven from the event pump: commands keep
     * running while the movie plays, and every flush gives it a chance to
     * present the frame that is due now. */
    updateAsyncMovie();
}

void ONScripterLabel::updateAsyncMovie()
{
    if (!async_movie || async_movie != static_cast<void *>(&movie_player))
        return;
    if (movie_player.paused())
        return;

    if (!movie_player.update(false)) {
        if (movie_player.status() == MovieStatus::Finished) {
            async_movie = NULL;
            clearMovieSurround();
            clearMovieFrame();
            dirty_rect.fill(screen_width, screen_height);
            /* Restore the scene immediately: without this the last decoded
             * frame stays on screen until the script happens to redraw. */
            flush(refreshMode());
        }
    }
}

void ONScripterLabel::advancePhase( int count )
{
    clearTimer(timer_id);

    resetCursorTime( count );
    timer_id = ons::add_timer( count, timerCallback, NULL );
}

void ONScripterLabel::advanceAnimPhase( int count )
{
    if ( anim_timer_id == NULL ){
        resetRemainingTime(count);
        anim_timer_id = ons::add_timer( count, animCallback, NULL );
    }
}

# if 0
void ONScripterLabel::forceResetAnimTimer()
{
    clearTimer( anim_timer_id );
    animEvent();
}
#endif

void ONScripterLabel::waitEventSub(int count)
{
    if (break_id != NULL){ // already in wait queue
        return;
    }

    //use WAIT_NO_ANIM_MODE to avoid animation refresh (e.g. in effect mode)
    //use count<0 to prevent generating an automatic break event (i.e. run until "done")
    int no_anim = false;
    if (event_mode & (WAIT_INPUT_MODE | WAIT_TEXTBTN_MODE)) {
        if ( (skip_mode & SKIP_NORMAL) || ctrl_pressed_status ||
             (event_mode & WAIT_NO_ANIM_MODE) )
            no_anim = true;
    }
    if (event_mode & (WAIT_TIMER_MODE | WAIT_TEXTOUT_MODE)){
        if (no_anim){
            clearTimer(anim_timer_id);
        } else {
            int duration = proceedCursorAnimation();
            if (duration >= 0){
                advancePhase();
            }

            duration = proceedAnimation();
            if (duration >= 0){
                advanceAnimPhase();
            }
        }

        if (count > 0){
            break_id = ons::add_timer(count, breakCallback, NULL);
        }
    }
    
    if ((count >= 0) && (break_id == 0))
        ons::push_user(ONS_BREAK_EVENT);

    runEventLoop();

    clearTimer(break_id);
}

bool ONScripterLabel::waitEvent( int count )
{
    while(1){
        waitEventSub( count );
        if ( system_menu_mode == SYSTEM_NULL ) break;
        // Only do this if we *actually tried to use the menu*
        // -Galladite 2025-05-09
        if ( rgosub_label && system_menu_mode == SYSTEM_MENU ) {
            system_menu_mode = SYSTEM_NULL;
            char *tmp = script_h.rgosub_wait_pos[script_h.cur_rgosub_wait];
            if(select_release & SELECT_RELEASE_REQUIRED) {
                select_release |= SELECT_RELEASE_RGOSUB;
                // This tells the loop to break and call the gosubReal
                // manually, among other things
                return true;
            } else {
            gosubReal( rgosub_label, tmp, false, clickstr_state,
                       script_h.rgosub_wait_1byte[script_h.cur_rgosub_wait]);
            script_h.cur_rgosub_wait = script_h.num_rgosub_waits = 0;
            return true;
            }
        }
        //if ( executeSystemCall() ) return true;
        int ret = executeSystemCall();
        if      (ret == 1) return true;
        else if (ret == 2) return false;
    }

    return false;
}

void ONScripterLabel::trapHandler()
{
    trap_mode = TRAP_NONE;
    stopCursorAnimation( clickstr_state );
    setCurrentLabel( trap_dest );
}

/* **************************************** *
 * Event handlers
 * **************************************** */
bool ONScripterLabel::mouseMoveEvent( MouseMotionEvent *event )
{
    current_button_state.x = event->x;
    current_button_state.y = event->y;

    if ( event_mode & WAIT_BUTTON_MODE ){
        mouseOverCheck( current_button_state.x, current_button_state.y );
        if (getmouseover_flag &&
            (current_over_button >= getmouseover_min) &&
            (current_over_button <= getmouseover_max)){
            current_button_state.set(current_over_button);
            volatile_button_state.set(current_over_button);
            playClickVoice();
            stopCursorAnimation( clickstr_state );
            return true;
        }
        else if (btnarea_flag &&
                 ( ((btnarea_pos < 0) && (event->y > -btnarea_pos)) ||
                   ((btnarea_pos > 0) && (event->y < btnarea_pos)) )){
            current_button_state.set(-4);
            volatile_button_state.set(-4);
            playClickVoice();
            stopCursorAnimation( clickstr_state );
            return true;
        }

    }
    return false;
}

bool ONScripterLabel::mousePressEvent( MouseButtonEvent *event )
// returns true if should break out of the event loop
{
    if ( variable_edit_mode ) return false;

    if (debug_level > 0 && event->button == BUTTON_RIGHT &&
        event->type == EventType::MouseButtonUp) {
        fprintf(stderr,
                "ons: right-click event_mode=0x%x display_mode=%d rmode=%d\n",
                (unsigned)event_mode, (int)display_mode, rmode_flag ? 1 : 0);
    }
    if (debug_level > 0 && event->button == BUTTON_LEFT &&
        event->type == EventType::MouseButtonUp) {
        fprintf(stderr,
                "ons: left-click (%d,%d) event_mode=0x%x over=%d\n",
                event->x, event->y, (unsigned)event_mode, current_over_button);
    }

    /* A click during an async movie that was started with the "click"
     * attribute stops the movie instead of advancing the script. */
    if ( async_movie && movie_click_flag &&
         event->type == EventType::MouseButtonDown ){
        stopMovie( async_movie );
        async_movie = NULL;
        return false;
    }

    if (event_mode & WAIT_BUTTON_MODE)
        last_keypress = KEYPRESS_NULL;

    //any mousepress clears automode, on the release
    if ( automode_flag ){
        if ( event->type == EventType::MouseButtonUp ){
            automode_flag = false;
            if (getskipoff_flag && (event_mode & WAIT_BUTTON_MODE)){
                current_button_state.set(-61);
                volatile_button_state.set(-61);
                return true;
            }
        }
        return false;
    }

    //trap that mouseclick!
    if ( ((event->button == BUTTON_RIGHT) && (trap_mode & TRAP_RIGHT_CLICK)) ||
         ((event->button == BUTTON_LEFT)  && (trap_mode & TRAP_LEFT_CLICK)) ){
        trapHandler();
        return true;
    }

    current_button_state.reset();
    current_button_state.x = event->x;
    current_button_state.y = event->y;
    current_button_state.down_flag = false;
    if (getskipoff_flag && (skip_mode & SKIP_NORMAL) &&
        (event_mode & WAIT_BUTTON_MODE)){
        skip_mode &= ~SKIP_NORMAL;
        current_button_state.set(-60);
        volatile_button_state.set(-60);
        return true;
    }

    skip_mode &= ~SKIP_NORMAL;

    //right-click
    if ((event->button == BUTTON_RIGHT) &&
        (event->type == EventType::MouseButtonUp) &&
        ( (rmode_flag && (event_mode & WAIT_TEXT_MODE)) ||
          (event_mode & (WAIT_BUTTON_MODE | WAIT_RCLICK_MODE)) )){
        current_button_state.set(-1);
        /* Tell the headless test harness this click was consumed by the
         * script (textbtnwait -> *r_menu); retrying would open the fallback
         * system menu on top of the game's own menu. */
        renderer_.notify_right_click_consumed();

        /* Right click opens the script-defined rmenu.  NScripter games such
         * as CLANNAD display their dialogue through btnwait (WAIT_BUTTON_MODE)
         * rather than clickwait, so restricting this to WAIT_TEXT_MODE left
         * the menu unreachable.  lrclick (WAIT_RCLICK_MODE) still expects the
         * -1 result and is not intercepted.
         *
         * textbtnwait (WAIT_TEXTBTN_MODE) is different: -1 is a script-level
         * signal.  CLANNAD uses it to jump to its own sprite-based *r_menu,
         * so the engine must not swallow it and open the fallback rmenu. */
        if (rmode_flag && !(event_mode & (WAIT_RCLICK_MODE | WAIT_TEXTBTN_MODE)) &&
            system_menu_mode == SYSTEM_NULL &&
            (event_mode & (WAIT_TEXT_MODE | WAIT_BUTTON_MODE))) {
            if (root_rmenu_link.next)
                system_menu_mode = SYSTEM_MENU;
            else
                system_menu_mode = SYSTEM_WINDOWERASE;
        }
    }
#if 0
    // Used for debugging right-click issues - Galladite 2023-2-25
    //    ( (rmode_flag && (event_mode & WAIT_TEXT_MODE)) ||
    //      (event_mode & (WAIT_BUTTON_MODE | WAIT_RCLICK_MODE)) )){
    else if ( (event->button == BUTTON_RIGHT) &&
                (event->type == EventType::MouseButtonUp) ){
        printf("Debug: caught unreceived right-click\n");
        printf("rmode_flag: %d\n", rmode_flag);
        if (event_mode & WAIT_TEXT_MODE)
            printf("event_mode: WAIT_TEXT_MODE\n");
        if (event_mode & WAIT_BUTTON_MODE)
            printf("event_mode: WAIT_BUTTON_MODE\n");
        if (event_mode & WAIT_RCLICK_MODE)
            printf("event_mode: WAIT_RCLICK_MODE\n");

        if (rmode_flag && (event_mode & WAIT_TEXT_MODE))
            printf("Cond 1: success\n");
        else {
            printf("Cond 1: failure - ");
            if (!rmode_flag)
                printf("rmode_flag failed: %d\n", rmode_flag);
            else if (!(event_mode & WAIT_TEXT_MODE))
                printf("event_mode failed\n");
            else
                printf("an unexpected error occurred\n");
        }
        if (event_mode & (WAIT_BUTTON_MODE | WAIT_RCLICK_MODE))
            printf("Cond 2: success\n");
        else
            printf("Cond 2: failure\n");
        printf("\n\n");
        return false;
    }
#endif
    //left-click
    else if ( (event->button == BUTTON_LEFT) &&
              ((event->type == EventType::MouseButtonUp) || btndown_flag) ){
        current_button_state.set(current_over_button);
        if ( event_mode & WAIT_TEXTOUT_MODE) {
            skip_mode |= (SKIP_TO_WAIT | SKIP_TO_EOL);
        }
        skip_effect = true;

        if ( event->type == EventType::MouseButtonDown )
            current_button_state.down_flag = true;
    }
    //middle-click
    else if ( (event->button == BUTTON_MIDDLE) &&
              ((event->type == EventType::MouseButtonUp) || btndown_flag) ){
        if (!getmclick_flag)
            return false;
        current_button_state.set(-70);
        if ( event->type == EventType::MouseButtonDown )
            current_button_state.down_flag = true;
    }
#if 1
    else if ((event->button == BUTTON_WHEELUP) &&
             ((event_mode & WAIT_TEXT_MODE) ||
              (usewheel_flag && (event_mode & WAIT_BUTTON_MODE)) ||
              (system_menu_mode == SYSTEM_LOOKBACK))) {

        current_button_state.set(-2);
        if (event_mode & WAIT_TEXT_MODE)
            system_menu_mode = SYSTEM_LOOKBACK;
    }
    else if ( (event->button == BUTTON_WHEELDOWN) &&
              ((enable_wheeldown_advance_flag && (event_mode & WAIT_TEXT_MODE)) ||
               (usewheel_flag && (event_mode & WAIT_BUTTON_MODE)) ||
               (system_menu_mode == SYSTEM_LOOKBACK) ) ) {

        if (event_mode & WAIT_TEXT_MODE)
            current_button_state.set(0);
        else
            current_button_state.set(-3);
    }
#endif
    else return false;

    if (current_button_state.valid_flag)
        volatile_button_state.set(current_button_state.button);

    if (event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE)){
        if (system_menu_mode == SYSTEM_NULL) playClickVoice();
        stopCursorAnimation( clickstr_state );
        return true;
    } else
        return false;
}

void ONScripterLabel::variableEditMode( KeyEvent *event )
{
    if (event_mode & WAIT_BUTTON_MODE)
        last_keypress = KEYPRESS_NULL;

    int  i;
    const char* var_name;
    char var_index[12];

    switch ( event->keysym.sym ) {
      case KEY_m:
        if ( (variable_edit_mode != EDIT_SELECT_MODE) &&
             (variable_edit_mode != EDIT_VOLUME_MODE) )
            return;
        variable_edit_mode = EDIT_MP3_VOLUME_MODE;
        variable_edit_num = music_volume;
        break;

      case KEY_s:
        if ( (variable_edit_mode != EDIT_SELECT_MODE) &&
             (variable_edit_mode != EDIT_VOLUME_MODE) )
            return;
        variable_edit_mode = EDIT_SE_VOLUME_MODE;
        variable_edit_num = se_volume;
        break;

      case KEY_v:
        if ( (variable_edit_mode != EDIT_SELECT_MODE) &&
             (variable_edit_mode != EDIT_VOLUME_MODE) )
            return;
        variable_edit_mode = EDIT_VOICE_VOLUME_MODE;
        variable_edit_num = voice_volume;
        break;

      case KEY_n:
        if ( variable_edit_mode != EDIT_SELECT_MODE ) return;
        variable_edit_mode = EDIT_VARIABLE_INDEX_MODE;
        variable_edit_num = 0;
        break;

      case KEY_9: case KEY_KP9: variable_edit_num = variable_edit_num * 10 + 9; break;
      case KEY_8: case KEY_KP8: variable_edit_num = variable_edit_num * 10 + 8; break;
      case KEY_7: case KEY_KP7: variable_edit_num = variable_edit_num * 10 + 7; break;
      case KEY_6: case KEY_KP6: variable_edit_num = variable_edit_num * 10 + 6; break;
      case KEY_5: case KEY_KP5: variable_edit_num = variable_edit_num * 10 + 5; break;
      case KEY_4: case KEY_KP4: variable_edit_num = variable_edit_num * 10 + 4; break;
      case KEY_3: case KEY_KP3: variable_edit_num = variable_edit_num * 10 + 3; break;
      case KEY_2: case KEY_KP2: variable_edit_num = variable_edit_num * 10 + 2; break;
      case KEY_1: case KEY_KP1: variable_edit_num = variable_edit_num * 10 + 1; break;
      case KEY_0: case KEY_KP0: variable_edit_num = variable_edit_num * 10 + 0; break;

      case KEY_MINUS: case KEY_KP_MINUS:
        if ( (variable_edit_mode == EDIT_VARIABLE_NUM_MODE) &&
             (variable_edit_num == 0) )
            variable_edit_sign = -1;
        break;

      case KEY_BACKSPACE:
        if ( variable_edit_num ) variable_edit_num /= 10;
        else if ( variable_edit_sign == -1 ) variable_edit_sign = 1;
        break;

      case KEY_RETURN: case KEY_KP_ENTER:
        switch( variable_edit_mode ){

          case EDIT_VARIABLE_INDEX_MODE:
            variable_edit_index = variable_edit_num;
            variable_edit_num = script_h.getVariableData(variable_edit_index).num;
            if ( variable_edit_num < 0 ){
                variable_edit_num = -variable_edit_num;
                variable_edit_sign = -1;
            }
            else{
                variable_edit_sign = 1;
            }
            break;

          case EDIT_VARIABLE_NUM_MODE:
            script_h.setNumVariable( variable_edit_index, variable_edit_sign * variable_edit_num );
            break;

          case EDIT_MP3_VOLUME_MODE:
            music_volume = variable_edit_num;
            setCurMusicVolume(music_volume);
            break;

          case EDIT_SE_VOLUME_MODE:
            se_volume = variable_edit_num;
            audio_engine.set_master_volume( !volume_on_flag ? 0.0f : se_volume / 100.0f );
            break;

          case EDIT_VOICE_VOLUME_MODE:
            voice_volume = variable_edit_num;

          default:
            break;
        }
        if ( variable_edit_mode == EDIT_VARIABLE_INDEX_MODE )
            variable_edit_mode = EDIT_VARIABLE_NUM_MODE;
        else if (edit_flag)
            variable_edit_mode = EDIT_SELECT_MODE;
        else
            variable_edit_mode = EDIT_VOLUME_MODE;
        break;

      case KEY_ESCAPE:
        if ( (variable_edit_mode == EDIT_SELECT_MODE) ||
             (variable_edit_mode == EDIT_VOLUME_MODE) ){
            variable_edit_mode = NOT_EDIT_MODE;
            renderer_.set_caption( DEFAULT_WM_TITLE );
            ons::sleep_ms(100);
            renderer_.set_caption( wm_title_string );
            return;
        }
        if (edit_flag)
            variable_edit_mode = EDIT_SELECT_MODE;
        else
            variable_edit_mode = EDIT_VOLUME_MODE;

      default:
        break;
    }

    if ( variable_edit_mode == EDIT_SELECT_MODE ){
        sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_SELECT_STRING );
    }
    else if ( variable_edit_mode == EDIT_VOLUME_MODE ){
        sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_VOLUME_STRING );
    }
    else if ( variable_edit_mode == EDIT_VARIABLE_INDEX_MODE ) {
        sprintf( wm_edit_string, "%s%s%d", EDIT_MODE_PREFIX, "Variable Index?  %", variable_edit_sign * variable_edit_num );
    }
    else if ( variable_edit_mode >= EDIT_VARIABLE_NUM_MODE ){
        int p=0;

        switch( variable_edit_mode ){

          case EDIT_VARIABLE_NUM_MODE:
            sprintf( var_index, "%%%d", variable_edit_index );
            var_name = var_index; p = script_h.getVariableData(variable_edit_index).num; break;

          case EDIT_MP3_VOLUME_MODE:
            var_name = "MP3 Volume"; p = music_volume; break;

          case EDIT_VOICE_VOLUME_MODE:
            var_name = "Voice Volume"; p = voice_volume; break;

          case EDIT_SE_VOLUME_MODE:
            var_name = "Sound effect Volume"; p = se_volume; break;

          default:
            var_name = "";
        }
        sprintf( wm_edit_string, "%sCurrent %s=%d  New value? %s%d",
                 EDIT_MODE_PREFIX, var_name, p, (variable_edit_sign==1)?"":"-", variable_edit_num );
    }

    renderer_.set_caption( wm_edit_string );
}

void ONScripterLabel::shiftCursorOnButton( int diff )
//moves the mouse cursor to the new button "moused-over" via keystroke
{
    int num = 0;
    ButtonLink *button = root_button_link.next;
    while (button) {
        button = button->next;
        ++num;
    }

    shortcut_mouse_line += diff;
    if      (shortcut_mouse_line < 0)    shortcut_mouse_line = num - 1;
    else if (shortcut_mouse_line >= num) shortcut_mouse_line = 0;
    
    button = root_button_link.next;
    for (int i = 0; i < shortcut_mouse_line; ++i)
        button = button->next;

    if (button) {
        Rect clip = {0, 0, button->select_rect.w, button->select_rect.h};
        int x = button->select_rect.x;
        int y = button->select_rect.y;
        if (x < 0) clip.x -= x;
        else if (x > screen_width){
            clip.w = 0;
            x = screen_width - 1;
        }
        else if (x+clip.w > screen_width) clip.w = screen_width - x;
        if (y < 0) clip.y -= y;
        else if (x > screen_width){
            clip.h = 0;
            y = screen_height - 1;
        }
        else if (y+clip.h > screen_height) clip.h = screen_height - y;
        if (transbtn_flag && (clip.x < (Sint16) clip.w) && (clip.y < (Sint16) clip.h)){
            AnimationInfo *anim = NULL;
            if ( button->button_type == ButtonLink::SPRITE_BUTTON ||
                 button->button_type == ButtonLink::EX_SPRITE_BUTTON )
                anim = &sprite_info[ button->sprite_no ];
            else
                anim = button->anim[0];
            Rect pos = anim->findOpaquePoint(&clip);
            x += pos.x;
            y += pos.y;
        } else {
            x += clip.x;
            y += clip.y;
        }
        renderer_.warp_mouse(x, y);
    }
}

bool ONScripterLabel::keyDownEvent( KeyEvent *event )
// returns true if should break out of the event loop
{
    if (event_mode & WAIT_BUTTON_MODE)
        last_keypress = event->keysym.sym;

    int last_ctrl_status = ctrl_pressed_status;

    switch ( event->keysym.sym ) {
      case KEY_RCTRL:
        ctrl_pressed_status  |= 0x01;
        // fall through
      case KEY_LCTRL:
        if (event->keysym.sym == KEY_LCTRL)
            ctrl_pressed_status  |= 0x02;
        if (last_ctrl_status != ctrl_pressed_status)
            skip_effect = true; // allow short-circuiting the current effect with ctrl
        //Ctrl key: do skip in text
        if (event_mode & (WAIT_INPUT_MODE | WAIT_TEXTOUT_MODE | WAIT_TEXTBTN_MODE)){
            current_button_state.set(0);
            volatile_button_state.set(0);
            playClickVoice();
            stopCursorAnimation( clickstr_state );
            return true;
        }
        if (event_mode & (WAIT_SLEEP_MODE)){
            stopCursorAnimation( clickstr_state );
            return true;
        }
        break;
      case KEY_RSHIFT:
        shift_pressed_status |= 0x01;
        break;
      case KEY_LSHIFT:
        shift_pressed_status |= 0x02;
        break;
#ifdef MACOSX
      case KEY_LMETA:
        apple_pressed_status |= 1;
        break;
      case KEY_RMETA:
        apple_pressed_status |= 1;
        break;
#endif
      default:
        break;
    }

    return false;
}

void ONScripterLabel::keyUpEvent( KeyEvent *event )
{
    if (event_mode & WAIT_BUTTON_MODE)
        last_keypress = event->keysym.sym;

    switch ( event->keysym.sym ) {
      case KEY_RCTRL:
        ctrl_pressed_status  &= ~0x01;
        break;
      case KEY_LCTRL:
        ctrl_pressed_status  &= ~0x02;
        break;
      case KEY_RSHIFT:
        shift_pressed_status &= ~0x01;
        break;
      case KEY_LSHIFT:
        shift_pressed_status &= ~0x02;
        break;
#ifdef MACOSX
      case KEY_LMETA:
        apple_pressed_status &= ~1;
        break;
      case KEY_RMETA:
        apple_pressed_status &= ~2;
        break;
#endif
      default:
        break;
    }
}

bool ONScripterLabel::keyPressEvent( KeyEvent *event )
// returns true if should break out of the event loop
{
    //reset the button state
    current_button_state.reset();
    current_button_state.down_flag = false;

    //any keypress clears automode, on the keyup
    if ( automode_flag ){
        if ( event->type == EventType::KeyUp ){
            automode_flag = false;
            if (getskipoff_flag && (event_mode & WAIT_BUTTON_MODE)){
                current_button_state.set(-61);
                volatile_button_state.set(-61);
                return true;
            }
        }
        return false;
    }

    if ( event->type == EventType::KeyUp ){
        if ( variable_edit_mode ){
            variableEditMode( event );
            return false;
        }

        //'m' is for mute (toggle)
        if ( (event->keysym.sym == KEY_m) && !ctrl_pressed_status ){
            volume_on_flag = !volume_on_flag;
            setVolumeMute(!volume_on_flag);
            printf("turned %s volume mute\n", !volume_on_flag?"on":"off");
        }

        //'v' is for entering Volume Edit Mode
        if ( (event->keysym.sym == KEY_v) && !ctrl_pressed_status ){
            // set to windowed mode, so that the caption-based edit menu shows
            menu_windowCommand();
            variable_edit_mode = EDIT_VOLUME_MODE;
            variable_edit_sign = 1;
            variable_edit_num = 0;
            sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_VOLUME_STRING );
            renderer_.set_caption( wm_edit_string );
        }

        //'z' is for entering Edit Mode (if enabled)
        if ( edit_flag && (event->keysym.sym == KEY_z) && !ctrl_pressed_status ){
            // set to windowed mode, so that the caption-based edit menu shows
            menu_windowCommand();
            variable_edit_mode = EDIT_SELECT_MODE;
            variable_edit_sign = 1;
            variable_edit_num = 0;
            sprintf( wm_edit_string, "%s%s", EDIT_MODE_PREFIX, EDIT_SELECT_STRING );
            renderer_.set_caption( wm_edit_string );
        }
    }

    //'s', Return, Enter, or Space will clear (regular) skip mode
    if ((event->type == EventType::KeyUp) &&
        ( (event->keysym.sym == KEY_RETURN) ||
          (event->keysym.sym == KEY_KP_ENTER) ||
          (event->keysym.sym == KEY_SPACE) ||
          (event->keysym.sym == KEY_s) )){
        if (getskipoff_flag && (skip_mode & SKIP_NORMAL) &&
            (event_mode & WAIT_BUTTON_MODE)){
            skip_mode &= ~SKIP_NORMAL;
            current_button_state.set(-60);
            volatile_button_state.set(-60);
            return true;
        }
        skip_mode &= ~SKIP_NORMAL;
    }

    //Shift-'q' is for Quit
    if (((shift_pressed_status && (event->keysym.sym == KEY_q)) 
#ifdef MACOSX
        || (apple_pressed_status && (event->keysym.sym == KEY_q)) 
#endif
       ) && (current_mode == NORMAL_MODE)) {

        endCommand();
    }

    //trap that 'left-click'!
    if ((trap_mode & TRAP_LEFT_CLICK) &&
        ( (event->keysym.sym == KEY_RETURN) ||
          (event->keysym.sym == KEY_KP_ENTER) ||
          (event->keysym.sym == KEY_SPACE) )){
        trapHandler();
        return true;
    }
    //trap that 'right-click'!
    else if ((trap_mode & TRAP_RIGHT_CLICK) &&
             (event->keysym.sym == KEY_ESCAPE)){
        trapHandler();
        return true;
    }

    //so many ways to 'left-click' a button
    if ((event_mode & WAIT_BUTTON_MODE) &&
        ( (((event->type == EventType::KeyUp) || btndown_flag) &&
           ( (!getenter_flag && (event->keysym.sym == KEY_RETURN)) ||
             (!getenter_flag && (event->keysym.sym == KEY_KP_ENTER)) )) ||
          (( spclclk_flag || !useescspc_flag ) &&
           (event->keysym.sym == KEY_SPACE)) )){
        if ( (event->keysym.sym == KEY_RETURN) ||
             (event->keysym.sym == KEY_KP_ENTER) ||
             (spclclk_flag && (event->keysym.sym == KEY_SPACE)) ){
            current_button_state.set(current_over_button);
            volatile_button_state.set(current_over_button);
            if ( event->type == EventType::KeyDown )
                current_button_state.down_flag = true;
        }
        else{
            current_button_state.set(0);
            volatile_button_state.set(0);
        }
        skip_effect = true;
        playClickVoice();
        stopCursorAnimation( clickstr_state );
        return true;
    }

    if ( event->type == EventType::KeyDown ) return false;

    if ((event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE)) &&
        ( (autoclick_time == 0) || (event_mode & WAIT_BUTTON_MODE) )){
        //Esc is for 'right-click' (sometimes)
        if (!useescspc_flag && (event->keysym.sym == KEY_ESCAPE)){
            current_button_state.set(-1);
            if (rmode_flag && (event_mode & WAIT_TEXT_MODE)){
                // Behaviour only emerged after the fix to
                // ONScripterLabel::waitEvent()
                // See also the behaviour of the "escape key."
                // -Galladite 2025-05-09
                /*
                if (root_rmenu_link.next)
                    system_menu_mode = SYSTEM_MENU;
                else
                    system_menu_mode = SYSTEM_WINDOWERASE;
                */
                system_menu_mode = SYSTEM_MENU;
            }
        }
        else if (useescspc_flag && (event->keysym.sym == KEY_ESCAPE)){
            current_button_state.set(-10);
        }
        else if (!spclclk_flag && useescspc_flag && (event->keysym.sym == KEY_SPACE)){
            current_button_state.set(-11);
        }
        //'h' or left-arrow for page-up
        else if (( (!getcursor_flag && (event->keysym.sym == KEY_LEFT)) ||
                   (event->keysym.sym == KEY_h) ) &&
                 ( (event_mode & WAIT_TEXT_MODE) ||
                   (usewheel_flag && !getcursor_flag &&
                    (event_mode & WAIT_BUTTON_MODE)) || 
                   (system_menu_mode == SYSTEM_LOOKBACK) )){
            current_button_state.set(-2);
            if (event_mode & WAIT_TEXT_MODE) system_menu_mode = SYSTEM_LOOKBACK;
        }
        //'l' or right-arrow for page-down
        else if (( (!getcursor_flag && (event->keysym.sym == KEY_RIGHT)) ||
                   (event->keysym.sym == KEY_l) ) &&
                 ( (enable_wheeldown_advance_flag && 
                    (event_mode & WAIT_TEXT_MODE)) ||
                   (usewheel_flag && (event_mode & WAIT_BUTTON_MODE)) ||
                   (system_menu_mode == SYSTEM_LOOKBACK) )){
            if (event_mode & WAIT_TEXT_MODE){
                current_button_state.set(0);
            }
            else{
                current_button_state.set(-3);
            }
        }
        //'k', 'p', or up-arrow for shift to mouseover next button
        else if (( (!getcursor_flag && (event->keysym.sym == KEY_UP)) ||
                   (event->keysym.sym == KEY_k) ||
                   (event->keysym.sym == KEY_p) ) &&
                 (event_mode & WAIT_BUTTON_MODE)){
            shiftCursorOnButton(1);
            return false;
        }
        //'j', 'n', or down-arrow for shift to mouseover previous button
        else if (( (!getcursor_flag && (event->keysym.sym == KEY_DOWN)) ||
                   (event->keysym.sym == KEY_j) ||
                   (event->keysym.sym == KEY_n)) &&
                 (event_mode & WAIT_BUTTON_MODE)){
            shiftCursorOnButton(-1);
            return false;
        }
        else if (getpageup_flag && (event->keysym.sym == KEY_PAGEUP)){
            current_button_state.set(-12);
        }
        else if (getpagedown_flag && (event->keysym.sym == KEY_PAGEDOWN)){
            current_button_state.set(-13);
        }
        else if ( (getenter_flag && (event->keysym.sym == KEY_RETURN)) ||
                  (getenter_flag && (event->keysym.sym == KEY_KP_ENTER)) ){
            current_button_state.set(-19);
        }
        else if (gettab_flag && (event->keysym.sym == KEY_TAB)){
            current_button_state.set(-20);
        }
        else if (getcursor_flag && (event->keysym.sym == KEY_UP)){
            current_button_state.set(-40);
        }
        else if (getcursor_flag && (event->keysym.sym == KEY_RIGHT)){
            current_button_state.set(-41);
        }
        else if (getcursor_flag && (event->keysym.sym == KEY_DOWN)){
            current_button_state.set(-42);
        }
        else if (getcursor_flag && (event->keysym.sym == KEY_LEFT)){
            current_button_state.set(-43);
        }
        else if (getinsert_flag && (event->keysym.sym == KEY_INSERT)){
            current_button_state.set(-50);
        }
        else if (getzxc_flag && (event->keysym.sym == KEY_z)){
            current_button_state.set(-51);
        }
        else if (getzxc_flag && (event->keysym.sym == KEY_x)){
            current_button_state.set(-52);
        }
        else if (getzxc_flag && (event->keysym.sym == KEY_c)){
            current_button_state.set(-53);
        }
        else if ( getfunction_flag ){
            if      ( event->keysym.sym == KEY_F1 )
                current_button_state.set(-21);
            else if ( event->keysym.sym == KEY_F2 )
                current_button_state.set(-22);
            else if ( event->keysym.sym == KEY_F3 )
                current_button_state.set(-23);
            else if ( event->keysym.sym == KEY_F4 )
                current_button_state.set(-24);
            else if ( event->keysym.sym == KEY_F5 )
                current_button_state.set(-25);
            else if ( event->keysym.sym == KEY_F6 )
                current_button_state.set(-26);
            else if ( event->keysym.sym == KEY_F7 )
                current_button_state.set(-27);
            else if ( event->keysym.sym == KEY_F8 )
                current_button_state.set(-28);
            else if ( event->keysym.sym == KEY_F9 )
                current_button_state.set(-29);
            else if ( event->keysym.sym == KEY_F10 )
                current_button_state.set(-30);
            else if ( event->keysym.sym == KEY_F11 )
                current_button_state.set(-31);
            else if ( event->keysym.sym == KEY_F12 )
                current_button_state.set(-32);
        }
        if ( current_button_state.valid_flag ){
            volatile_button_state.set(current_button_state.button);
            stopCursorAnimation( clickstr_state );

            return true;
        }
    }

    //catch 'left-button click' that fell through?
    if ((event_mode & WAIT_INPUT_MODE) && !key_pressed_flag &&
        ( (autoclick_time == 0) || (event_mode & WAIT_BUTTON_MODE) )){
        //check for "button click"
        if ( (event->keysym.sym == KEY_RETURN) ||
             (event->keysym.sym == KEY_KP_ENTER) ||
             (event->keysym.sym == KEY_SPACE) ){
            key_pressed_flag = true;
            skip_effect = true;
            current_button_state.set(0);
            volatile_button_state.set(0);
            playClickVoice();
            stopCursorAnimation( clickstr_state );

            return true;
        }
    }

    if ((event_mode & (WAIT_INPUT_MODE | WAIT_TEXTBTN_MODE)) &&
        !key_pressed_flag){
        //'s' is for skip mode
        if ((event->keysym.sym == KEY_s) && !automode_flag &&
            !ctrl_pressed_status){
            if (!(skip_mode & SKIP_NORMAL))
                skip_effect = true; // short-circuit a current effect
            skip_mode |= SKIP_NORMAL;
            printf("toggle skip to true\n");
            key_pressed_flag = true;
            current_button_state.set(0);
            volatile_button_state.set(0);
            stopCursorAnimation( clickstr_state );

            return true;
        }
        //'o' is for one-page mode toggle
        else if ( (event->keysym.sym == KEY_o) && !ctrl_pressed_status ){
            if (skip_mode & SKIP_TO_EOP)
                skip_mode &= ~SKIP_TO_EOP;
            else
                skip_mode |= SKIP_TO_EOP;
            printf("toggle draw one page flag to %s\n",
                   (skip_mode & SKIP_TO_EOP?"true":"false"));
            if (skip_mode & SKIP_TO_EOP){
                current_button_state.set(0);
                volatile_button_state.set(0);
                stopCursorAnimation( clickstr_state );

                return true;
            }
        }
        //'a' is for automode
        else if ((event->keysym.sym == KEY_a) && !ctrl_pressed_status &&
                 mode_ext_flag && !automode_flag){
            automode_flag = true;
            skip_mode &= ~SKIP_NORMAL;
            printf("change to automode\n");
            key_pressed_flag = true;
            current_button_state.set(0);
            volatile_button_state.set(0);
            stopCursorAnimation( clickstr_state );

            return true;
        }
        //toggle default text speed
        else if ( event->keysym.sym == KEY_0 ){
            if (++text_speed_no > 2) text_speed_no = 0;
            sentence_font.wait_time = -1;
        }
        //slow default text speed
        else if ( event->keysym.sym == KEY_1 ){
            text_speed_no = 0;
            sentence_font.wait_time = -1;
        }
        //medium default text speed
        else if ( event->keysym.sym == KEY_2 ){
            text_speed_no = 1;
            sentence_font.wait_time = -1;
        }
        //fast default text speed
        else if ( event->keysym.sym == KEY_3 ){
            text_speed_no = 2;
            sentence_font.wait_time = -1;
        }
    }

        //'f' is for fullscreen toggle
        if ( (event->keysym.sym == KEY_f) && !ctrl_pressed_status ){
            if ( fullscreen_mode ) menu_windowCommand();
            else                   menu_fullCommand();
        }

    //using insani's skippable wait
    if ( event_mode & WAIT_SLEEP_MODE) {
        if (event->keysym.sym == KEY_s )
        {
            skip_mode |= SKIP_TO_WAIT;
            skip_mode &= ~SKIP_NORMAL;
            key_pressed_flag = true;
        }
    }
    if ((skip_mode & SKIP_TO_WAIT) && 
        ((event->keysym.sym == KEY_RETURN) ||
         (event->keysym.sym == KEY_KP_ENTER) ||
         (event->keysym.sym == KEY_SPACE) )) {
        skip_mode &= ~SKIP_TO_WAIT;
        key_pressed_flag = true;
    }
    if ((event_mode & WAIT_TEXTOUT_MODE) &&
        ((event->keysym.sym == KEY_RETURN) ||
         (event->keysym.sym == KEY_KP_ENTER) ||
         (event->keysym.sym == KEY_SPACE) )) {
        skip_mode |= (SKIP_TO_WAIT | SKIP_TO_EOL);
        key_pressed_flag = true;
    }

#if defined(WIN32) && defined(USE_MESSAGEBOX)
    if ((event->keysym.sym == KEY_F1) && (version_str != NULL)){
        //F1 is for Help (on Windows), so show the About dialog box
        menu_windowCommand();
        MessageBox(NULL, version_str, "About",
                   MB_OK|MB_ICONINFORMATION);

        key_pressed_flag = true;
    }
#endif

    return false;
}

void ONScripterLabel::animEvent( void )
{
    if (!(event_mode & WAIT_NO_ANIM_MODE)){
        int duration = proceedAnimation();

        if ( duration >= 0 ){
            if (anim_timer_id == NULL)
                flush(refreshMode() | (draw_cursor_flag?REFRESH_CURSOR_MODE:0));
            advanceAnimPhase( duration );
        }
    }
}

void ONScripterLabel::timerEvent( void )
{
    int duration = proceedCursorAnimation();

    if ( duration >= 0 ){
        flush(refreshMode() | (draw_cursor_flag?REFRESH_CURSOR_MODE:0));
        advancePhase( duration );
    }

    volatile_button_state.reset();
}


void ONScripterLabel::runEventLoop()
{
    InputEvent event;
    bool started_in_automode = automode_flag;

    while ( true ) {
        renderer_.tick();

        if ( !input_.wait( event ) ) {
            if ( renderer_.quit_requested() ) break;
            /* No input arrived: keep an async movie advancing. */
            updateAsyncMovie();
            continue;
        }

        bool ret = false;
        bool ctrl_toggle = (ctrl_pressed_status != 0);
        bool voice_just_ended = false;
        bool had_automode = automode_flag;

        // ignore continuous mouse motion
        while (event.type == EventType::MouseMotion){
            InputEvent tmp_event;
            if ( !input_.peek( tmp_event ) ) break;
            if (tmp_event.type != EventType::MouseMotion) break;
            input_.pop( tmp_event );
            event = tmp_event;
        }

        if (event.type == EventType::User) {
            switch (event.user.code) {
              case ONS_TIMER_EVENT:
                timerEvent();
                break;

              case ONS_ANIM_EVENT:
                animEvent();
                break;

              case ONS_SOUND_EVENT:
              case ONS_CDAUDIO_EVENT:
              case ONS_BGMFADE_EVENT:
              case ONS_SEQMUSIC_EVENT:
              case ONS_MUSIC_EVENT:
                flushEventSub( event );
                break;

              case ONS_WAVE_EVENT:
                flushEventSub( event );
                if ( (event.user.code2 != 0) ||
                     !(event_mode & WAIT_VOICE_MODE) ) break;
                voice_just_ended = true;
                event_mode &= ~WAIT_VOICE_MODE;
                // fall through

              case ONS_BREAK_EVENT:
                if ((event_mode & WAIT_VOICE_MODE) && wave_sample[0]){
                    break;
                }
                if (voice_just_ended) {
                    clearTimer(break_id);
                    if (automode_flag && (automode_time > 0)) {
                        break_id = ons::add_timer(automode_time, breakCallback, NULL);
                        break;
                    } else if (autoclick_time > 0) {
                        break_id = ons::add_timer(autoclick_time, breakCallback, NULL);
                        break;
                    }
                }
                if (!automode_flag && started_in_automode &&
                    (clickstr_state != CLICK_NONE)) {
                    started_in_automode = false;
                    break;
                }
                if (automode_flag || (autoclick_time > 0))
                    current_button_state.set(0);
                else if (btntime_value > 0){
                    if ( usewheel_flag )
                        current_button_state.set(-5);
                    else
                        current_button_state.set(-2);
                } else
                    current_button_state.set(0);
                if ((event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE)) &&
                    ( (clickstr_state == CLICK_WAIT) ||
                      (clickstr_state == CLICK_NEWPAGE) )){
                    playClickVoice();
                    stopCursorAnimation( clickstr_state );
                }
                return;

              default:
                break;
            }
            continue;
        }

        switch (event.type) {
          case EventType::MouseMotion:
            ret = mouseMoveEvent( &event.motion );
            if (ret) return;
            break;

          case EventType::MouseButtonDown:
            if ( !btndown_flag ) break;

            // fall through
          case EventType::MouseButtonUp:
            ret = mousePressEvent( &event.button );
            if (ret) return;
            if (!(event_mode & WAIT_TEXTOUT_MODE) && had_automode && !automode_flag){
                clearTimer(break_id);
            }
            break;

          case EventType::KeyDown:
            event.key.keysym = transKey(event.key.keysym, true);
            ret = keyDownEvent( &event.key );
            ctrl_toggle ^= (ctrl_pressed_status != 0);
            //allow skipping sleep waits with start of ctrl keydown
            ret |= (event_mode & WAIT_SLEEP_MODE) && ctrl_toggle;
            if ( btndown_flag )
                ret |= keyPressEvent( &event.key );
            if (ret) return;
            break;

          case EventType::KeyUp:
            event.key.keysym = transKey(event.key.keysym, false);
            keyUpEvent( &event.key );
            ret = keyPressEvent( &event.key );
            if (ret) return;
            break;

          /* Wheel motion is delivered as button 4/5 presses by InputSystem,
           * so it is handled by mousePressEvent(). */

          case EventType::FocusGained:
            if ( !event.gain ) break;

            // fall through
          case EventType::Expose:
            renderer_.present_all( *screen_surface );
            break;

          case EventType::Quit:
            endCommand();
            break;

          default:
            break;
        }
    }
}

