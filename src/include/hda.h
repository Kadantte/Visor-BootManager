#ifndef HDA_H
#define HDA_H

#include <efi.h>

/* Intel HD Audio playback, just enough of it to play one short sound before
 * ExitBootServices. No interrupts, no mixing, no capture: set up a single
 * output stream, push samples through it, tear everything down.
 *
 * Codecs handled: Realtek (0x10EC), Conexant (0x14F1), Intel display audio
 * (0x8086) and AMD/ATI display audio (0x1002 / 0x1022). Anything else still
 * goes through the generic widget walk, which is often enough.
 *
 * Every entry point is safe to call on hardware without an HDA controller and
 * on AArch64, where the whole thing compiles to stubs.
 */

#define HDA_SAMPLE_RATE   48000
#define HDA_CHANNELS      2

/* Bring-up budget: PCI scan, controller reset, codec enumeration and route
 * finding all have to fit in here. A boot must never be held up waiting on a
 * sound effect, so this is deliberately tight - a machine whose codec does not
 * answer promptly simply gets no sound.
 *
 * Once a stream is actually running the deadline extends by the length of the
 * sound plus a little slack, since by then we know the hardware works. */
#define HDA_SETUP_BUDGET_MS  400
#define HDA_DRAIN_SLACK_MS   250

/* Hard ceiling once a stream is running. */
#define HDA_PLAY_BUDGET_MS   8000

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
