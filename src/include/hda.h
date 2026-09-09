#ifndef HDA_H
#define HDA_H

#include <efi.h>

/* Intel HD Audio playback: one short output stream before ExitBootServices.
 * No interrupts, mixing or capture. Realtek/Conexant/Intel/AMD codecs; anything
 * else goes through the generic widget walk. Stubs on AArch64 / no HDA. */

#define HDA_SAMPLE_RATE   48000
#define HDA_CHANNELS      2

/* Bring-up budget - a boot must never wait on a sound effect. Once a stream is
 * running the deadline extends by sound length plus slack. */
#define HDA_SETUP_BUDGET_MS  400
#define HDA_DRAIN_SLACK_MS   250

/* Hard ceiling once a stream is running. */
#define HDA_PLAY_BUDGET_MS   8000

/* Teardown gets its own allowance; the play budget is typically spent by then. */
#define HDA_CLEANUP_BUDGET_MS 200

/* Why playback did not happen, for the boot log. */
#define HDA_OK             0
#define HDA_NO_CONTROLLER  1   /* no PCI class 04:03 device */
#define HDA_NO_CODEC       2   /* controller up, no codec responded */
#define HDA_NO_PATH        3   /* codec found, no usable DAC->pin route */
#define HDA_NO_MEMORY      4
#define HDA_HW_ERROR       5   /* reset/stream did not come up in time */
#define HDA_TIMEOUT        6
#define HDA_UNSUPPORTED    7   /* not built for this architecture */

/* Play 16-bit signed stereo interleaved PCM at 48 kHz.
 * frames = sample count per channel (samples / 2). Blocks until the sound
 * finishes or the budget runs out. Returns one of the HDA_* codes above. */
int hda_play_pcm(const INT16 *pcm, UINTN frames);

void* hda_play_begin(const INT16 *pcm, UINTN frames, UINTN active_frames,
                     int *status);

/* Split playback lets the caller draw (e.g. a fade-in) while the sound plays:
 * prepare() wires up hardware and returns a handle, start() releases the DMA
 * engine, done() says whether the audible part finished, cut() stops now and
 * end() plays it out before tearing down. A handle must end with cut() or
 * end(). begin() is prepare()+start() rolled into one. */
void* hda_play_prepare(const INT16 *pcm, UINTN frames, UINTN active_frames,
                       int *status);
int   hda_play_start(void *handle);
int   hda_play_done(void *handle);
int   hda_play_cut(void *handle);
int   hda_play_end(void *handle);

/* Probe only: bring up a controller, find a codec and an output path, tear it
 * down again. Returns an HDA_* code. Useful for logging what a machine can do
 * without making noise. */
int hda_probe(void);

/* Human-readable form of an HDA_* code, for efi_log(). */
const CHAR16* hda_status_str(int code);

#endif
