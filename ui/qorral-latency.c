/*
 * Input-to-display latency probe (Qorral): x-qorral-latency-probe presses a
 * key and watches a console's display updates until the brightness of one
 * pixel crosses the middle, then reports the time as a QORRAL_LATENCY event.
 *
 * The measured span is from the key event entering QEMU's input layer to the
 * display update that carries the changed frame: the guest's input handling,
 * its rendering and presentation, and the scanout reaching QEMU's display
 * layer. Showing that frame in a host window comes on top.
 *
 * Stage marks split the span (all relative to the key press, 0 = not seen):
 * the guest reading the key from the keyboard device, the guest presenting
 * (a scanout flush or flip; the last one before the matching update counts,
 * every one is counted), and the start of the matching display update before
 * the display listeners run. Key read to present is the guest (input stack,
 * app, rendering, its frame queue); present to update start is the host GPU
 * path; update start to the end is the display listeners.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "qapi/qapi-events-ui.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"

static struct {
    bool armed;
    QemuConsole *con;
    int x, y;           /* < 0: the centre */
    int before, last;   /* brightness 0..255 */
    int64_t t0_us;
    int64_t updates;
    int64_t key_read_us, first_present_us, present_us, update_us, presents;
    QEMUTimer *timeout;
} probe;

static bool probe_tracing(void)
{
    static int on = -1;

    if (on < 0) {
        const char *env = g_getenv("QORRAL_LATENCY_TRACE");
        on = env && *env && strcmp(env, "0") != 0;
    }
    return on;
}

void qorral_latency_trace(const char *what, uint32_t arg)
{
    if (!probe.armed || !probe_tracing()) {
        return;
    }
    fprintf(stderr, "qorral-latency: +%.2f ms %s 0x%x\n",
            (qemu_clock_get_us(QEMU_CLOCK_REALTIME) - probe.t0_us) / 1000.0,
            what, arg);
}

void qorral_latency_mark(QorralLatencyMark mark)
{
    static const char *const names[] = { "key-read", "present", "update" };
    int64_t now_us;

    if (!probe.armed) {
        return;
    }
    qorral_latency_trace(names[mark], 0);
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    switch (mark) {
    case QORRAL_LATENCY_KEY_READ:
        if (!probe.key_read_us) {
            probe.key_read_us = now_us;
        }
        break;
    case QORRAL_LATENCY_PRESENT:
        probe.presents++;
        if (!probe.first_present_us) {
            probe.first_present_us = now_us;
        }
        probe.present_us = now_us;
        break;
    case QORRAL_LATENCY_UPDATE:
        probe.update_us = now_us;
        break;
    }
}

bool qorral_latency_armed(void)
{
    return probe.armed;
}

static int64_t probe_rel(int64_t t_us)
{
    return t_us ? t_us - probe.t0_us : 0;
}

/* Brightness (0..255) of the probe pixel, or -1 when it cannot be read. */
static int probe_brightness(QemuConsole *con, int x, int y)
{
    DisplaySurface *s = qemu_console_surface(con);
    uint8_t rgba[4];

    if (s) {
        int w = surface_width(s), h = surface_height(s);
        pixman_format_code_t f = surface_format(s);
        const uint8_t *p;
        int off;

        x = x < 0 ? w / 2 : x;
        y = y < 0 ? h / 2 : y;
        if (x >= w || y >= h || surface_bytes_per_pixel(s) != 4) {
            return -1;
        }
        /*
         * In memory (little endian) the colour bytes come first, except for
         * formats whose alpha or padding is the lowest byte.
         */
        off = (f == PIXMAN_b8g8r8x8 || f == PIXMAN_b8g8r8a8 ||
               f == PIXMAN_r8g8b8x8 || f == PIXMAN_r8g8b8a8) ? 1 : 0;
        p = (const uint8_t *)surface_data(s) + (size_t)y * surface_stride(s) +
            (size_t)x * 4 + off;
        return (p[0] + p[1] + p[2]) / 3;
    }
    if (!qemu_console_scanout_read_pixel(con, x, y, rgba)) {
        return -1;
    }
    return (rgba[0] + rgba[1] + rgba[2]) / 3;
}

static void probe_finish(int64_t elapsed_us, bool timed_out)
{
    probe.armed = false;
    timer_del(probe.timeout);
    qapi_event_send_qorral_latency(timed_out ? 0 : elapsed_us, probe.updates,
                                   timed_out, probe.before, probe.last,
                                   probe_rel(probe.key_read_us),
                                   probe_rel(probe.first_present_us),
                                   timed_out ? 0 : probe_rel(probe.present_us),
                                   timed_out ? 0 : probe_rel(probe.update_us),
                                   probe.presents);
}

static void probe_timeout(void *opaque)
{
    if (probe.armed) {
        probe_finish(0, true);
    }
}

static void probe_display_update(QemuConsole *con)
{
    int64_t now_us;
    int b;

    if (!probe.armed || con != probe.con) {
        return;
    }
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);  /* before the readback */
    probe.updates++;
    b = probe_brightness(con, probe.x, probe.y);
    qorral_latency_trace("probe-pixel", b);
    if (b < 0) {
        return;
    }
    probe.last = b;
    if ((probe.before < 128) != (b < 128)) {
        probe_finish(now_us - probe.t0_us, false);
    }
}

void qmp_x_qorral_latency_probe(bool has_key, QKeyCode key, bool has_x,
                                int64_t x, bool has_y, int64_t y,
                                bool has_head, int64_t head,
                                bool has_timeout_ms, int64_t timeout_ms,
                                Error **errp)
{
    QemuConsole *con = qemu_console_lookup_by_index(has_head ? head : 0);
    KeyValue kv = { .type = KEY_VALUE_KIND_QCODE };
    unsigned int lnx;
    int b;

    if (!con) {
        error_setg(errp, "there is no console %" PRId64, has_head ? head : 0);
        return;
    }
    if (probe.armed) {
        error_setg(errp, "a latency probe is already running");
        return;
    }
    probe.x = has_x ? x : -1;
    probe.y = has_y ? y : -1;
    b = probe_brightness(con, probe.x, probe.y);
    if (b < 0) {
        error_setg(errp, "the display of console %" PRId64 " cannot be read "
                   "(no surface, and no GL readback for its scanout)",
                   has_head ? head : 0);
        return;
    }
    qemu_console_set_update_hook(probe_display_update);
    if (!probe.timeout) {
        probe.timeout = timer_new_ms(QEMU_CLOCK_REALTIME, probe_timeout, NULL);
    }
    probe.con = con;
    probe.before = probe.last = b;
    probe.updates = 0;
    probe.key_read_us = probe.first_present_us = probe.present_us = 0;
    probe.update_us = probe.presents = 0;
    probe.armed = true;
    timer_mod(probe.timeout, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              (has_timeout_ms && timeout_ms > 0 ? timeout_ms : 2000));

    kv.u.qcode.data = has_key ? key : Q_KEY_CODE_SPC;
    lnx = qemu_input_key_value_to_linux(&kv);
    probe.t0_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    qemu_input_event_send_key_linux(NULL, lnx, true);
    qemu_input_event_sync();
    qemu_input_event_send_key_linux(NULL, lnx, false);
    qemu_input_event_sync();
}
