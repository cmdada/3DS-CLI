#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <3ds.h>

/*
 * 3DS hardware accessors backing the synthetic `hw` tree of the virtio-9p
 * device (see virtio_9p.h).
 *
 * Everything the guest can reach that isn't a real filesystem lives here:
 * power/battery state, the motion sensors, the sliders, console identity,
 * and the three "streaming" peripherals (camera, microphone, speakers).
 *
 * Why files rather than proper virtio device classes: there is no virtio
 * transport for most of this. Battery would need a power-supply driver,
 * the cameras have no virtio analog at all, and virtio-snd is a large
 * multi-queue spec for something the guest only ever wants to blast PCM
 * at. A flat tree of text/binary files needs no guest driver whatsoever —
 * `cat /mnt/hw/battery` works from any language, including shell — and it
 * costs one 9P server we already need for the SD card. The sensors that DO
 * map cleanly onto an existing driver (accelerometer, gyroscope, sliders)
 * are additionally exposed as a real evdev node; see virtio_input.h.
 *
 * Service init policy: the cheap, always-useful services (ptm, mcu, cfg,
 * HID sensors) are brought up once at startup. Camera, microphone and NDSP
 * are initialised lazily on first access and, for the camera, torn down
 * again immediately — they are power-hungry and partly exclusive, so an
 * app that never touches them should never pay for them.
 */

#define HW_CAM_WIDTH    400
#define HW_CAM_HEIGHT   240
#define HW_CAM_BYTES    (HW_CAM_WIDTH * HW_CAM_HEIGHT * 2)

/* Mic capture ring. Sampling runs continuously once started and wraps; the
   guest reads whatever arrived since its last read (stream semantics, the
   file offset is ignored) so a reader that falls behind drops old audio
   rather than lagging further and further behind real time. */
#define HW_MIC_BUF_SIZE 0x10000
#define HW_MIC_ENC_BYTES 2

/* Playback: a small pool of wave buffers cycled round-robin. Writes that
   arrive with no free buffer are dropped rather than blocking, since this
   runs on the emulation thread and stalling here stalls the whole guest. */
#define HW_AUDIO_CHN    0
#define HW_AUDIO_NBUF   4
#define HW_AUDIO_BUFSZ  (16 * 1024)
#define HW_AUDIO_RATE   32730.0f

static struct {
    bool ptmu, mcu, cfgu, sensors;
    bool cam_ready, mic_ready, ndsp_ready;
    bool mic_sampling;
    u8  *mic_buf;
    u32  mic_read_pos;
    ndspWaveBuf audio_wb[HW_AUDIO_NBUF];
    u8  *audio_mem[HW_AUDIO_NBUF];
    int  audio_next;
} hw;

static void hw3ds_init(void) {
    memset(&hw, 0, sizeof(hw));
    hw.ptmu = R_SUCCEEDED(ptmuInit());
    hw.mcu  = R_SUCCEEDED(mcuHwcInit());
    hw.cfgu = R_SUCCEEDED(cfguInit());
    /* The motion sensors are off by default and stay on for the whole
       session; enabling them is cheap and hidScanInput() in the main loop
       refreshes both from shared memory for free. */
    hw.sensors = R_SUCCEEDED(HIDUSER_EnableAccelerometer()) &&
                 R_SUCCEEDED(HIDUSER_EnableGyroscope());
}

static void hw3ds_exit(void) {
    if (hw.mic_sampling) { MICU_StopSampling(); hw.mic_sampling = false; }
    if (hw.mic_ready)  { micExit(); hw.mic_ready = false; }
    if (hw.mic_buf)    { free(hw.mic_buf); hw.mic_buf = NULL; }
    if (hw.ndsp_ready) {
        ndspChnReset(HW_AUDIO_CHN);
        for (int i = 0; i < HW_AUDIO_NBUF; i++)
            if (hw.audio_mem[i]) { linearFree(hw.audio_mem[i]); hw.audio_mem[i] = NULL; }
        ndspExit();
        hw.ndsp_ready = false;
    }
    if (hw.cam_ready)  { camExit(); hw.cam_ready = false; }
    if (hw.sensors)    { HIDUSER_DisableGyroscope(); HIDUSER_DisableAccelerometer(); }
    if (hw.cfgu)       cfguExit();
    if (hw.mcu)        mcuHwcExit();
    if (hw.ptmu)       ptmuExit();
}

/* ------------------------------------------------------------------
 * Text-valued sensor/state files
 * ------------------------------------------------------------------ */

static int hw_rd_battery(char *b, int n) {
    u8 lvl = 0;
    /* mcu gives a real 0-100 percentage; ptm only reports a coarse 0-5
       bucket, so it's the fallback rather than the primary source. */
    if (hw.mcu && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&lvl)))
        return snprintf(b, n, "%u\n", lvl);
    if (hw.ptmu && R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl)))
        return snprintf(b, n, "%u\n", (unsigned)(lvl * 20));
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_battery_voltage(char *b, int n) {
    u8 v = 0;
    if (hw.mcu && R_SUCCEEDED(MCUHWC_GetBatteryVoltage(&v)))
        /* Raw register units are 20mV steps. */
        return snprintf(b, n, "%u\n", (unsigned)v * 20);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_charging(char *b, int n) {
    u8 st = 0;
    if (hw.ptmu && R_SUCCEEDED(PTMU_GetBatteryChargeState(&st)))
        return snprintf(b, n, "%u\n", st ? 1u : 0u);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_adapter(char *b, int n) {
    bool st = false;
    if (hw.ptmu && R_SUCCEEDED(PTMU_GetAdapterState(&st)))
        return snprintf(b, n, "%u\n", st ? 1u : 0u);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_shell(char *b, int n) {
    u8 st = 0;
    if (hw.ptmu && R_SUCCEEDED(PTMU_GetShellState(&st)))
        return snprintf(b, n, "%s\n", st ? "open" : "closed");
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_steps(char *b, int n) {
    u32 steps = 0;
    if (hw.ptmu && R_SUCCEEDED(PTMU_GetTotalStepCount(&steps)))
        return snprintf(b, n, "%lu\n", (unsigned long)steps);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_wifi(char *b, int n) {
    return snprintf(b, n, "%u\n", (unsigned)osGetWifiStrength());
}

static int hw_rd_accel(char *b, int n) {
    accelVector v = {0, 0, 0};
    if (hw.sensors) hidAccelRead(&v);
    return snprintf(b, n, "%d %d %d\n", v.x, v.y, v.z);
}

static int hw_rd_gyro(char *b, int n) {
    angularRate r = {0, 0, 0};
    if (hw.sensors) hidGyroRead(&r);
    return snprintf(b, n, "%d %d %d\n", r.x, r.y, r.z);
}

static int hw_rd_slider_3d(char *b, int n) {
    /* Two decimals of a 0.00-1.00 range: the slider's own resolution is
       coarser than that, so more digits would be invented precision. */
    int milli = (int)(osGet3DSliderState() * 100.0f + 0.5f);
    return snprintf(b, n, "%d.%02d\n", milli / 100, milli % 100);
}

static int hw_rd_slider_volume(char *b, int n) {
    u8 vol = 0;
    if (R_SUCCEEDED(HIDUSER_GetSoundVolume(&vol)))
        return snprintf(b, n, "%u\n", vol);
    if (hw.mcu && R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&vol)))
        return snprintf(b, n, "%u\n", vol);
    return snprintf(b, n, "unknown\n");
}

static const char *hw_model_name(u8 model) {
    switch (model) {
    case 0: return "Old 3DS";
    case 1: return "Old 3DS XL";
    case 2: return "New 3DS";
    case 3: return "Old 2DS";
    case 4: return "New 3DS XL";
    case 5: return "New 2DS XL";
    default: return "unknown";
    }
}

static int hw_rd_model(char *b, int n) {
    u8 m = 0xff;
    if (hw.cfgu && R_SUCCEEDED(CFGU_GetSystemModel(&m)))
        return snprintf(b, n, "%s\n", hw_model_name(m));
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_region(char *b, int n) {
    static const char *rn[] = {"JPN","USA","EUR","AUS","CHN","KOR","TWN"};
    u8 r = 0xff;
    if (hw.cfgu && R_SUCCEEDED(CFGU_SecureInfoGetRegion(&r)) && r < 7)
        return snprintf(b, n, "%s\n", rn[r]);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_language(char *b, int n) {
    static const char *ln[] = {"ja","en","fr","de","it","es","zh","ko","nl","pt","ru","tw"};
    u8 l = 0xff;
    if (hw.cfgu && R_SUCCEEDED(CFGU_GetSystemLanguage(&l)) && l < 12)
        return snprintf(b, n, "%s\n", ln[l]);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_firmware(char *b, int n) {
    OS_VersionBin nver, cver;
    char s[64];
    if (R_SUCCEEDED(osGetSystemVersionDataString(&nver, &cver, s, sizeof(s))))
        return snprintf(b, n, "%s\n", s);
    return snprintf(b, n, "unknown\n");
}

static int hw_rd_info(char *b, int n) {
    char model[32], batt[32], chg[32], wifi[32];
    hw_rd_model(model, sizeof(model));
    hw_rd_battery(batt, sizeof(batt));
    hw_rd_charging(chg, sizeof(chg));
    hw_rd_wifi(wifi, sizeof(wifi));
    /* The sub-reads leave their trailing newline in place; strip it so the
       summary doesn't come out double-spaced. */
    for (char *p = model; *p; p++) if (*p == '\n') *p = 0;
    for (char *p = batt;  *p; p++) if (*p == '\n') *p = 0;
    for (char *p = chg;   *p; p++) if (*p == '\n') *p = 0;
    for (char *p = wifi;  *p; p++) if (*p == '\n') *p = 0;
    return snprintf(b, n,
        "model:    %s\n"
        "battery:  %s%%\n"
        "charging: %s\n"
        "wifi:     %s/3\n",
        model, batt, chg, wifi);
}

/* ------------------------------------------------------------------
 * LEDs (write-only)
 * ------------------------------------------------------------------ */

static int hw_wr_leds(const char *buf, int len) {
    char cmd[64];
    int n = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, buf, n);
    cmd[n] = 0;
    if (!hw.mcu) return -1;

    unsigned a = 0, g = 0, b = 0;
    if (sscanf(cmd, "wifi %u", &a) == 1) {
        return R_SUCCEEDED(MCUHWC_SetWifiLedState(a != 0)) ? len : -1;
    }
    if (sscanf(cmd, "power %u", &a) == 1) {
        return R_SUCCEEDED(MCUHWC_SetPowerLedState((powerLedState)a)) ? len : -1;
    }
    if (sscanf(cmd, "info %u %u %u", &a, &g, &b) == 3) {
        /* Solid colour = the same value in all 32 pattern slots, with
           smoothing off so it doesn't fade between identical entries. */
        InfoLedPattern p;
        memset(&p, 0, sizeof(p));
        p.delay = 0x10; p.smoothing = 0; p.loopDelay = 0; p.blinkSpeed = 0;
        for (int i = 0; i < 32; i++) {
            p.redPattern[i]   = (u8)a;
            p.greenPattern[i] = (u8)g;
            p.bluePattern[i]  = (u8)b;
        }
        return R_SUCCEEDED(MCUHWC_SetInfoLedPattern(&p)) ? len : -1;
    }
    return -1;
}

/* ------------------------------------------------------------------
 * Camera
 * ------------------------------------------------------------------ */

/* Captures one 400x240 RGB565 frame into caller-provided storage.
   Returns bytes written, or 0 on failure. The camera is activated and
   deactivated around each capture: holding it open drains the battery and
   keeps the LED lit, and the guest reads frames far too slowly (this is a
   software-emulated RISC-V machine) for a persistent stream to be worth
   the cost. */
static int hw_capture_frame(u32 select, u32 port, u8 *out) {
    if (!hw.cam_ready) {
        if (R_FAILED(camInit())) return 0;
        hw.cam_ready = true;
    }

    Handle recv = 0;
    int got = 0;
    u32 bufsize = 0;

    if (R_FAILED(CAMU_SetSize(select, SIZE_CTR_TOP_LCD, CONTEXT_A))) goto done;
    if (R_FAILED(CAMU_SetOutputFormat(select, OUTPUT_RGB_565, CONTEXT_A))) goto done;
    CAMU_SetFrameRate(select, FRAME_RATE_30);
    CAMU_SetNoiseFilter(select, true);
    CAMU_SetAutoExposure(select, true);
    CAMU_SetAutoWhiteBalance(select, true);
    CAMU_SetTrimming(port, false);
    if (R_FAILED(CAMU_Activate(select))) goto done;

    if (R_FAILED(CAMU_GetMaxBytes(&bufsize, HW_CAM_WIDTH, HW_CAM_HEIGHT))) goto deactivate;
    CAMU_SetTransferBytes(port, bufsize, HW_CAM_WIDTH, HW_CAM_HEIGHT);
    CAMU_ClearBuffer(port);
    if (R_FAILED(CAMU_SetReceiving(&recv, out, port, HW_CAM_BYTES, (s16)bufsize))) goto deactivate;
    if (R_FAILED(CAMU_StartCapture(port))) goto deactivate;

    /* 1s is comfortably longer than a 30fps frame interval plus the
       sensor's warm-up; if it expires the hardware isn't going to
       produce anything and we'd rather return short than wedge the guest. */
    if (R_SUCCEEDED(svcWaitSynchronization(recv, 1000000000LL)))
        got = HW_CAM_BYTES;

    CAMU_StopCapture(port);

deactivate:
    if (recv) svcCloseHandle(recv);
    CAMU_Activate(SELECT_NONE);
done:
    return got;
}

/* ------------------------------------------------------------------
 * Microphone
 * ------------------------------------------------------------------ */

static bool hw_mic_start(void) {
    if (hw.mic_sampling) return true;
    if (!hw.mic_buf) {
        hw.mic_buf = (u8 *)memalign(0x1000, HW_MIC_BUF_SIZE);
        if (!hw.mic_buf) return false;
        memset(hw.mic_buf, 0, HW_MIC_BUF_SIZE);
    }
    if (!hw.mic_ready) {
        if (R_FAILED(micInit(hw.mic_buf, HW_MIC_BUF_SIZE))) return false;
        hw.mic_ready = true;
    }
    MICU_SetPower(true);
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                    MICU_SAMPLE_RATE_16360,
                                    0, micGetSampleDataSize(), true)))
        return false;
    hw.mic_sampling = true;
    hw.mic_read_pos = 0;
    return true;
}

/* Stream semantics: returns however much new audio has landed in the ring
   since the previous call, so the file offset is deliberately ignored.

   When nothing has arrived yet this returns silence rather than 0. A
   zero-length read is EOF to every POSIX reader, so `cat mic.pcm` would stop
   instantly on the very first read - before the microphone has had a chance
   to produce a single sample. Padding keeps the file behaving like the
   continuous capture stream it is, and keeps the output time-aligned instead
   of splicing out the gaps. It also means a reader has to stop on its own
   terms (`dd count=`, or Ctrl-C), exactly like reading any capture device. */
#define HW_MIC_IDLE_CHUNK 1024

static int hw_mic_read(u8 *out, int max) {
    if (max <= 0) return 0;
    if (!hw_mic_start()) return 0;
    u32 size = micGetSampleDataSize();
    if (!size) return 0;
    u32 wpos = micGetLastSampleOffset();
    if (wpos >= size) wpos = 0;

    u32 avail = (wpos >= hw.mic_read_pos) ? (wpos - hw.mic_read_pos)
                                          : (size - hw.mic_read_pos + wpos);
    if (avail > (u32)max) avail = (u32)max;
    /* Keep whole PCM16 samples together; a half-sample split would make
       every subsequent sample in the stream byte-swapped. */
    avail -= avail % HW_MIC_ENC_BYTES;
    if (!avail) {
        u32 pad = (u32)max < HW_MIC_IDLE_CHUNK ? (u32)max : HW_MIC_IDLE_CHUNK;
        pad -= pad % HW_MIC_ENC_BYTES;
        memset(out, 0, pad);
        return (int)pad;
    }

    u32 first = size - hw.mic_read_pos;
    if (first > avail) first = avail;
    memcpy(out, hw.mic_buf + hw.mic_read_pos, first);
    if (avail > first) memcpy(out + first, hw.mic_buf, avail - first);
    hw.mic_read_pos = (hw.mic_read_pos + avail) % size;
    return (int)avail;
}

/* ------------------------------------------------------------------
 * Audio out
 * ------------------------------------------------------------------ */

static bool hw_audio_start(void) {
    if (hw.ndsp_ready) return true;
    if (R_FAILED(ndspInit())) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(HW_AUDIO_CHN);
    ndspChnSetInterp(HW_AUDIO_CHN, NDSP_INTERP_LINEAR);
    ndspChnSetRate(HW_AUDIO_CHN, HW_AUDIO_RATE);
    ndspChnSetFormat(HW_AUDIO_CHN, NDSP_FORMAT_STEREO_PCM16);
    for (int i = 0; i < HW_AUDIO_NBUF; i++) {
        hw.audio_mem[i] = (u8 *)linearAlloc(HW_AUDIO_BUFSZ);
        if (!hw.audio_mem[i]) { ndspExit(); return false; }
        memset(&hw.audio_wb[i], 0, sizeof(ndspWaveBuf));
        hw.audio_wb[i].status = NDSP_WBUF_DONE;
    }
    hw.ndsp_ready = true;
    return true;
}

/* Accepts signed 16-bit stereo PCM at 32730Hz. Returns the number of bytes
   consumed; a short return means the buffer pool was full. Deliberately
   never blocks - this runs on the emulation thread, so waiting for the DSP
   would freeze the entire guest, and dropping audio is the lesser evil. */
static int hw_audio_write(const u8 *data, int len) {
    if (!hw_audio_start()) return -1;
    int written = 0;
    while (written < len) {
        ndspWaveBuf *wb = &hw.audio_wb[hw.audio_next];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
            break; /* pool full - drop the remainder */

        int chunk = len - written;
        if (chunk > HW_AUDIO_BUFSZ) chunk = HW_AUDIO_BUFSZ;
        chunk -= chunk % 4; /* whole stereo PCM16 frames only */
        if (chunk <= 0) break;

        memcpy(hw.audio_mem[hw.audio_next], data + written, chunk);
        DSP_FlushDataCache(hw.audio_mem[hw.audio_next], chunk);
        memset(wb, 0, sizeof(*wb));
        wb->data_vaddr = hw.audio_mem[hw.audio_next];
        wb->nsamples   = chunk / 4;
        ndspChnWaveBufAdd(HW_AUDIO_CHN, wb);

        hw.audio_next = (hw.audio_next + 1) % HW_AUDIO_NBUF;
        written += chunk;
    }
    /* Report the whole write as consumed even when buffers were dropped:
       a short write would make the guest's userspace retry in a spin loop,
       which is worse for a realtime stream than silently losing a frame. */
    return len;
}
