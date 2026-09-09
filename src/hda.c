#include "hda.h"

#if !defined(__x86_64__)

/* No HDA on AArch64 targets: the controller is x86 chipset hardware. */

int hda_play_pcm(const INT16 *pcm, UINTN frames) {
    (void)pcm; (void)frames;
    return HDA_UNSUPPORTED;
}

void* hda_play_begin(const INT16 *pcm, UINTN frames, UINTN active_frames,
                     int *status) {
    (void)pcm; (void)frames; (void)active_frames;
    if (status) *status = HDA_UNSUPPORTED;
    return NULL;
}

void* hda_play_prepare(const INT16 *pcm, UINTN frames, UINTN active_frames,
                       int *status) {
    (void)pcm; (void)frames; (void)active_frames;
    if (status) *status = HDA_UNSUPPORTED;
    return NULL;
}

int hda_play_start(void *handle) {
    (void)handle;
    return HDA_UNSUPPORTED;
}

int hda_play_done(void *handle) {
    (void)handle;
    return 1;
}

int hda_play_cut(void *handle) {
    (void)handle;
    return HDA_UNSUPPORTED;
}

int hda_play_end(void *handle) {
    (void)handle;
    return HDA_UNSUPPORTED;
}

int hda_probe(void) { return HDA_UNSUPPORTED; }

const CHAR16* hda_status_str(int code) {
    (void)code;
    return L"unsupported on this architecture";
}

#else

#include <efi.h>
#include <efilib.h>
#include "efi_helpers.h"
#include "arch.h"

extern EFI_BOOT_SERVICES *BS;

static UINT64 g_stream_start_us;

/* ---------------------------------------------------------------- registers */

/* Controller MMIO, BAR0. Offsets from the HD Audio 1.0a spec. */
#define REG_GCAP        0x00    /* 16: capabilities, low bit = 64-bit OK */
#define REG_VMIN        0x02
#define REG_VMAJ        0x03
#define REG_GCTL        0x08    /* 32: bit0 CRST */
#define REG_WAKEEN      0x0C
#define REG_STATESTS    0x0E    /* 16: one bit per codec address */
#define REG_INTCTL      0x20
#define REG_INTSTS      0x24
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48    /* 16 */
#define REG_CORBRP      0x4A    /* 16: bit15 = reset */
#define REG_CORBCTL     0x4C    /* 8:  bit1 = run */
#define REG_CORBSIZE    0x4E    /* 8 */
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58    /* 16: bit15 = reset */
#define REG_RINTCNT     0x5A    /* 16 */
#define REG_RIRBCTL     0x5C    /* 8:  bit1 = DMA run */
#define REG_RIRBSTS     0x5D    /* 8 */
#define REG_RIRBSIZE    0x5E    /* 8 */
#define REG_DPLBASE     0x70
#define REG_DPUBASE     0x74

#define GCTL_CRST       (1u << 0)
#define CORBCTL_RUN     (1u << 1)
#define CORBRP_RST      (1u << 15)
#define RIRBCTL_DMAEN   (1u << 1)
#define RIRBCTL_RIE     (1u << 0)   /* response interrupt enable */
#define RIRBWP_RST      (1u << 15)

/* Stream descriptor block. Output streams start after the input ones; the
 * count of each lives in GCAP. */
#define SD_CTL          0x00    /* 24 bits: bit0 SRST, bit1 RUN, 20:23 stream# */
#define SD_STS          0x03    /* 8 */
#define SD_LPIB         0x04    /* 32: link position in buffer */
#define SD_CBL          0x08    /* 32: cyclic buffer length */
#define SD_LVI          0x0C    /* 16: last valid index */
#define SD_FIFOW        0x0E
#define SD_FIFOS        0x10
#define SD_FMT          0x12    /* 16 */
#define SD_BDLPL        0x18
#define SD_BDLPU        0x1C

#define SDCTL_SRST      (1u << 0)
#define SDCTL_RUN       (1u << 1)
#define SDSTS_BCIS      (1u << 2)
#define SDSTS_FIFOE     (1u << 3)
#define SDSTS_DESE      (1u << 4)

/* GCAP field extraction */
#define GCAP_ISS(g)     (((g) >> 8)  & 0x0F)
#define GCAP_OSS(g)     (((g) >> 12) & 0x0F)
#define GCAP_64OK(g)    ((g) & 0x0001)

/* 48 kHz, 16 bit, 2 channels: base 48k, mult 1, div 1, bits=001, chan=1. */
#define STREAM_FORMAT   0x0011

/* The stream tag we claim. Any nonzero 4-bit value; nothing else is running. */
#define STREAM_TAG      1

/* --------------------------------------------------------------- verbs */

/* A codec command is 32 bits: CAd[31:28] NID[27:20] verb+payload[19:0].
 * 12-bit verbs carry an 8-bit payload, 4-bit verbs carry 16 bits. */
#define VERB12(nid, verb, payload) \
    (((UINT32)(nid) << 20) | ((UINT32)(verb) << 8) | ((UINT32)(payload) & 0xFF))
#define VERB4(nid, verb, payload) \
    (((UINT32)(nid) << 20) | ((UINT32)(verb) << 16) | ((UINT32)(payload) & 0xFFFF))

#define V_GET_PARAM             0xF00
#define V_GET_CONN_SEL          0xF01
#define V_SET_CONN_SEL          0x701
#define V_GET_CONN_LIST         0xF02
#define V_GET_PIN_SENSE         0xF09
#define V_SET_PIN_SENSE         0x709
#define V_GET_POWER_STATE       0xF05
#define V_SET_POWER_STATE       0x705
#define V_SET_CONV_FORMAT       0x2     /* 4-bit verb */
#define V_GET_CONV_FORMAT       0xA     /* 4-bit verb */
#define V_GET_CONV_STREAM       0xF06
#define V_SET_CONV_STREAM       0x706
#define V_GET_PIN_CTL           0xF07
#define V_SET_PIN_CTL           0x707
#define V_GET_EAPD              0xF0C
#define V_SET_EAPD              0x70C
#define V_GET_AMP_GAIN          0xB     /* 4-bit verb */
#define V_SET_AMP_GAIN          0x3     /* 4-bit verb */
#define V_GET_CONFIG_DEFAULT    0xF1C
#define V_SET_CHAN_STREAMID     0x706
#define V_FUNC_RESET            0x7FF

/* GPIO registers on the function group; read-only, logged for the OS driver. */
#define V_GET_GPIO_DATA         0xF15
#define V_GET_GPIO_MASK         0xF16
#define V_GET_GPIO_DIR          0xF17
#define V_GET_GPIO_STICKY       0xF1A

/* Power states, for V_SET_POWER_STATE */
#define PS_D0                   0x00
#define PS_D3HOT                0x03

/* GET_PARAMETER indices */
#define P_VENDOR_ID             0x00
#define P_REVISION_ID           0x02
#define P_SUBORDINATE_NODE      0x04
#define P_FUNC_TYPE             0x05
#define P_AUDIO_WIDGET_CAP      0x09
#define P_PCM_SUPPORT           0x0A
#define P_STREAM_FORMATS        0x0B
#define P_PIN_CAP               0x0C
#define P_INPUT_AMP_CAP         0x0D
#define P_CONN_LIST_LEN         0x0E
#define P_POWER_STATES          0x0F
#define P_PROCESSING_CAP        0x10
#define P_GPIO_COUNT            0x11
#define P_OUTPUT_AMP_CAP        0x12
#define P_VOLUME_KNOB_CAP       0x13

#define FUNC_TYPE_AUDIO         0x01

/* Widget capabilities, from P_AUDIO_WIDGET_CAP */
#define WCAP_TYPE(c)            (((c) >> 20) & 0x0F)
#define WCAP_DELAY(c)           (((c) >> 16) & 0x0F)
#define WCAP_CHAN_EXT(c)        (((c) >> 13) & 0x07)
#define WCAP_LR_SWAP            (1u << 11)
#define WCAP_POWER_CTL          (1u << 10)
#define WCAP_DIGITAL            (1u << 9)
#define WCAP_CONN_LIST          (1u << 8)
#define WCAP_UNSOL              (1u << 7)
#define WCAP_PROC               (1u << 6)
#define WCAP_STRIPE             (1u << 5)
#define WCAP_FORMAT_OVERRIDE    (1u << 4)
#define WCAP_AMP_OVERRIDE       (1u << 3)
#define WCAP_OUT_AMP            (1u << 2)
#define WCAP_IN_AMP             (1u << 1)
#define WCAP_STEREO             (1u << 0)

#define WTYPE_AUDIO_OUT         0x0
#define WTYPE_AUDIO_IN          0x1
#define WTYPE_AUDIO_MIXER       0x2
#define WTYPE_AUDIO_SELECTOR    0x3
#define WTYPE_PIN_COMPLEX       0x4
#define WTYPE_POWER             0x5
#define WTYPE_VOLUME_KNOB       0x6
#define WTYPE_BEEP_GEN          0x7
#define WTYPE_VENDOR            0xF

/* Pin capabilities */
#define PINCAP_OUTPUT           (1u << 4)
#define PINCAP_INPUT            (1u << 5)
#define PINCAP_HP_DRIVE         (1u << 3)
#define PINCAP_PRESENCE_DETECT  (1u << 2)
#define PINCAP_TRIGGER_REQ      (1u << 1)
#define PINCAP_IMPEDANCE_SENSE  (1u << 0)
#define PINCAP_EAPD             (1u << 16)
#define PINCAP_HDMI             (1u << 7)
#define PINCAP_DP               (1u << 24)

/* Pin widget control */
#define PINCTL_OUT_EN           (1u << 6)
#define PINCTL_IN_EN            (1u << 5)
#define PINCTL_HP_EN            (1u << 7)

/* Configuration default: where a pin physically goes */
#define CFG_PORT_CONN(c)        (((c) >> 30) & 0x03)
#define CFG_DEFAULT_DEV(c)      (((c) >> 20) & 0x0F)
#define PORT_CONN_JACK          0
#define PORT_CONN_NONE          1
#define PORT_CONN_FIXED         2
#define PORT_CONN_BOTH          3

#define DEV_LINE_OUT            0x0
#define DEV_SPEAKER             0x1
#define DEV_HP_OUT              0x2
#define DEV_SPDIF_OUT           0x4
#define DEV_DIGITAL_OUT         0x5

/* Amplifier gain/mute payload (4-bit verb, 16-bit payload) */
#define AMP_SET_OUTPUT          (1u << 15)
#define AMP_SET_INPUT           (1u << 14)
#define AMP_SET_LEFT            (1u << 13)
#define AMP_SET_RIGHT           (1u << 12)
#define AMP_SET_INDEX(i)        (((UINT32)(i) & 0x0F) << 8)
#define AMP_MUTE                (1u << 7)
#define AMP_GAIN(v)             ((UINT32)(v) & 0x7F)

/* GET_AMP_GAIN_MUTE payload. Deliberately a different layout from the SET
 * macros above: the get verb reads one channel of one amp per call and answers
 * with the 8-bit mute|gain value in the low byte. Mixing the two up silently
 * snapshots the wrong register, so keep them visibly distinct. */
#define AMP_GET_OUTPUT          (1u << 15)
#define AMP_GET_INPUT           0u
#define AMP_GET_LEFT            (1u << 13)
#define AMP_GET_RIGHT           0u
#define AMP_GET_INDEX(i)        ((UINT32)(i) & 0x0F)
#define AMP_VALUE(r)            ((UINT8)((r) & 0xFF))

#define AMPCAP_OFFSET(c)        ((c) & 0x7F)
#define AMPCAP_NUM_STEPS(c)     (((c) >> 8) & 0x7F)
#define AMPCAP_MUTE_CAP(c)      ((c) & 0x80000000u)

/* Known vendors, for the quirks. */
#define VENDOR_REALTEK          0x10EC
#define VENDOR_CONEXANT         0x14F1
#define VENDOR_INTEL            0x8086
#define VENDOR_ATI              0x1002
#define VENDOR_AMD              0x1022
#define VENDOR_NVIDIA           0x10DE

/* ---------------------------------------------------------------- sizing */

#define CORB_ENTRIES    256     /* 256 * 4 = 1 KB, the largest size */
#define RIRB_ENTRIES    256     /* 256 * 8 = 2 KB */
#define BDL_ENTRIES     4

#define MAX_WIDGETS     96
#define MAX_CODECS      16
#define MAX_PINS        24

/* Undo journal depth, sized for the worst real route (8-widget path, mixer,
 * silenced pins). Overflow is reported, not silently dropped. */
#define MAX_SAVED_VERBS 128

/* Codecs answer in microseconds; a stuck one should not cost more than a blink. */
#define CODEC_TIMEOUT_US    20000
#define RESET_TIMEOUT_US    500000

/* Probe-sweep timeout: 16 addresses x full timeout would eat the setup budget. */
#define PROBE_TIMEOUT_US    2000

/* Codec pipeline latency, on top of the stream FIFO. */
#define CODEC_PIPELINE_US   2000

/* DMA region: CORB (1 KB) | RIRB (2 KB) | BDL | position | pad, as one block.
 * The sample buffer is a separate, larger allocation. */
#define DMA_CORB_OFF    0x0000
#define DMA_RIRB_OFF    0x0400
#define DMA_BDL_OFF     0x0C00
#define DMA_POS_OFF     0x0D00
#define DMA_RING_SIZE   0x1000

typedef struct {
    UINT64 addr;
    UINT32 len;
    UINT32 flags;       /* bit0 = interrupt on completion */
} __attribute__((packed)) bdl_entry_t;

typedef struct {
    UINT8  nid;
    UINT8  type;
    UINT8  n_conn;
    UINT8  digital;
    UINT32 caps;
    UINT8  conn[16];
} widget_t;

/* One undo step: a codec command that puts a register back. */
typedef struct {
    UINT32 cmd;
} saved_verb_t;

/* Controller registers we overwrite during bring-up, so teardown can hand the
 * controller back to the firmware exactly as it found it. */
typedef struct {
    int    valid;
    int    adopted;         /* firmware had it out of reset; we did not reset */
    UINT32 corb_lbase, corb_ubase;
    UINT32 rirb_lbase, rirb_ubase;
    UINT8  corb_ctl, corb_size;
    UINT8  rirb_ctl, rirb_size;
    UINT16 corb_wp, rintcnt;
    UINT32 dpl, dpu;
    UINT32 intctl;
    UINT16 wakeen;
    UINT32 sd_ctl, sd_cbl, sd_bdlpl, sd_bdlpu;
    UINT16 sd_lvi, sd_fmt;
} hw_saved_t;

typedef struct {
    EFI_PCI_IO_PROTOCOL *pci;

    /* ring buffers, host view */
    UINT32      *corb;
    UINT64      *rirb;
    bdl_entry_t *bdl;
    UINT32      *dmapos;
    void        *ring_host;
    void        *ring_map;
    UINT64       ring_dev;

    INT16   *samples;
    void    *samp_map;
    UINT64   samp_dev;
    UINTN    samp_pages;

    UINT16   corb_wp;
    UINT16   rirb_rp;

    UINTN    out_stream;    /* index of the output stream descriptor */
    UINTN    sd_base;       /* MMIO offset of that descriptor */

    UINT8    codec_addr;
    UINT8    afg_nid;       /* audio function group root */
    UINT32   vendor;

    widget_t widgets[MAX_WIDGETS];
    UINTN    widget_count;

    /* the route we found */
    UINT8    dac_nid;
    UINT8    pin_nid;
    UINT8    pin_dev;
    INT8     pin_jack;
    UINT8    path[8];       /* dac ... pin, inclusive */
    UINTN    path_len;

    /* Changed codec registers, replayed in reverse at teardown. */
    saved_verb_t undo[MAX_SAVED_VERBS];
    UINTN    undo_count;
    int      undo_partial;  /* journal is incomplete: no honest restore */
    int      state_lost;    /* we had to reset the codec; nothing to restore */

    hw_saved_t hw;

    /* Playback gain as a fraction of amp range; lower for headphones. */
    UINTN    gain_num;
    UINTN    gain_den;

    /* Verb timeout, lowered while probing codec addresses that may be absent. */
    UINT64   cmd_timeout_us;

    UINT64   start_us;
    UINT64   deadline_us;
    int      stream_running;
    UINT64   saved_attrs;
    int      attrs_saved;
} hda_t;

/* --------------------------------------------------------- MMIO plumbing */

static UINT8 mm_read8(hda_t *h, UINT32 off) {
    UINT8 v = 0;
    h->pci->Mem.Read(h->pci, EfiPciIoWidthUint8, 0, off, 1, &v);
    return v;
}

static UINT16 mm_read16(hda_t *h, UINT32 off) {
    UINT16 v = 0;
    h->pci->Mem.Read(h->pci, EfiPciIoWidthUint16, 0, off, 1, &v);
    return v;
}

static UINT32 mm_read32(hda_t *h, UINT32 off) {
    UINT32 v = 0;
    h->pci->Mem.Read(h->pci, EfiPciIoWidthUint32, 0, off, 1, &v);
    return v;
}

static void mm_write8(hda_t *h, UINT32 off, UINT8 v) {
    h->pci->Mem.Write(h->pci, EfiPciIoWidthUint8, 0, off, 1, &v);
}

static void mm_write16(hda_t *h, UINT32 off, UINT16 v) {
    h->pci->Mem.Write(h->pci, EfiPciIoWidthUint16, 0, off, 1, &v);
}

static void mm_write32(hda_t *h, UINT32 off, UINT32 v) {
    h->pci->Mem.Write(h->pci, EfiPciIoWidthUint32, 0, off, 1, &v);
}

/* ------------------------------------------------------------ time budget */

static int budget_left(hda_t *h) {
    return arch_now_us() < h->deadline_us;
}

/* BS->Stall wrapper (granularity is coarse, so keep the unit small and check
 * the budget between spins). */
static void spin_us(UINTN us) {
    BS->Stall(us);
}

/* -------------------------------------------------------- codec verb I/O */

/* Push one command into the CORB and wait for its response in the RIRB.
 * Returns 0 on success with *resp filled, nonzero on timeout. */
static int codec_cmd(hda_t *h, UINT32 payload, UINT32 *resp) {
    if (!budget_left(h)) return -1;

    UINT32 cmd = ((UINT32)h->codec_addr << 28) | payload;

    /* Note RIRBWP before issuing so we can detect the new entry. */
    UINT16 wp_before = mm_read16(h, REG_RIRBWP) & 0xFF;

    h->corb_wp = (UINT16)((h->corb_wp + 1) % CORB_ENTRIES);
    h->corb[h->corb_wp] = cmd;
    mm_write16(h, REG_CORBWP, h->corb_wp);

    UINT64 start = arch_now_us();
    for (;;) {
        UINT16 wp = mm_read16(h, REG_RIRBWP) & 0xFF;
        if (wp != wp_before) {
            /* Answer sits at the RIRBWP the controller just advanced. */
            UINT16 slot = wp % RIRB_ENTRIES;
            UINT64 r = h->rirb[slot];
            h->rirb_rp = wp;
            mm_write8(h, REG_RIRBSTS, 0x05);  /* clear response-interrupt latch */
            if (resp) *resp = (UINT32)r;
            return 0;
        }
        if (arch_now_us() - start > h->cmd_timeout_us) return -1;
        if (!budget_left(h)) return -1;
        spin_us(10);
    }
}

/* Commands with no meaningful response still need their RIRB slot drained. */
static int codec_set(hda_t *h, UINT32 payload) {
    UINT32 dummy = 0;
    return codec_cmd(h, payload, &dummy);
}

static UINT32 codec_param(hda_t *h, UINT8 nid, UINT8 param) {
    UINT32 r = 0;
    if (codec_cmd(h, VERB12(nid, V_GET_PARAM, param), &r) != 0) return 0;
    return r;
}

/* ------------------------------------------------------------ undo journal */

/* Record one undo command. Overflow marks the journal partial - no honest
 * restore, so teardown leaves the codec quiet instead. */
static void undo_push(hda_t *h, UINT32 cmd) {
    if (h->undo_count >= MAX_SAVED_VERBS) { h->undo_partial = 1; return; }
    h->undo[h->undo_count++].cmd = cmd;
}

/* Read, journal the undo, then write. `get` selects the register, `undo_sel`
 * the same register via a set verb, `mask` the response bits that belong in the
 * value byte. A failed read still writes but marks the journal partial. */
static int codec_write_saved(hda_t *h, UINT32 get, UINT32 undo_sel,
                             UINT32 mask, UINT32 set) {
    UINT32 old = 0;
    if (codec_cmd(h, get, &old) == 0) undo_push(h, undo_sel | (old & mask));
    else                              h->undo_partial = 1;
    return codec_set(h, set);
}

/* --------------------------------------------------- controller bring-up */

static EFI_PCI_IO_PROTOCOL* find_controller(void) {
    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(&PciIoProtocol, &count);
    if (!handles) return NULL;

    EFI_PCI_IO_PROTOCOL *found = NULL;
    for (UINTN i = 0; i < count && !found; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], &PciIoProtocol, (void**)&pci)))
            continue;
        if (!pci) continue;

        /* Class code lives at config offset 0x09..0x0B: prog-if, subclass,
         * base class. Multimedia (0x04) / HD Audio (0x03). */
        UINT8 cls[3] = {0, 0, 0};
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint8, 0x09, 3, cls)))
            continue;
        if (cls[2] == 0x04 && cls[1] == 0x03) found = pci;
    }

    efi_free_pool(handles);
    return found;
}

/* Enable memory space and bus mastering, remembering what we changed. */
static int pci_enable(hda_t *h) {
    UINT64 have = 0;
    if (!EFI_ERROR(h->pci->Attributes(h->pci, EfiPciIoAttributeOperationGet,
                                      0, &have))) {
        h->saved_attrs = have;
        h->attrs_saved = 1;
    }
    EFI_STATUS st = h->pci->Attributes(h->pci, EfiPciIoAttributeOperationEnable,
                                       EFI_PCI_IO_ATTRIBUTE_MEMORY |
                                       EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
    return EFI_ERROR(st) ? -1 : 0;
}

/* Snapshot controller registers before rings_init() overwrites them; the
 * stream descriptor is saved separately once its index is known. */
static void hw_snapshot(hda_t *h) {
    hw_saved_t *s = &h->hw;

    s->corb_lbase = mm_read32(h, REG_CORBLBASE);
    s->corb_ubase = mm_read32(h, REG_CORBUBASE);
    s->corb_ctl   = mm_read8(h,  REG_CORBCTL);
    s->corb_size  = mm_read8(h,  REG_CORBSIZE);
    s->corb_wp    = mm_read16(h, REG_CORBWP);

    s->rirb_lbase = mm_read32(h, REG_RIRBLBASE);
    s->rirb_ubase = mm_read32(h, REG_RIRBUBASE);
    s->rirb_ctl   = mm_read8(h,  REG_RIRBCTL);
    s->rirb_size  = mm_read8(h,  REG_RIRBSIZE);
    s->rintcnt    = mm_read16(h, REG_RINTCNT);

    /* CORBRP/RIRBWP are pointer registers; the only writable bit is reset,
     * so there is nothing to put back. */

    s->dpl    = mm_read32(h, REG_DPLBASE);
    s->dpu    = mm_read32(h, REG_DPUBASE);
    s->intctl = mm_read32(h, REG_INTCTL);
    s->wakeen = mm_read16(h, REG_WAKEEN);

    s->valid = 1;
}

/* Save the stream descriptor we are about to program. Called from rings_init()
 * once h->sd_base is known. */
static void sd_snapshot(hda_t *h) {
    UINT32 base = (UINT32)h->sd_base;
    hw_saved_t *s = &h->hw;

    s->sd_ctl   = mm_read32(h, base + SD_CTL);
    s->sd_cbl   = mm_read32(h, base + SD_CBL);
    s->sd_lvi   = mm_read16(h, base + SD_LVI);
    s->sd_fmt   = mm_read16(h, base + SD_FMT);
    s->sd_bdlpl = mm_read32(h, base + SD_BDLPL);
    s->sd_bdlpu = mm_read32(h, base + SD_BDLPU);
}

/* Adopt a running controller as-is. A link reset would wipe the codec state
 * the firmware programmed at POST (GPIO/EAPD/pin config), which an OS driver
 * inherits; nothing we need requires a reset. */
static int controller_acquire(hda_t *h) {
    hw_snapshot(h);

    if (mm_read32(h, REG_GCTL) & GCTL_CRST) {
        h->hw.adopted = 1;
        efi_log(L"hda: controller already out of reset - adopting it as-is");
    } else {
        /* Bring the link up ourselves: clear CRST, wait, set, wait again. */
        efi_log(L"hda: controller was in reset - bringing the link up");
        h->hw.adopted = 0;
        h->state_lost = 1;

        mm_write32(h, REG_GCTL, mm_read32(h, REG_GCTL) & ~GCTL_CRST);

        UINT64 start = arch_now_us();
        while (mm_read32(h, REG_GCTL) & GCTL_CRST) {
            if (arch_now_us() - start > RESET_TIMEOUT_US) return -1;
            if (!budget_left(h)) return -1;
            spin_us(100);
        }
        spin_us(500);

        mm_write32(h, REG_GCTL, mm_read32(h, REG_GCTL) | GCTL_CRST);
        start = arch_now_us();
        while (!(mm_read32(h, REG_GCTL) & GCTL_CRST)) {
            if (arch_now_us() - start > RESET_TIMEOUT_US) return -1;
            if (!budget_left(h)) return -1;
            spin_us(100);
        }

        /* Codecs need time on the link before STATESTS is meaningful. The spec
         * asks for 521 us; give it a little more. */
        spin_us(1000);
    }

    /* Interrupts stay off: we poll everything. Both registers are in the
     * snapshot, so teardown puts them back. */
    mm_write32(h, REG_INTCTL, 0);
    mm_write16(h, REG_WAKEEN, 0);
    return 0;
}

/* Allocate the ring page and point the controller at it. */
static int rings_init(hda_t *h) {
    UINTN pages = DMA_RING_SIZE / 4096;
    if (pages == 0) pages = 1;

    /* AllocateBuffer with the below-4GB constraint handled for us when the
     * controller cannot do 64-bit addressing. */
    UINT16 gcap = mm_read16(h, REG_GCAP);
    void *host = NULL;
    EFI_STATUS st = h->pci->AllocateBuffer(h->pci, AllocateAnyPages,
                                           EfiBootServicesData, pages,
                                           &host, 0);
    if (EFI_ERROR(st) || !host) return -1;
    h->ring_host = host;

    UINTN bytes = pages * 4096;
    for (UINTN i = 0; i < bytes; i++) ((UINT8*)host)[i] = 0;

    EFI_PHYSICAL_ADDRESS dev = 0;
    UINTN mapped = bytes;
    st = h->pci->Map(h->pci, EfiPciIoOperationBusMasterCommonBuffer, host,
                     &mapped, &dev, &h->ring_map);
    if (EFI_ERROR(st) || mapped < bytes) return -1;

    if (!GCAP_64OK(gcap) && (dev + bytes) > 0x100000000ULL) {
        /* Controller is 32-bit only and we landed high. Nothing safe to do. */
        return -1;
    }

    h->ring_dev = dev;
    h->corb   = (UINT32*)((UINT8*)host + DMA_CORB_OFF);
    h->rirb   = (UINT64*)((UINT8*)host + DMA_RIRB_OFF);
    h->bdl    = (bdl_entry_t*)((UINT8*)host + DMA_BDL_OFF);
    h->dmapos = (UINT32*)((UINT8*)host + DMA_POS_OFF);

    /* CORB: stop, size 256, base, reset read pointer, run. */
    mm_write8(h, REG_CORBCTL, 0);
    mm_write8(h, REG_CORBSIZE, 0x02);           /* 256 entries */
    mm_write32(h, REG_CORBLBASE, (UINT32)(h->ring_dev + DMA_CORB_OFF));
    mm_write32(h, REG_CORBUBASE, (UINT32)((h->ring_dev + DMA_CORB_OFF) >> 32));

    mm_write16(h, REG_CORBRP, CORBRP_RST);
    UINT64 start = arch_now_us();
    while (!(mm_read16(h, REG_CORBRP) & CORBRP_RST)) {
        if (arch_now_us() - start > CODEC_TIMEOUT_US) break;
        spin_us(10);
    }
    mm_write16(h, REG_CORBRP, 0);
    start = arch_now_us();
    while (mm_read16(h, REG_CORBRP) & CORBRP_RST) {
        if (arch_now_us() - start > CODEC_TIMEOUT_US) break;
        spin_us(10);
    }
    mm_write16(h, REG_CORBWP, 0);
    h->corb_wp = 0;
    mm_write8(h, REG_CORBCTL, CORBCTL_RUN);

    /* RIRB: stop, size 256, base, reset write pointer; we poll, so no coalescing. */
    mm_write8(h, REG_RIRBCTL, 0);
    mm_write8(h, REG_RIRBSIZE, 0x02);
    mm_write32(h, REG_RIRBLBASE, (UINT32)(h->ring_dev + DMA_RIRB_OFF));
    mm_write32(h, REG_RIRBUBASE, (UINT32)((h->ring_dev + DMA_RIRB_OFF) >> 32));
    mm_write16(h, REG_RIRBWP, RIRBWP_RST);
    UINT64 rstart = arch_now_us();
    while (!(mm_read16(h, REG_RIRBWP) & RIRBWP_RST)) {
        if (arch_now_us() - rstart > CODEC_TIMEOUT_US) break;
        spin_us(10);
    }
    mm_write16(h, REG_RIRBWP, 0);
    rstart = arch_now_us();
    while (mm_read16(h, REG_RIRBWP) & RIRBWP_RST) {
        if (arch_now_us() - rstart > CODEC_TIMEOUT_US) break;
        spin_us(10);
    }
    mm_write16(h, REG_RINTCNT, 1);
    h->rirb_rp = 0;
    mm_write8(h, REG_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RIE);

    /* DMA position buffer (falls back to LPIB if unsupported). */
    mm_write32(h, REG_DPLBASE, (UINT32)(h->ring_dev + DMA_POS_OFF) | 1u);
    mm_write32(h, REG_DPUBASE, (UINT32)((h->ring_dev + DMA_POS_OFF) >> 32));

    /* Pick the first output stream descriptor. They come after the inputs. */
    UINTN iss = GCAP_ISS(gcap);
    h->out_stream = iss;
    h->sd_base = 0x80 + h->out_stream * 0x20;

    /* Save our descriptor before programming it. */
    sd_snapshot(h);
    return 0;
}

/* ------------------------------------------------------ codec enumeration */

/* Try to claim the codec at `addr`: it has to answer, and it has to have an
 * audio function group. Sets h->codec_addr, h->vendor and h->afg_nid. */
static int codec_claim(hda_t *h, UINT8 addr) {
    h->codec_addr = addr;

    UINT32 vid = codec_param(h, 0, P_VENDOR_ID);
    if (vid == 0 || vid == 0xFFFFFFFF) return -1;

    /* Something is home: stop rushing, the rest of the walk gets the full timeout. */
    h->cmd_timeout_us = CODEC_TIMEOUT_US;
    h->vendor = vid >> 16;

    /* Root node lists the function groups. Find the audio one. */
    UINT32 sub = codec_param(h, 0, P_SUBORDINATE_NODE);
    UINT8 start = (UINT8)((sub >> 16) & 0xFF);
    UINT8 count = (UINT8)(sub & 0xFF);
    for (UINT8 i = 0; i < count; i++) {
        UINT8 nid = (UINT8)(start + i);
        UINT32 ft = codec_param(h, nid, P_FUNC_TYPE);
        if ((ft & 0x7F) == FUNC_TYPE_AUDIO) { h->afg_nid = nid; return 0; }
    }

    /* Emulated codecs may hide function groups; scan low node IDs directly. */
    if (count == 0) {
        for (UINT8 nid = 1; nid <= 8; nid++) {
            UINT32 ft = codec_param(h, nid, P_FUNC_TYPE);
            if ((ft & 0x7F) == FUNC_TYPE_AUDIO) { h->afg_nid = nid; return 0; }
        }
    }
    return -1;
}

static int find_codec(hda_t *h) {
    /* STATESTS is only reliable after a reset, so treat it as a hint; if empty,
     * probe every address with a short timeout. */
    UINT16 states = mm_read16(h, REG_STATESTS);
    UINT16 candidates = states ? states : 0xFFFF;
    h->cmd_timeout_us = states ? CODEC_TIMEOUT_US : PROBE_TIMEOUT_US;

    for (UINT8 addr = 0; addr < MAX_CODECS; addr++) {
        if (!(candidates & (1u << addr))) continue;
        if (!budget_left(h)) break;
        if (codec_claim(h, addr) == 0) return 0;
    }
    h->cmd_timeout_us = CODEC_TIMEOUT_US;

    /* Last resort: a function reset wakes some emulated codecs but discards
     * firmware state, so only after the normal sweep fails. */
    for (UINT8 addr = 0; addr < MAX_CODECS; addr++) {
        if (!(candidates & (1u << addr))) continue;
        if (!budget_left(h)) break;

        h->codec_addr = addr;
        codec_set(h, VERB12(0, V_FUNC_RESET, 0));
        spin_us(1000);
        if (codec_claim(h, addr) == 0) {
            h->state_lost = 1;
            efi_log(L"hda: codec only answered after a function reset - its "
                    L"firmware-programmed state is gone");
            return 0;
        }
    }
    return -1;
}

static widget_t* widget_for(hda_t *h, UINT8 nid) {
    for (UINTN i = 0; i < h->widget_count; i++)
        if (h->widgets[i].nid == nid) return &h->widgets[i];
    return NULL;
}

static void read_connections(hda_t *h, widget_t *w) {
    w->n_conn = 0;
    if (!(w->caps & WCAP_CONN_LIST)) return;

    UINT32 len = codec_param(h, w->nid, P_CONN_LIST_LEN);
    UINTN n = len & 0x7F;
    int longform = (len & 0x80) != 0;
    if (n > 16) n = 16;

    /* GET_CONNECTION_LIST: 4 short or 2 long entries per call, starting at the
     * payload index. */
    UINTN per = longform ? 2 : 4;
    for (UINTN i = 0; i < n; i += per) {
        UINT32 r = 0;
        if (codec_cmd(h, VERB12(w->nid, V_GET_CONN_LIST, (UINT8)i), &r) != 0)
            return;
        for (UINTN k = 0; k < per && (i + k) < n; k++) {
            UINT16 e = longform ? (UINT16)((r >> (k * 16)) & 0xFFFF)
                                : (UINT16)((r >> (k * 8)) & 0xFF);
            /* High bit marks a range endpoint; first entry of the pair is enough to walk
             * backwards, so ranges are not expanded. */
            UINT16 nid = longform ? (e & 0x7FFF) : (e & 0x7F);
            if (w->n_conn < 16) w->conn[w->n_conn++] = (UINT8)nid;
        }
    }
}

static int enumerate_widgets(hda_t *h) {
    UINT32 sub = codec_param(h, h->afg_nid, P_SUBORDINATE_NODE);
    UINT8 start = (UINT8)((sub >> 16) & 0xFF);
    UINT8 count = (UINT8)(sub & 0xFF);
    if (count == 0) return -1;

    h->widget_count = 0;
    for (UINT8 i = 0; i < count && h->widget_count < MAX_WIDGETS; i++) {
        if (!budget_left(h)) return -1;
        UINT8 nid = (UINT8)(start + i);
        UINT32 caps = codec_param(h, nid, P_AUDIO_WIDGET_CAP);
        if (caps == 0) continue;

        widget_t *w = &h->widgets[h->widget_count++];
        w->nid     = nid;
        w->caps    = caps;
        w->type    = (UINT8)WCAP_TYPE(caps);
        w->digital = (caps & WCAP_DIGITAL) ? 1 : 0;
        read_connections(h, w);
    }
    return h->widget_count ? 0 : -1;
}

/* ------------------------------------------------------- path finding */

/* Is anything plugged into this jack? 1 yes, 0 no, -1 unknown. */
static int pin_jack_present(hda_t *h, UINT8 nid, UINT32 cap) {
    if (!(cap & PINCAP_PRESENCE_DETECT)) return -1;

    if (cap & PINCAP_TRIGGER_REQ) {
        codec_set(h, VERB12(nid, V_SET_PIN_SENSE, 0));
        spin_us(1000);
    }

    UINT32 sense = 0;
    if (codec_cmd(h, VERB12(nid, V_GET_PIN_SENSE, 0), &sense) != 0) return -1;
    return (sense & 0x80000000u) ? 1 : 0;
}

/* Score an output pin; a plugged-in headphone jack beats the speakers. */
static int pin_score(hda_t *h, widget_t *w, UINT8 *dev_out, INT8 *jack_out) {
    UINT32 cap = codec_param(h, w->nid, P_PIN_CAP);
    if (!(cap & PINCAP_OUTPUT)) return -1;

    UINT32 cfg = 0;
    if (codec_cmd(h, VERB12(w->nid, V_GET_CONFIG_DEFAULT, 0), &cfg) != 0)
        return -1;

    UINTN conn = CFG_PORT_CONN(cfg);
    UINTN dev  = CFG_DEFAULT_DEV(cfg);

    if (conn == PORT_CONN_NONE) return -1;

    if (dev_out) *dev_out = (UINT8)dev;
    if (jack_out) *jack_out = -1;

    /* Digital pins count only when something is plugged in, so a dead HDMI port
     * does not eat the budget. */
    if (w->digital || (cap & (PINCAP_HDMI | PINCAP_DP))) {
        if (cap & PINCAP_PRESENCE_DETECT) {
            UINT32 sense = 0;
            if (codec_cmd(h, VERB12(w->nid, V_GET_PIN_SENSE, 0), &sense) != 0)
                return -1;
            if (!(sense & 0x80000000u)) return -1;
        }
        return 10;
    }

    /* Fixed wiring (the case speaker) can't report a jack; skip presence. */
    int fixed = (conn == PORT_CONN_FIXED);
    int jack = fixed ? -1 : pin_jack_present(h, w->nid, cap);
    if (jack_out) *jack_out = (INT8)jack;

    switch (dev) {
    case DEV_HP_OUT:
        if (jack == 1) return 100;
        if (jack == 0) return 4;
        return 45;
    case DEV_LINE_OUT:
        if (fixed) return 40;
        if (jack == 1) return 70;
        if (jack == 0) return 3;
        return 35;
    case DEV_SPEAKER:
        if (jack == 0) return 12;
        return 40;
    default:
        return jack == 0 ? 2 : 5;
    }
}

/* Depth-first walk backwards from a pin through mixers and selectors until we
 * reach an Audio Output widget. Records the route in h->path, pin last. */
static int walk_to_dac(hda_t *h, UINT8 nid, UINT8 *trail, UINTN depth) {
    if (depth >= 8) return -1;
    widget_t *w = widget_for(h, nid);
    if (!w) return -1;

    trail[depth] = nid;

    if (w->type == WTYPE_AUDIO_OUT) {
        /* Found the converter. Copy the trail out, DAC first. */
        h->path_len = depth + 1;
        for (UINTN i = 0; i <= depth; i++)
            h->path[i] = trail[depth - i];
        h->dac_nid = nid;
        return 0;
    }

    if (w->type != WTYPE_PIN_COMPLEX && w->type != WTYPE_AUDIO_MIXER &&
        w->type != WTYPE_AUDIO_SELECTOR)
        return -1;

    for (UINTN i = 0; i < w->n_conn; i++) {
        if (!budget_left(h)) return -1;
        if (walk_to_dac(h, w->conn[i], trail, depth + 1) == 0) {
            /* On the way back out, make selectors point at the branch we
             * took. Mixers sum their inputs, so they need no selection. */
            if (w->type == WTYPE_AUDIO_SELECTOR ||
                (w->type == WTYPE_PIN_COMPLEX && w->n_conn > 1))
                codec_write_saved(h, VERB12(nid, V_GET_CONN_SEL, 0),
                                  VERB12(nid, V_SET_CONN_SEL, 0), 0xFF,
                                  VERB12(nid, V_SET_CONN_SEL, (UINT8)i));
            return 0;
        }
    }
    return -1;
}

static const CHAR16* pin_dev_str(UINT8 dev) {
    switch (dev) {
    case DEV_HP_OUT:      return L"headphones";
    case DEV_SPEAKER:     return L"speaker";
    case DEV_LINE_OUT:    return L"line out";
    case DEV_SPDIF_OUT:   return L"S/PDIF";
    case DEV_DIGITAL_OUT: return L"digital out";
    default:              return L"other output";
    }
}

static int find_output_path(hda_t *h) {
    struct { UINT8 nid; UINT8 dev; INT8 jack; int score; } cand[MAX_PINS];
    UINTN n = 0;

    for (UINTN i = 0; i < h->widget_count && n < MAX_PINS; i++) {
        if (!budget_left(h)) break;
        widget_t *w = &h->widgets[i];
        if (w->type != WTYPE_PIN_COMPLEX) continue;

        UINT8 dev = 0xFF;
        INT8 jack = -1;
        int s = pin_score(h, w, &dev, &jack);
        if (s < 0) continue;

        cand[n].nid = w->nid;
        cand[n].dev = dev;
        cand[n].jack = jack;
        cand[n].score = s;
        n++;
    }

    /* Try pins best-first; a dead jack never silences a working speaker. */
    for (UINTN tries = 0; tries < n; tries++) {
        UINTN best = 0;
        int best_score = -1;
        for (UINTN i = 0; i < n; i++)
            if (cand[i].score > best_score) { best_score = cand[i].score; best = i; }
        if (best_score < 0) break;
        cand[best].score = -1;

        UINT8 trail[8];
        h->pin_nid = cand[best].nid;
        if (walk_to_dac(h, h->pin_nid, trail, 0) == 0) {
            h->pin_dev  = cand[best].dev;
            h->pin_jack = cand[best].jack;

            CHAR16 msg[128];
            SPrint(msg, sizeof(msg), L"hda: playing to %s (pin 0x%02x%s)",
                   pin_dev_str(h->pin_dev), (unsigned int)h->pin_nid,
                   h->pin_jack == 1 ? L", jack in use" : L"");
            efi_log(msg);
            return 0;
        }
        if (!budget_left(h)) break;
    }

    h->pin_nid = 0;
    return -1;
}

/* --------------------------------------------------- widget configuration */

/* Widget amp caps, unless the widget does not override the function group's. */
static UINT32 amp_caps(hda_t *h, UINT8 nid, UINT32 caps, int is_output) {
    UINT8 from = (caps & WCAP_AMP_OVERRIDE) ? nid : h->afg_nid;
    return codec_param(h, from, is_output ? P_OUTPUT_AMP_CAP : P_INPUT_AMP_CAP);
}

/* Our playback level, as a fraction of this amp's range. */
static UINT32 amp_gain(hda_t *h, UINT32 ampcap) {
    UINTN steps = AMPCAP_NUM_STEPS(ampcap);
    if (!steps) return 0;
    return (UINT32)((steps * h->gain_num) / h->gain_den);
}

/* Set one amp channel to `value`, remembering the value we found there. */
static void amp_write(hda_t *h, UINT8 nid, UINT32 sel_get, UINT32 sel_set,
                      UINT32 value) {
    codec_write_saved(h, VERB4(nid, V_GET_AMP_GAIN, sel_get),
                      VERB4(nid, V_SET_AMP_GAIN, sel_set), 0xFF,
                      VERB4(nid, V_SET_AMP_GAIN, sel_set | value));
}

/* Mute one amp channel, but only if not already muted (saves a write + slot). */
static void amp_mute_if_open(hda_t *h, UINT8 nid, UINT32 sel_get,
                             UINT32 sel_set) {
    UINT32 old = 0;
    if (codec_cmd(h, VERB4(nid, V_GET_AMP_GAIN, sel_get), &old) != 0) return;
    if (old & AMP_MUTE) return;
    undo_push(h, VERB4(nid, V_SET_AMP_GAIN, sel_set) | (old & 0xFF));
    codec_set(h, VERB4(nid, V_SET_AMP_GAIN, sel_set | AMP_MUTE));
}

/* Unmute the widget's output amp and set it to our playback level. */
static void amp_open_output(hda_t *h, UINT8 nid, UINT32 caps) {
    if (!(caps & WCAP_OUT_AMP)) return;

    UINT32 gain = AMP_GAIN(amp_gain(h, amp_caps(h, nid, caps, 1)));

    amp_write(h, nid, AMP_GET_OUTPUT | AMP_GET_LEFT,
              AMP_SET_OUTPUT | AMP_SET_LEFT, gain);
    if (caps & WCAP_STEREO)
        amp_write(h, nid, AMP_GET_OUTPUT | AMP_GET_RIGHT,
                  AMP_SET_OUTPUT | AMP_SET_RIGHT, gain);
}

/* Open only the input amp carrying our signal, mute the rest. Input amps are
 * per-connection, and a mixer's other inputs are analog loopbacks whose bias
 * would bleed hiss into the output. `from_nid` feeds this widget on our route. */
static void amp_open_input(hda_t *h, UINT8 nid, UINT32 caps, UINT8 from_nid) {
    if (!(caps & WCAP_IN_AMP)) return;

    widget_t *w = widget_for(h, nid);
    UINTN n = w && w->n_conn ? w->n_conn : 1;
    if (n > 16) n = 16;

    UINT32 gain = AMP_GAIN(amp_gain(h, amp_caps(h, nid, caps, 0)));
    int stereo = (caps & WCAP_STEREO) != 0;

    for (UINTN i = 0; i < n; i++) {
        if (!budget_left(h)) return;

        /* Single connection, unknown source: assume it is ours. */
        int ours = (w && from_nid) ? (w->conn[i] == from_nid) : (n == 1);

        UINT32 get_l = AMP_GET_INPUT | AMP_GET_LEFT  | AMP_GET_INDEX(i);
        UINT32 set_l = AMP_SET_INPUT | AMP_SET_LEFT  | AMP_SET_INDEX(i);
        UINT32 get_r = AMP_GET_INPUT | AMP_GET_RIGHT | AMP_GET_INDEX(i);
        UINT32 set_r = AMP_SET_INPUT | AMP_SET_RIGHT | AMP_SET_INDEX(i);

        if (ours) {
            amp_write(h, nid, get_l, set_l, gain);
            if (stereo) amp_write(h, nid, get_r, set_r, gain);
        } else {
            amp_mute_if_open(h, nid, get_l, set_l);
            if (stereo) amp_mute_if_open(h, nid, get_r, set_r);
        }
    }
}

static void power_up(hda_t *h, UINT8 nid, UINT32 caps) {
    if (!(caps & WCAP_POWER_CTL)) return;
    /* A power state answers with the state we asked for in the low nibble and
     * the state the widget actually reached above it; only the former is a legal
     * payload to write back. */
    codec_write_saved(h, VERB12(nid, V_GET_POWER_STATE, 0),
                      VERB12(nid, V_SET_POWER_STATE, 0), 0x0F,
                      VERB12(nid, V_SET_POWER_STATE, PS_D0));
}

/* Quiet every analog output pin other than the one we chose, so a codec that
 * feeds speaker and headphones from the same converter doesn't play both. */
static void silence_other_outputs(hda_t *h) {
    for (UINTN i = 0; i < h->widget_count; i++) {
        if (!budget_left(h)) return;
        widget_t *w = &h->widgets[i];
        if (w->type != WTYPE_PIN_COMPLEX) continue;
        if (w->nid == h->pin_nid || w->digital) continue;

        UINT32 cap = codec_param(h, w->nid, P_PIN_CAP);
        if (!(cap & PINCAP_OUTPUT)) continue;

        UINT32 ctl = 0;
        if (codec_cmd(h, VERB12(w->nid, V_GET_PIN_CTL, 0), &ctl) != 0) continue;
        if (!(ctl & PINCTL_OUT_EN)) continue;

        /* We already hold the old value; journal it directly, no second read. */
        undo_push(h, VERB12(w->nid, V_SET_PIN_CTL, (UINT8)ctl));
        codec_set(h, VERB12(w->nid, V_SET_PIN_CTL,
                            (UINT8)(ctl & ~(PINCTL_OUT_EN | PINCTL_HP_EN))));
    }
}

static int configure_path(hda_t *h) {
    /* Headphones are more sensitive, so use half gain. A codec with two amp stages
     * attenuates twice - quiet rather than loud, the safe direction. */
    if (h->pin_dev == DEV_HP_OUT) { h->gain_num = 1; h->gain_den = 2; }

    /* Whole function group to D0 first; individual widgets may also need it. */
    codec_write_saved(h, VERB12(h->afg_nid, V_GET_POWER_STATE, 0),
                      VERB12(h->afg_nid, V_SET_POWER_STATE, 0), 0x0F,
                      VERB12(h->afg_nid, V_SET_POWER_STATE, PS_D0));

    for (UINTN i = 0; i < h->path_len; i++) {
        UINT8 nid = h->path[i];
        widget_t *w = widget_for(h, nid);
        if (!w) continue;

        /* path[] runs DAC first, so the widget feeding this one is path[i-1]. */
        UINT8 from = i ? h->path[i - 1] : 0;

        power_up(h, nid, w->caps);
        amp_open_output(h, nid, w->caps);
        amp_open_input(h, nid, w->caps, from);
    }

    silence_other_outputs(h);

    /* The pin needs its output driver switched on. */
    UINT32 pincap = codec_param(h, h->pin_nid, P_PIN_CAP);
    UINT32 ctl = PINCTL_OUT_EN;
    if (pincap & PINCAP_HP_DRIVE) ctl |= PINCTL_HP_EN;
    codec_write_saved(h, VERB12(h->pin_nid, V_GET_PIN_CTL, 0),
                      VERB12(h->pin_nid, V_SET_PIN_CTL, 0), 0xFF,
                      VERB12(h->pin_nid, V_SET_PIN_CTL, (UINT8)ctl));

    /* EAPD gates the external amp; enable wherever the pin claims it. */
    if (pincap & PINCAP_EAPD)
        codec_write_saved(h, VERB12(h->pin_nid, V_GET_EAPD, 0),
                          VERB12(h->pin_nid, V_SET_EAPD, 0), 0xFF,
                          VERB12(h->pin_nid, V_SET_EAPD, 0x02));  /* EAPD on */

    /* Point the converter at our stream and format. The format is a 16-bit
     * payload, so it is journalled by hand. */
    UINT32 fmt = 0;
    if (codec_cmd(h, VERB4(h->dac_nid, V_GET_CONV_FORMAT, 0), &fmt) == 0)
        undo_push(h, VERB4(h->dac_nid, V_SET_CONV_FORMAT, fmt & 0xFFFF));
    codec_set(h, VERB4(h->dac_nid, V_SET_CONV_FORMAT, STREAM_FORMAT));

    codec_write_saved(h, VERB12(h->dac_nid, V_GET_CONV_STREAM, 0),
                      VERB12(h->dac_nid, V_SET_CONV_STREAM, 0), 0xFF,
                      VERB12(h->dac_nid, V_SET_CONV_STREAM,
                             (UINT8)((STREAM_TAG << 4) | 0)));
    return 0;
}

/* ------------------------------------------------------- stream handling */

static int stream_reset(hda_t *h) {
    UINT32 base = (UINT32)h->sd_base;

    /* Stop first, then pulse SRST and wait for both edges. */
    mm_write8(h, base + SD_CTL, 0);
    UINT64 start = arch_now_us();
    while (mm_read8(h, base + SD_CTL) & SDCTL_RUN) {
        if (arch_now_us() - start > CODEC_TIMEOUT_US) break;
        spin_us(10);
    }

    mm_write8(h, base + SD_CTL, SDCTL_SRST);
    start = arch_now_us();
    while (!(mm_read8(h, base + SD_CTL) & SDCTL_SRST)) {
        if (arch_now_us() - start > CODEC_TIMEOUT_US) return -1;
        if (!budget_left(h)) return -1;
        spin_us(10);
    }
    mm_write8(h, base + SD_CTL, 0);
    start = arch_now_us();
    while (mm_read8(h, base + SD_CTL) & SDCTL_SRST) {
        if (arch_now_us() - start > CODEC_TIMEOUT_US) return -1;
        if (!budget_left(h)) return -1;
        spin_us(10);
    }
    return 0;
}

/* Copy PCM into a DMA-visible buffer and describe it with a BDL. */
static int stream_prepare(hda_t *h, const INT16 *pcm, UINTN frames) {
    UINTN bytes = frames * HDA_CHANNELS * sizeof(INT16);
    if (bytes == 0) return -1;

    /* Cyclic buffer length must be a multiple of 128 bytes; round up, tail plays
     * as silence. */
    UINTN padded = (bytes + 127) & ~(UINTN)127;
    UINTN pages = (padded + 4095) / 4096;

    void *host = NULL;
    EFI_STATUS st = h->pci->AllocateBuffer(h->pci, AllocateAnyPages,
                                           EfiBootServicesData, pages,
                                           &host, 0);
    if (EFI_ERROR(st) || !host) return -1;
    h->samples = (INT16*)host;
    h->samp_pages = pages;

    UINT8 *dst = (UINT8*)host;
    const UINT8 *src = (const UINT8*)pcm;
    for (UINTN i = 0; i < bytes; i++) dst[i] = src[i];
    for (UINTN i = bytes; i < pages * 4096; i++) dst[i] = 0;

    UINTN mapped = pages * 4096;
    EFI_PHYSICAL_ADDRESS dev = 0;
    st = h->pci->Map(h->pci, EfiPciIoOperationBusMasterCommonBuffer, host,
                     &mapped, &dev, &h->samp_map);
    if (EFI_ERROR(st) || mapped < padded) return -1;

    UINT16 gcap = mm_read16(h, REG_GCAP);
    if (!GCAP_64OK(gcap) && (dev + padded) > 0x100000000ULL) return -1;
    h->samp_dev = dev;

    /* Two BDL entries describing halves of the same buffer. One entry works
     * on most controllers but some dislike a single-entry list. */
    UINTN half = padded / 2;
    if (half & 127) half = (half + 127) & ~(UINTN)127;
    if (half == 0 || half >= padded) {
        h->bdl[0].addr  = dev;
        h->bdl[0].len   = (UINT32)padded;
        h->bdl[0].flags = 0;
        mm_write16(h, (UINT32)h->sd_base + SD_LVI, 0);
    } else {
        h->bdl[0].addr  = dev;
        h->bdl[0].len   = (UINT32)half;
        h->bdl[0].flags = 0;
        h->bdl[1].addr  = dev + half;
        h->bdl[1].len   = (UINT32)(padded - half);
        h->bdl[1].flags = 0;
        mm_write16(h, (UINT32)h->sd_base + SD_LVI, 1);
    }

    UINT32 base = (UINT32)h->sd_base;
    mm_write32(h, base + SD_BDLPL, (UINT32)(h->ring_dev + DMA_BDL_OFF));
    mm_write32(h, base + SD_BDLPU, (UINT32)((h->ring_dev + DMA_BDL_OFF) >> 32));
    mm_write32(h, base + SD_CBL, (UINT32)padded);
    mm_write16(h, base + SD_FMT, STREAM_FORMAT);

    /* Our stream tag goes in CTL bits 20..23, alongside the run bit. */
    UINT32 ctl = mm_read32(h, base + SD_CTL);
    ctl &= 0x00F0FFFFu;
    ctl |= ((UINT32)STREAM_TAG << 20);
    mm_write32(h, base + SD_CTL, ctl);

    /* Clear any stale status bits before we start. */
    mm_write8(h, base + SD_STS, SDSTS_BCIS | SDSTS_FIFOE | SDSTS_DESE);
    return 0;
}

static void stream_start(hda_t *h) {
    UINT32 base = (UINT32)h->sd_base;
    UINT32 ctl = mm_read32(h, base + SD_CTL);
    mm_write32(h, base + SD_CTL, ctl | SDCTL_RUN);
    h->stream_running = 1;
}

/* In-flight audio (stream FIFO + codec pipeline) continues after RUN clears;
 * teardown must wait it out or the tail of the sound is clipped. */
static UINT64 output_latency_us(hda_t *h) {
    UINT64 us = 0;
    if (h->sd_base) {
        /* SD_FIFOS holds the FIFO size in 32-bit words, less one. Our format is
         * 48 kHz, two channels, 16 bit - 192 bytes per millisecond, which is
         * 48 words per millisecond. */
        UINT32 fifo = (UINT32)mm_read16(h, (UINT32)h->sd_base + SD_FIFOS) + 1;
        us = (UINT64)fifo * 1000ULL / 48ULL;
    }
    return us + CODEC_PIPELINE_US;
}

static void stream_stop(hda_t *h) {
    if (!h->stream_running) return;
    UINT32 base = (UINT32)h->sd_base;
    mm_write32(h, base + SD_CTL, mm_read32(h, base + SD_CTL) & ~SDCTL_RUN);
    h->stream_running = 0;
}

/* Wait for the DMA engine to walk once through the buffer. The position
 * register is the only progress signal we have without interrupts. */
static int stream_drain(hda_t *h, UINTN bytes) {
    UINT32 base = (UINT32)h->sd_base;

    /* Expected duration, plus a little slack, capped by the global budget. */
    UINTN frames = bytes / (HDA_CHANNELS * sizeof(INT16));
    UINT64 expect_us = (UINT64)frames * 1000000ULL / HDA_SAMPLE_RATE;
    UINT64 start = g_stream_start_us;
    UINT64 limit = start + expect_us + 100000ULL;

    UINT32 last = mm_read32(h, base + SD_LPIB);
    UINT64 last_change = start;
    int moved = 0;

    for (;;) {
        UINT64 now = arch_now_us();
        UINT32 pos = mm_read32(h, base + SD_LPIB);

        if (pos != last) { moved = 1; last = pos; last_change = now; }

        /* Done when we have gone round: position wraps back toward zero. */
        if (moved && now - start >= expect_us) break;

        if (now > limit) break;
        if (!budget_left(h)) return -1;

        /* Position frozen for long enough that the stream is clearly not
         * running. Bail rather than burn the rest of the budget. */
        if (!moved && now - last_change > 50000ULL) return -1;

        spin_us(200);
    }
    return moved ? 0 : -1;
}

/* ----------------------------------------------------------- diagnostics */

/* Log the codec state an OS driver inherits from us. GPIOs often gate the
 * external amp's mute line, so this shows whether we disturbed what the
 * firmware set at POST. Compare with Linux's /proc/asound/card0/codec#0. */
static void hda_log_state(hda_t *h, const CHAR16 *when) {
    if (!h->corb || !h->afg_nid) return;

    CHAR16 msg[192];
    UINT32 count = codec_param(h, h->afg_nid, P_GPIO_COUNT);
    UINT32 mask = 0, dir = 0, data = 0, sticky = 0;

    codec_cmd(h, VERB12(h->afg_nid, V_GET_GPIO_MASK, 0),   &mask);
    codec_cmd(h, VERB12(h->afg_nid, V_GET_GPIO_DIR, 0),    &dir);
    codec_cmd(h, VERB12(h->afg_nid, V_GET_GPIO_DATA, 0),   &data);
    codec_cmd(h, VERB12(h->afg_nid, V_GET_GPIO_STICKY, 0), &sticky);

    SPrint(msg, sizeof(msg),
           L"hda: %s - codec %04x afg 0x%02x, gpio n=%u mask=%02x dir=%02x "
           L"data=%02x sticky=%02x",
           when, (unsigned int)h->vendor, (unsigned int)h->afg_nid,
           (unsigned int)(count & 0xFF), (unsigned int)(mask & 0xFF),
           (unsigned int)(dir & 0xFF), (unsigned int)(data & 0xFF),
           (unsigned int)(sticky & 0xFF));
    efi_log(msg);

    if (h->pin_nid) {
        UINT32 ctl = 0, eapd = 0;
        codec_cmd(h, VERB12(h->pin_nid, V_GET_PIN_CTL, 0), &ctl);
        codec_cmd(h, VERB12(h->pin_nid, V_GET_EAPD, 0),    &eapd);
        SPrint(msg, sizeof(msg),
               L"hda: %s - pin 0x%02x ctl=%02x eapd=%02x",
               when, (unsigned int)h->pin_nid,
               (unsigned int)(ctl & 0xFF), (unsigned int)(eapd & 0xFF));
        efi_log(msg);
    }
}

/* -------------------------------------------------------------- teardown */

/* Mute route output amps, pin first. Not journalled: restore overwrites it
 * all, and the analog path is silent before anything else moves (no clicks). */
static void mute_path(hda_t *h) {
    for (UINTN n = h->path_len; n > 0; n--) {
        UINT8 nid = h->path[n - 1];
        widget_t *w = widget_for(h, nid);
        if (!w || !(w->caps & WCAP_OUT_AMP)) continue;

        codec_set(h, VERB4(nid, V_SET_AMP_GAIN,
                           AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT |
                           AMP_MUTE));
    }
}

/* Replay the journal in reverse. Returns how many registers went back. */
static UINTN restore_codec(hda_t *h) {
    UINTN done = 0;
    for (UINTN n = h->undo_count; n > 0; n--)
        if (codec_set(h, h->undo[n - 1].cmd) == 0) done++;
    return done;
}

/* Honest restore impossible (codec was reset / journal overflowed): mute,
 * drop the pin, power the function group down. Guarantees silence; the OS
 * driver powers it back up when it loads. */
static void quiet_codec(hda_t *h) {
    mute_path(h);
    if (h->pin_nid) {
        codec_set(h, VERB12(h->pin_nid, V_SET_PIN_CTL, 0));
        codec_set(h, VERB12(h->pin_nid, V_SET_EAPD, 0));
    }
    codec_set(h, VERB12(h->afg_nid, V_SET_POWER_STATE, PS_D3HOT));
}

static void hda_cleanup(hda_t *h) {
    if (!h->pci) return;

    /* Stop the DMA and confirm it: the sample buffer is freed below, and a stream
     * still walking it would play back whatever the OS puts there next. */
    int was_playing = h->stream_running;
    stream_stop(h);
    int dma_stuck = 0;
    if (h->sd_base) {
        UINT32 base = (UINT32)h->sd_base;
        UINT64 start = arch_now_us();
        while (mm_read8(h, base + SD_CTL) & SDCTL_RUN) {
            if (arch_now_us() - start > CODEC_TIMEOUT_US) {
                dma_stuck = 1;
                efi_log(L"WARN: hda: stream will not stop - leaking its DMA "
                        L"buffer rather than letting the engine read freed "
                        L"memory");
                break;
            }
            spin_us(10);
        }
    }

    /* Let in-flight samples drain before muting, or the tail is clipped. Skip when
     * the stream only ran for bring-up (hda_probe) - nothing in flight. */
    if (was_playing && h->sd_base)
        spin_us((UINTN)output_latency_us(h));

    /* Codec side. Needs the command ring, so skip it entirely if bring-up never
     * got that far. */
    if (h->corb && h->afg_nid) {
        mute_path(h);
        if (h->pin_nid) {
            codec_set(h, VERB12(h->pin_nid, V_SET_PIN_CTL, 0));
            codec_set(h, VERB12(h->pin_nid, V_SET_EAPD, 0));
        }

        if (h->state_lost || h->undo_partial) {
            /* Either the firmware's codec state was already gone before we
             * could record it, or we recorded only part of it. Restoring half a
             * snapshot is worse than not pretending to. */
            efi_log(h->undo_partial
                    ? L"WARN: hda: undo journal incomplete - leaving the codec "
                      L"quiet instead of half-restored"
                    : L"hda: codec state was already lost - leaving it quiet "
                      L"rather than half-restored");
            quiet_codec(h);
        } else {
            UINTN done = restore_codec(h);
            CHAR16 msg[128];
            SPrint(msg, sizeof(msg),
                   L"hda: restored %u of %u codec registers",
                   (unsigned int)done, (unsigned int)h->undo_count);
            efi_log(msg);
            if (done != h->undo_count)
                efi_log(L"WARN: hda: some codec registers would not go back");
        }
        h->undo_count = 0;

        hda_log_state(h, L"handing back");
    }

    /* Controller side: reset our descriptor first so its pointers stop
     * referring to memory we are about to free, then restore every register we
     * overwrote. No GCTL/function reset here - it would wipe the codec state we
     * just preserved for the OS driver. */
    if (h->sd_base && !dma_stuck) {
        UINT32 base = (UINT32)h->sd_base;
        stream_reset(h);
        mm_write32(h, base + SD_BDLPL, 0);
        mm_write32(h, base + SD_BDLPU, 0);
        mm_write32(h, base + SD_CBL, 0);
        mm_write16(h, base + SD_LVI, 0);

        if (h->hw.valid) {
            mm_write32(h, base + SD_BDLPL, h->hw.sd_bdlpl);
            mm_write32(h, base + SD_BDLPU, h->hw.sd_bdlpu);
            mm_write32(h, base + SD_CBL,   h->hw.sd_cbl);
            mm_write16(h, base + SD_LVI,   h->hw.sd_lvi);
            mm_write16(h, base + SD_FMT,   h->hw.sd_fmt);
            /* Run/reset bits masked off: we destroyed a firmware stream by borrowing the
             * descriptor, so restarting it would be worse than leaving it stopped. */
            mm_write32(h, base + SD_CTL,
                       h->hw.sd_ctl & ~(UINT32)(SDCTL_RUN | SDCTL_SRST));
        }
    }

    mm_write8(h, REG_CORBCTL, 0);
    mm_write8(h, REG_RIRBCTL, 0);
    mm_write32(h, REG_DPLBASE, 0);
    mm_write32(h, REG_DPUBASE, 0);

    if (h->hw.valid) {
        mm_write8(h,  REG_CORBSIZE,  h->hw.corb_size);
        mm_write32(h, REG_CORBLBASE, h->hw.corb_lbase);
        mm_write32(h, REG_CORBUBASE, h->hw.corb_ubase);
        mm_write16(h, REG_CORBWP,    h->hw.corb_wp);
        mm_write8(h,  REG_RIRBSIZE,  h->hw.rirb_size);
        mm_write32(h, REG_RIRBLBASE, h->hw.rirb_lbase);
        mm_write32(h, REG_RIRBUBASE, h->hw.rirb_ubase);
        mm_write16(h, REG_RINTCNT,   h->hw.rintcnt);
        mm_write32(h, REG_DPLBASE,   h->hw.dpl);
        mm_write32(h, REG_DPUBASE,   h->hw.dpu);
        mm_write32(h, REG_INTCTL,    h->hw.intctl);
        mm_write16(h, REG_WAKEEN,    h->hw.wakeen);
        /* Run bits last, after everything they act on is back in place. */
        mm_write8(h, REG_CORBCTL, h->hw.corb_ctl);
        mm_write8(h, REG_RIRBCTL, h->hw.rirb_ctl);
    }
    h->corb = NULL;

    if (!dma_stuck) {
        if (h->samp_map) h->pci->Unmap(h->pci, h->samp_map);
        if (h->samples)  h->pci->FreeBuffer(h->pci, h->samp_pages, h->samples);
        if (h->ring_map) h->pci->Unmap(h->pci, h->ring_map);
        if (h->ring_host) h->pci->FreeBuffer(h->pci, DMA_RING_SIZE / 4096,
                                             h->ring_host);
        h->samp_map = NULL; h->samples = NULL;
        h->ring_map = NULL; h->ring_host = NULL;
    }

    if (h->attrs_saved)
        h->pci->Attributes(h->pci, EfiPciIoAttributeOperationSet,
                           h->saved_attrs, NULL);
}

/* ------------------------------------------------------------- entry points */

/* Shared bring-up: controller, rings, codec, widgets, route. */
static int hda_open(hda_t *h, UINTN budget_ms) {
    arch_clock_init();
    h->start_us = arch_now_us();
    h->deadline_us = h->start_us + (UINT64)budget_ms * 1000ULL;

    /* Defaults codec_cmd() depends on; a zeroed struct would time out every verb. */
    h->cmd_timeout_us = CODEC_TIMEOUT_US;
    h->gain_num = 3;
    h->gain_den = 4;

    h->pci = find_controller();
    if (!h->pci) return HDA_NO_CONTROLLER;

    if (pci_enable(h) != 0) return HDA_HW_ERROR;
    if (controller_acquire(h) != 0) return HDA_HW_ERROR;
    if (rings_init(h) != 0) return HDA_NO_MEMORY;
    if (find_codec(h) != 0) return HDA_NO_CODEC;
    if (enumerate_widgets(h) != 0) return HDA_NO_CODEC;
    if (find_output_path(h) != 0) return HDA_NO_PATH;
    hda_log_state(h, L"found");
    if (!budget_left(h)) return HDA_TIMEOUT;
    return HDA_OK;
}

/* One live playback at a time; a static avoids allocating for a single caller. */
static hda_t g_hda;
static UINTN g_play_bytes;
static UINTN g_active_bytes;

void* hda_play_prepare(const INT16 *pcm, UINTN frames, UINTN active_frames,
                       int *status) {
    int rc = HDA_HW_ERROR;
    if (!pcm || !frames) { if (status) *status = rc; return NULL; }

    for (UINTN i = 0; i < sizeof(g_hda); i++) ((UINT8*)&g_hda)[i] = 0;
    g_play_bytes = 0;
    g_active_bytes = 0;

    rc = hda_open(&g_hda, HDA_SETUP_BUDGET_MS);
    if (rc != HDA_OK) goto fail;

    rc = HDA_HW_ERROR;
    if (configure_path(&g_hda) != 0) goto fail;
    if (stream_reset(&g_hda) != 0)   goto fail;

    if (stream_prepare(&g_hda, pcm, frames) != 0) { rc = HDA_NO_MEMORY; goto fail; }

    g_play_bytes = frames * HDA_CHANNELS * sizeof(INT16);
    if (!active_frames || active_frames > frames) active_frames = frames;
    g_active_bytes = active_frames * HDA_CHANNELS * sizeof(INT16);

    if (status) *status = HDA_OK;
    return &g_hda;

fail:
    hda_cleanup(&g_hda);
    if (status) *status = rc;
    return NULL;
}

/* Release the DMA engine. The time budget starts here, not at prepare(). */
int hda_play_start(void *handle) {
    if (handle != &g_hda || !g_play_bytes) return HDA_HW_ERROR;
    if (g_hda.stream_running) return HDA_OK;

    UINTN frames = g_play_bytes / (HDA_CHANNELS * sizeof(INT16));
    UINT64 play_us = (UINT64)frames * 1000000ULL / HDA_SAMPLE_RATE;
    UINT64 now = arch_now_us();

    g_hda.start_us = now;
    UINT64 want = now + play_us + (UINT64)HDA_DRAIN_SLACK_MS * 1000ULL;
    UINT64 ceiling = now + (UINT64)HDA_PLAY_BUDGET_MS * 1000ULL;
    g_hda.deadline_us = want < ceiling ? want : ceiling;

    stream_start(&g_hda);
    g_stream_start_us = arch_now_us();
    return HDA_OK;
}

/* Has the audible part played? The audible samples end at expect_us, but the
 * FIFO and codec pipeline still hold some of them, and the menu path tears down
 * right after this returns - otherwise the tail is clipped. */
int hda_play_done(void *handle) {
    if (handle != &g_hda || !g_hda.stream_running) return 1;
    if (!budget_left(&g_hda)) return 1;

    UINTN frames = g_active_bytes / (HDA_CHANNELS * sizeof(INT16));
    UINT64 expect_us = (UINT64)frames * 1000000ULL / HDA_SAMPLE_RATE;
    return (arch_now_us() - g_stream_start_us) >= expect_us + output_latency_us(&g_hda);
}

void* hda_play_begin(const INT16 *pcm, UINTN frames, UINTN active_frames,
                     int *status) {
    void *h = hda_play_prepare(pcm, frames, active_frames, status);
    if (!h) return NULL;
    hda_play_start(h);
    return h;
}

/* Stop early and free the hardware, without waiting for the sound to end. */
int hda_play_cut(void *handle) {
    if (handle != &g_hda) return HDA_HW_ERROR;
    hda_cleanup(&g_hda);
    return HDA_OK;
}

int hda_play_end(void *handle) {
    if (handle != &g_hda) return HDA_HW_ERROR;

    /* Nothing was started; just give the hardware back. */
    if (!g_hda.stream_running) {
        g_hda.deadline_us = arch_now_us() + (UINT64)HDA_CLEANUP_BUDGET_MS * 1000ULL;
        hda_cleanup(&g_hda);
        return HDA_OK;
    }

    int drained = stream_drain(&g_hda, g_active_bytes);
    int timed_out = !budget_left(&g_hda);

    /* Restore verbs need the codec alive; give teardown its own budget - the play
     * deadline is spent. */
    g_hda.deadline_us = arch_now_us() + (UINT64)HDA_CLEANUP_BUDGET_MS * 1000ULL;
    hda_cleanup(&g_hda);

    if (drained != 0) return timed_out ? HDA_TIMEOUT : HDA_HW_ERROR;
    return HDA_OK;
}

int hda_play_pcm(const INT16 *pcm, UINTN frames) {
    int rc = HDA_HW_ERROR;
    void *hh = hda_play_begin(pcm, frames, frames, &rc);
    if (!hh) return rc;
    return hda_play_end(hh);
}

int hda_probe(void) {
    hda_t h = {0};
    int rc = hda_open(&h, HDA_SETUP_BUDGET_MS);
    hda_cleanup(&h);
    return rc;
}

const CHAR16* hda_status_str(int code) {
    switch (code) {
    case HDA_OK:            return L"ok";
    case HDA_NO_CONTROLLER: return L"no HD Audio controller on the PCI bus";
    case HDA_NO_CODEC:      return L"controller up but no codec answered";
    case HDA_NO_PATH:       return L"codec found but no usable output route";
    case HDA_NO_MEMORY:     return L"could not allocate DMA memory";
    case HDA_HW_ERROR:      return L"controller or stream did not come up";
    case HDA_TIMEOUT:       return L"ran out of time budget";
    case HDA_UNSUPPORTED:   return L"unsupported on this architecture";
    default:                return L"unknown error";
    }
}

#endif /* __x86_64__ */
