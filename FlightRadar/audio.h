// audio.h - one-shot playback of the "military aircraft" announcement over
// an I2S amplifier (e.g. MAX98357A). The clip itself is compiled in from
// audio_data.h - see extras/wav_to_header.py to replace the placeholder tone
// with your own recording.
#pragma once

void audioInit();          // call once from setup(); safe to call even if AUDIO_ENABLED is 0
void audioPlayMilitary();  // non-blocking: signals the audio task to (re)start the clip
bool audioTestSpeaker();   // queue the configured WAV even when announcements are disabled
void audioPlayEmergencyBeep(); // non-blocking urgent alert tone (squawk 7500)
void audioPlayEmergency7700(); // non-blocking: loud 2s-on/1s-off pattern, 5s of total beep time (squawk 7700)
bool audioClipBusy();      // true while the military/test clip is actively playing over I2S -
                            // lets the UI keep a full-screen alert up for exactly the clip's duration
