/*
 * VL53L9 explorer: every sensor setting and transform control, live over the UART.
 *
 * Host -> device, one command per line:
 *   get                       state as JSON
 *   set key=value ...         change settings, rebuild the pipeline, reply with state
 *                             keys: usecase sync power context binning exposure dss
 *                                   dss_index period_us streams, or any transform control name
 *   out bin|ascii|text        frame output: binary packets, ASCII depth map, or fps lines
 *   baud N                    switch baud rate; reverts to 921600 unless a command arrives within 3 s
 *   status                    sensor status registers
 *   ping
 *
 * Device -> host: plain log lines, "$J {json}" lines, and binary frame packets
 *   A5 5A 'V' '9' | u32 len | body[len] | u32 crc32(body)
 *   body = pkt_hdr_t, then per stream: u8 id, u8 type (0 = f32, 1 = u32), u16 0, width * height * 4 bytes
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media-c/stream_buffers.h"
#include "vl53l9.h"
#include "vl53l9_app_common.h"
#include "vl53l9_app_conf.h"
#include "vl53l9_device_conf.h"
#include "vl53l9_interface.h"
#include "vl53l9_transform.h"
#include "vl53l9_utils.h"

#define PROTO_VERSION   (1)
#define DEFAULT_BAUD    (921600U)
#define BAUD_CONFIRM_MS (3000U)
#define MAX_CTRLS       (32)
#define RX_RING_SIZE    (4096U) /* a full profile is one ~1 KB "set" line, arriving while we block on TX */
#define LINE_SIZE       (2048U)

extern UART_HandleTypeDef hlpuart1;

typedef struct {
    const char *name;
    const char *format;
    uint8_t type; /* 0 = float32, 1 = uint32 */
} out_stream_t;

static const out_stream_t k_streams[] = {
    { "depth", "ZF32", 0 },      { "amplitude", "AF32", 0 },   { "ambient", "IF32", 0 },
    { "confidence", "CF32", 0 }, { "reflectance", "RF32", 0 }, { "status", "CU32", 1 },
};
#define NB_STREAMS (sizeof(k_streams) / sizeof(k_streams[0]))

typedef struct {
    char name[48];
    const char *nick;
    const char *description;
    vtid_t type;
    value_t value;
    spec_t spec;
} ctrl_t;

typedef enum { OUT_TEXT, OUT_ASCII, OUT_BIN } out_mode_t;

typedef struct {
    vl53l9_profile_t cfg;
    uint32_t streams; /* bit i = k_streams[i] */
    ctrl_t ctrls[MAX_CTRLS];
    size_t nb_ctrls;
} settings_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t nb_streams;
    uint16_t width;
    uint16_t height;
    uint8_t binning;
    uint8_t dss;
    uint8_t context;
    uint8_t sync;
    uint16_t temperature;
    uint32_t frame_counter;
    uint32_t fps_x100;
} pkt_hdr_t;

static settings_t g_set;
static settings_t g_prev_set;
static out_mode_t g_out = OUT_ASCII;
static uint32_t g_baud = DEFAULT_BAUD;
static uint32_t g_baud_deadline; /* 0 = confirmed */

static vl53l9_device_t *g_dev;
static uint8_t g_calib[VL53L9_CALIB_DATA_SIZE];

static transform_t *g_transform;
static buffer_t g_raw[2];
static stream_buffer_t g_items[1 + NB_STREAMS];
static stream_buffers_t g_sbufs;
static uint16_t g_raw_size, g_out_w, g_out_h;
static bool g_streaming;

/* ---------------------------------------------------------------- UART RX */

static uint8_t g_rx_ring[RX_RING_SIZE];
static volatile uint16_t g_rx_head, g_rx_tail;
static uint8_t g_rx_byte;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart != &hlpuart1) {
        return;
    }
    uint16_t next = (g_rx_head + 1U) % RX_RING_SIZE;
    if (next != g_rx_tail) {
        g_rx_ring[g_rx_head] = g_rx_byte;
        g_rx_head = next;
    }
    HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    /* overrun aborts the reception: re-arm it */
    if (huart == &hlpuart1) {
        HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);
    }
}

static int uart_set_baud(uint32_t baud) {
    fflush(stdout);
    while (__HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_TC) == RESET) {
    }
    HAL_UART_AbortReceive(&hlpuart1);
    hlpuart1.Init.BaudRate = baud;
    int ret = (HAL_UART_Init(&hlpuart1) == HAL_OK) ? 0 : -1;
    if (ret) {
        hlpuart1.Init.BaudRate = g_baud;
        HAL_UART_Init(&hlpuart1);
    } else {
        g_baud = baud;
    }
    HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);
    return ret;
}

static void uart_write(const void *p_data, size_t size) {
    const uint8_t *p = p_data;
    while (size > 0U) {
        uint16_t chunk = (size > 0x8000U) ? 0x8000U : (uint16_t)size;
        HAL_UART_Transmit(&hlpuart1, p, chunk, 5000);
        p += chunk;
        size -= chunk;
    }
}

/* ---------------------------------------------------------------- JSON out */

static void json_str(const char *s) {
    putchar('"');
    for (; (s != NULL) && (*s != '\0'); s++) {
        if ((*s == '"') || (*s == '\\')) {
            putchar('\\');
            putchar(*s);
        } else if ((unsigned char)*s < 0x20U) {
            putchar(' ');
        } else {
            putchar(*s);
        }
    }
    putchar('"');
}

/* newlib-nano printf has no %f */
static void json_float(float f) {
    if (!isfinite(f) || (fabsf(f) >= 2e9f)) {
        printf("null");
    } else if (fabsf(f) >= 1e5f) {
        printf("%ld", (long)f);
    } else {
        long i = lroundf(f * 10000.0f);
        long a = labs(i);
        printf("%s%ld.%04ld", (i < 0) ? "-" : "", a / 10000L, a % 10000L);
    }
}

static void json_value(const value_t *v) {
    switch (v->tid) {
    case VTID_BOOL:
        printf(v->val.v_bool ? "true" : "false");
        break;
    case VTID_UINT32:
        printf("%lu", (unsigned long)v->val.v_uint32);
        break;
    case VTID_INT32:
        printf("%ld", (long)v->val.v_int32);
        break;
    case VTID_FLOAT:
        json_float(v->val.v_float);
        break;
    default:
        printf("null");
        break;
    }
}

static void json_spec_num(vtid_t tid, num_type_t n) {
    switch (tid) {
    case VTID_UINT32:
        printf("%lu", (unsigned long)n.v_uint);
        break;
    case VTID_INT32:
        printf("%ld", (long)n.v_int);
        break;
    case VTID_FLOAT:
        json_float(n.v_float);
        break;
    default:
        printf("null");
        break;
    }
}

static const char *type_name(vtid_t tid) {
    switch (tid) {
    case VTID_BOOL:
        return "bool";
    case VTID_UINT32:
        return "u32";
    case VTID_INT32:
        return "i32";
    case VTID_FLOAT:
        return "f32";
    default:
        return "other";
    }
}

static void json_end(void) {
    printf("}\n");
    fflush(stdout);
}

static void report_error(const char *what, int code) {
    printf("$J {\"t\":\"err\",\"msg\":");
    json_str(what);
    printf(",\"code\":%d", code);
    json_end();
}

static void report_state(void) {
    const vl53l9_profile_t *c = &g_set.cfg;
    printf("$J {\"t\":\"state\",\"sensor\":{\"sync\":%u,\"power\":%u,\"context\":%u,\"period_us\":%lu,"
           "\"binning\":%u,\"exposure\":%u,\"dss\":%u,\"dss_index\":%u},",
           (unsigned)c->sync, (unsigned)c->power, (unsigned)c->context, (unsigned long)c->frame_period_us,
           (unsigned)c->binning, (unsigned)c->exposure_ms, (unsigned)c->dss, (unsigned)c->dss_default_index);
    printf("\"streams\":%lu,\"stream_names\":[", (unsigned long)g_set.streams);
    for (size_t i = 0; i < NB_STREAMS; i++) {
        printf(i ? "," : "");
        json_str(k_streams[i].name);
    }
    printf("],\"w\":%u,\"h\":%u,\"out\":\"%s\",\"baud\":%lu,\"ctrls\":[", g_out_w, g_out_h,
           (g_out == OUT_BIN) ? "bin" : (g_out == OUT_ASCII) ? "ascii" : "text", (unsigned long)g_baud);
    for (size_t i = 0; i < g_set.nb_ctrls; i++) {
        const ctrl_t *k = &g_set.ctrls[i];
        printf(i ? ",{\"n\":" : "{\"n\":");
        json_str(k->name);
        printf(",\"nick\":");
        json_str(k->nick);
        printf(",\"d\":");
        json_str(k->description);
        printf(",\"ty\":\"%s\",\"v\":", type_name(k->type));
        json_value(&k->value);
        if (k->spec.tid != VTID_INVALID) {
            printf(",\"min\":");
            json_spec_num(k->spec.tid, k->spec.min);
            printf(",\"max\":");
            json_spec_num(k->spec.tid, k->spec.max);
        }
        printf("}");
    }
    printf("]");
    json_end();
}

static void report_status(void) {
    vl53l9_status_t s = { 0 };
    int ret = vl53l9_get_status(g_dev, &s);
    if (ret) {
        report_error("get_status", ret);
        return;
    }
    printf("$J {\"t\":\"status\",\"fsm\":%u,\"command\":%u,\"firmware\":%u,\"errors\":{\"vhv_overvoltage\":%u,"
           "\"vhv_undervoltage\":%u,\"spad_supply_overload\":%u,\"hvboost_limit\":%u,\"sof_outside_blanking\":%u,"
           "\"pll_lock\":%u,\"ref_array\":%u,\"internal_fw\":%u},\"laser_driver\":[%u,%u,%u,%u,%u]",
           s.fsm, s.command, s.firmware, s.error.vhv_overvoltage, s.error.vhv_undervoltage,
           s.error.spad_supply_overload, s.error.hvboost_limit, s.error.sof_outside_blanking, s.error.pll_lock,
           s.error.ref_array, s.error.internal_fw, s.laser_driver[0], s.laser_driver[1], s.laser_driver[2],
           s.laser_driver[3], s.laser_driver[4]);
    json_end();
}

/* ---------------------------------------------------------------- pipeline */

static void ctrls_load_defaults(void) {
    const controls_t *list;
    if (transform_get_controls(g_transform, &list)) {
        return;
    }
    g_set.nb_ctrls = 0;
    for (size_t i = 0; (i < controls_size(list)) && (g_set.nb_ctrls < MAX_CTRLS); i++) {
        const control_t *c = controls_get(list, i);
        bool settable = (c->type == VTID_BOOL) || (c->type == VTID_UINT32) || (c->type == VTID_INT32) ||
                        (c->type == VTID_FLOAT);
        if (!settable || !(c->flags & CTRL_FLAGS_WRITABLE)) {
            continue; /* calib-buffer is set separately */
        }
        ctrl_t *k = &g_set.ctrls[g_set.nb_ctrls++];
        strncpy(k->name, c->name, sizeof(k->name) - 1U);
        k->nick = c->nick;
        k->description = c->description;
        k->type = c->type;
        k->value = c->value;
        k->spec = c->spec;
    }
}

static void pipe_teardown(void) {
    if (g_streaming) {
        vl53l9_stop(g_dev);
        g_streaming = false;
    }
    if (g_transform != NULL) {
        transform_finalize(g_transform); /* errors only mean "not in that state" */
        transform_release(g_transform);
        vl53l9_transform_destroy(g_transform);
        g_transform = NULL;
    }
    vl53l9_app_free_buffer(&g_raw[0]);
    vl53l9_app_free_buffer(&g_raw[1]);
    for (size_t i = 1; i < (1U + NB_STREAMS); i++) {
        vl53l9_app_free_buffer(&g_items[i].buffer);
    }
    memset(g_items, 0, sizeof(g_items));
}

static int set_caps(const char *stream, uint16_t w, uint16_t h, const char *format) {
    capabilities_t *caps = vl53l9_app_create_capabilities(w, h, format);
    int ret = transform_set_stream_capabilities(g_transform, stream, caps);
    vl53l9_app_free_capabilities(caps);
    return ret;
}

#define TRY(step, expr)                                                                                          \
    do {                                                                                                         \
        int _r = (expr);                                                                                         \
        if (_r) {                                                                                                \
            report_error(step, _r);                                                                              \
            return _r;                                                                                           \
        }                                                                                                        \
    } while (0)

static int pipe_build(void) {
    vl53l9_profile_t *c = &g_set.cfg;
    uint16_t in_w, in_h;

    if (g_set.streams == 0U) {
        g_set.streams = 1U; /* the transform needs at least one output */
    }

    TRY("sensor profile", vl53l9_utils_set_profile(g_dev, c));
    TRY("raw buffer size", vl53l9_get_raw_buffer_size(c->binning, c->dss, &g_raw_size));
    TRY("binning", vl53l9_utils_get_i3c_resolution(c->binning, c->dss, &in_w, &in_h));
    TRY("binning", vl53l9_utils_get_frame_resolution(c->binning, &g_out_w, &g_out_h));

    g_transform = vl53l9_transform_create();
    if (g_transform == NULL) {
        report_error("transform create", -1);
        return -1;
    }
    TRY("transform initialize", transform_initialize(g_transform));
    if (g_set.nb_ctrls == 0U) {
        ctrls_load_defaults();
    }

    TRY("raw caps", set_caps("raw", in_w, in_h, (c->dss == VL53L9_DSS_DISABLE) ? "3DMX" : "3DMD"));
    for (size_t i = 0; i < NB_STREAMS; i++) {
        if (g_set.streams & (1UL << i)) {
            TRY(k_streams[i].name, set_caps(k_streams[i].name, g_out_w, g_out_h, k_streams[i].format));
        }
    }

    TRY("calib-buffer",
        transform_set_control(g_transform, "calib-buffer", (value_t){ .val.v_ptr = g_calib, .tid = VTID_POINTER }));
    for (size_t i = 0; i < g_set.nb_ctrls; i++) {
        TRY(g_set.ctrls[i].name, transform_set_control(g_transform, g_set.ctrls[i].name, g_set.ctrls[i].value));
    }
    TRY("transform prepare", transform_prepare(g_transform));

    g_raw[0] = vl53l9_app_create_buffer(g_raw_size);
    g_raw[1] = vl53l9_app_create_buffer(g_raw_size);
    size_t n = 0;
    g_items[n++] = (stream_buffer_t){ .name = "raw", .buffer = g_raw[0] };
    for (size_t i = 0; i < NB_STREAMS; i++) {
        if (g_set.streams & (1UL << i)) {
            g_items[n++] = vl53l9_app_create_stream_buffer(k_streams[i].name, g_out_w * g_out_h * 4U);
        }
    }
    g_sbufs = (stream_buffers_t){ .items = g_items, .size = n, .capacity = n, .item_size = sizeof(stream_buffer_t) };

    /* acknowledge to avoid stale interrupt event that may occur after reset */
    platform_acknowledge_event(CONF_DEVICE_ID, PLATFORM_GPIO_IT_EVT);
    TRY("start", vl53l9_start(g_dev));
    g_streaming = true;
    return 0;
}

static int sensor_reset(void) {
    platform_power_reset(CONF_DEVICE_ID);
    platform_assign_dynamic_address();
    TRY("init", vl53l9_init(g_dev));
    TRY("calib data", vl53l9_get_calib_data(g_dev, g_calib));
    return 0;
}

/* Rebuild with g_set; on failure fall back to g_prev_set, then to a full sensor reset. */
static void pipe_rebuild(void) {
    pipe_teardown();
    if (pipe_build() == 0) {
        return;
    }
    g_set = g_prev_set;
    pipe_teardown();
    if (pipe_build() == 0) {
        return;
    }
    pipe_teardown();
    if ((sensor_reset() == 0) && (pipe_build() == 0)) {
        return;
    }
    report_status();
    vl53l9_app_handle_error();
}

/* ---------------------------------------------------------------- commands */

static ctrl_t *ctrl_find(const char *name) {
    for (size_t i = 0; i < g_set.nb_ctrls; i++) {
        if (strcmp(g_set.ctrls[i].name, name) == 0) {
            return &g_set.ctrls[i];
        }
    }
    return NULL;
}

static bool parse_ulong(const char *s, unsigned long *p_out) {
    char *end;
    *p_out = strtoul(s, &end, 0);
    return (*s != '\0') && (*end == '\0');
}

static bool ctrl_parse(ctrl_t *k, const char *s) {
    char *end;
    switch (k->type) {
    case VTID_BOOL:
        if ((strcmp(s, "1") == 0) || (strcmp(s, "true") == 0)) {
            k->value.val.v_bool = true;
        } else if ((strcmp(s, "0") == 0) || (strcmp(s, "false") == 0)) {
            k->value.val.v_bool = false;
        } else {
            return false;
        }
        return true;
    case VTID_UINT32:
        k->value.val.v_uint32 = strtoul(s, &end, 0);
        break;
    case VTID_INT32:
        k->value.val.v_int32 = strtol(s, &end, 0);
        break;
    case VTID_FLOAT:
        k->value.val.v_float = strtof(s, &end);
        break;
    default:
        return false;
    }
    return (*s != '\0') && (*end == '\0');
}

static bool set_key(const char *key, const char *val) {
    vl53l9_profile_t *c = &g_set.cfg;
    unsigned long v;

    ctrl_t *k = ctrl_find(key);
    if (k != NULL) {
        return ctrl_parse(k, val);
    }
    if (!parse_ulong(val, &v)) {
        return false;
    }
    if (strcmp(key, "usecase") == 0) {
        vl53l9_profile_t *p = vl53l9_utils_get_profile((vl53l9_usecase_t)v);
        if (p == NULL) {
            return false;
        }
        *c = *p;
    } else if (strcmp(key, "sync") == 0) {
        c->sync = (vl53l9_sync_mode_t)v;
    } else if (strcmp(key, "power") == 0) {
        c->power = (vl53l9_power_mode_t)v;
    } else if (strcmp(key, "context") == 0) {
        c->context = (vl53l9_context_t)v;
    } else if (strcmp(key, "period_us") == 0) {
        c->frame_period_us = v;
    } else if (strcmp(key, "binning") == 0) {
        c->binning = (uint8_t)v;
    } else if (strcmp(key, "exposure") == 0) {
        c->exposure_ms = (uint16_t)v;
    } else if (strcmp(key, "dss") == 0) {
        c->dss = (vl53l9_dss_mode_t)v;
    } else if (strcmp(key, "dss_index") == 0) {
        c->dss_default_index = (uint8_t)v;
    } else if (strcmp(key, "streams") == 0) {
        g_set.streams = v & ((1UL << NB_STREAMS) - 1U);
    } else {
        return false;
    }
    return true;
}

/* Returns true if the pipeline was rebuilt. */
static bool cmd_exec(char *line) {
    char *cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return false;
    }

    bool known = true;
    bool rebuilt = false;

    if (strcmp(cmd, "ping") == 0) {
        printf("$J {\"t\":\"pong\"");
        json_end();
    } else if (strcmp(cmd, "get") == 0) {
        report_state();
    } else if (strcmp(cmd, "status") == 0) {
        report_status();
    } else if (strcmp(cmd, "out") == 0) {
        char *mode = strtok(NULL, " \t");
        if ((mode != NULL) && (strcmp(mode, "bin") == 0)) {
            g_out = OUT_BIN;
        } else if ((mode != NULL) && (strcmp(mode, "ascii") == 0)) {
            g_out = OUT_ASCII;
        } else if ((mode != NULL) && (strcmp(mode, "text") == 0)) {
            g_out = OUT_TEXT;
        } else {
            report_error("out: expected bin|ascii|text", -1);
        }
        report_state();
    } else if (strcmp(cmd, "baud") == 0) {
        unsigned long baud;
        char *arg = strtok(NULL, " \t");
        if ((arg == NULL) || !parse_ulong(arg, &baud)) {
            report_error("baud: expected a number", -1);
        } else {
            printf("$J {\"t\":\"baud\",\"baud\":%lu", baud);
            json_end();
            if (uart_set_baud(baud)) {
                report_error("baud: unsupported rate", -1);
            } else if (baud != DEFAULT_BAUD) {
                g_baud_deadline = HAL_GetTick() + BAUD_CONFIRM_MS;
                return false; /* this command does not confirm the new rate */
            }
        }
    } else if (strcmp(cmd, "set") == 0) {
        g_prev_set = g_set;
        bool ok = true;
        for (char *tok = strtok(NULL, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
            char *eq = strchr(tok, '=');
            if (eq == NULL) {
                ok = false;
                report_error(tok, -1);
                break;
            }
            *eq = '\0';
            if (!set_key(tok, eq + 1)) {
                ok = false;
                report_error(tok, -1);
                break;
            }
        }
        if (ok) {
            pipe_rebuild();
            rebuilt = true;
        } else {
            g_set = g_prev_set;
        }
        report_state();
    } else {
        known = false;
        report_error(cmd, -1);
    }

    if (known) {
        g_baud_deadline = 0U;
    }
    return rebuilt;
}

static bool cmd_poll(void) {
    static char line[LINE_SIZE];
    static size_t len;
    bool rebuilt = false;

    while (g_rx_tail != g_rx_head) {
        char ch = (char)g_rx_ring[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1U) % RX_RING_SIZE;
        if ((ch == '\n') || (ch == '\r')) {
            if (len > 0U) {
                line[len] = '\0';
                rebuilt |= cmd_exec(line);
                len = 0U;
            }
        } else if (len < (LINE_SIZE - 1U)) {
            line[len++] = ch;
        }
    }

    if ((g_baud_deadline != 0U) && ((int32_t)(HAL_GetTick() - g_baud_deadline) > 0)) {
        g_baud_deadline = 0U;
        uart_set_baud(DEFAULT_BAUD);
        printf("baud not confirmed, back to %lu\n", (unsigned long)DEFAULT_BAUD);
    }
    return rebuilt;
}

/* ---------------------------------------------------------------- output */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t size) {
    crc = ~crc;
    while (size--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static void emit_binary(const vl53l9_meta_t *p_meta, uint32_t fps_x100) {
    size_t plane = (size_t)g_out_w * g_out_h * 4U;
    pkt_hdr_t hdr = {
        .version = PROTO_VERSION,
        .nb_streams = (uint8_t)(g_sbufs.size - 1U),
        .width = g_out_w,
        .height = g_out_h,
        .binning = g_set.cfg.binning,
        .dss = g_set.cfg.dss,
        .context = g_set.cfg.context,
        .sync = g_set.cfg.sync,
        .temperature = p_meta->temperature,
        .frame_counter = p_meta->frame_counter,
        .fps_x100 = fps_x100,
    };
    uint32_t len = sizeof(hdr) + hdr.nb_streams * (4U + plane);
    static const uint8_t magic[4] = { 0xA5, 0x5A, 'V', '9' };

    fflush(stdout);
    uart_write(magic, sizeof(magic));
    uart_write(&len, sizeof(len));
    uart_write(&hdr, sizeof(hdr));
    uint32_t crc = crc32_update(0U, (const uint8_t *)&hdr, sizeof(hdr));

    for (size_t i = 1; i < g_sbufs.size; i++) {
        uint8_t id = 0;
        while (strcmp(k_streams[id].name, g_items[i].name) != 0) {
            id++;
        }
        uint8_t shdr[4] = { id, k_streams[id].type, 0, 0 };
        const uint8_t *data = buffer_get_data(&g_items[i].buffer, 0);
        uart_write(shdr, sizeof(shdr));
        uart_write(data, plane);
        crc = crc32_update(crc, shdr, sizeof(shdr));
        crc = crc32_update(crc, data, plane);
    }
    uart_write(&crc, sizeof(crc));
}

static void emit_frame(uint8_t *p_raw, uint32_t fps_x100) {
    vl53l9_frame_t frame = { 0 };
    if (vl53l9_utils_parse_frame(p_raw, g_raw_size, &frame)) {
        report_error("parse frame", -1);
        return;
    }

    switch (g_out) {
    case OUT_BIN:
        emit_binary(frame.p_metadata, fps_x100);
        break;
    case OUT_ASCII: {
        stream_buffer_t *depth = stream_buffers_find(&g_sbufs, "depth");
        if (depth != NULL) {
            vl53l9_app_print_frame((float *)buffer_get_data(&depth->buffer, 0), g_out_h, g_out_w);
        }
        /* \033[K clears leftovers of a longer previous line */
        printf("Frame n. %lu @ %lu.%02lu fps%s\033[K\n", (unsigned long)frame.p_metadata->frame_counter,
               (unsigned long)(fps_x100 / 100U), (unsigned long)(fps_x100 % 100U),
               (depth == NULL) ? " (enable the depth stream for ASCII)" : "");
        break;
    }
    case OUT_TEXT:
        printf("Frame n. %lu @ %lu.%02lu fps\n", (unsigned long)frame.p_metadata->frame_counter,
               (unsigned long)(fps_x100 / 100U), (unsigned long)(fps_x100 % 100U));
        break;
    }
}

/* ---------------------------------------------------------------- main loop */

void vl53l9_app(void) {
    g_dev = vl53l9_get_device(CONF_DEVICE_ID);
    HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);

    g_set.cfg = *vl53l9_utils_get_profile(CONF_USECASE);
    g_set.cfg.sync = VL53L9_SYNC_MANUAL; /* as in ranging_i3c: one frame per trigger */
    g_set.streams = 1U;                  /* depth */
    g_prev_set = g_set;

    if (sensor_reset() || pipe_build()) {
        vl53l9_app_handle_error();
    }

    uint32_t id = 0;
    vl53l9_get_device_id(g_dev, &id);
    printf("$J {\"t\":\"hello\",\"app\":\"vl53l9-explorer\",\"proto\":%d,\"device_id\":%lu", PROTO_VERSION,
           (unsigned long)id);
    json_end();
    report_state();

    platform_profiler_enable();
    uint32_t last = platform_profiler_get_timestamp();
    uint32_t fps_x100 = 0;
    uint8_t capture = 0;
    bool first = true;

    while (1) {
        if (cmd_poll()) {
            capture = 0;
            first = true;
        }

        uint8_t *p_raw = buffer_get_data(&g_raw[capture], 0);
        uint32_t timeout_ms = (g_set.cfg.frame_period_us / 1000U) + 1000U;

        if (g_set.cfg.sync == VL53L9_SYNC_MANUAL) {
            int ret = vl53l9_trigger_frame(g_dev);
            if (ret) {
                report_error("trigger", ret);
                pipe_rebuild();
                first = true;
                continue;
            }
        }

        if (platform_wait_for_event(CONF_DEVICE_ID, PLATFORM_GPIO_IT_EVT, timeout_ms)) {
            report_error("frame timeout (sync=0 needs an external SYNC_IN)", -1);
            report_status();
            pipe_rebuild();
            first = true;
            continue;
        }
        platform_acknowledge_event(CONF_DEVICE_ID, PLATFORM_GPIO_IT_EVT);

        int ret = vl53l9_get_frame_async(g_dev, p_raw, g_raw_size);
        if (ret) {
            report_error("get_frame_async", ret);
            pipe_rebuild();
            first = true;
            continue;
        }

        /* process and send the previous frame while the DMA fetches this one */
        if (!first) {
            g_items[0].buffer = g_raw[1 - capture];
            ret = transform_process_stream(g_transform, &g_sbufs);
            if (ret) {
                report_error("process", ret);
            } else {
                emit_frame(buffer_get_data(&g_raw[1 - capture], 0), fps_x100);
            }
        }

        if (platform_wait_for_event(CONF_DEVICE_ID, PLATFORM_I3C_DMA_RX_EVT, 1000)) {
            report_error("i3c dma timeout", -1);
            pipe_rebuild();
            first = true;
            continue;
        }
        platform_acknowledge_event(CONF_DEVICE_ID, PLATFORM_I3C_DMA_RX_EVT);
        SCB_InvalidateDCache_by_Addr((uint32_t *)p_raw, g_raw_size);

        ret = vl53l9_get_frame_async_ack(g_dev, p_raw, g_raw_size);
        if (ret) {
            report_error("get_frame_async_ack", ret);
            pipe_rebuild();
            first = true;
            continue;
        }

        uint32_t now = platform_profiler_get_timestamp();
        uint32_t us = platform_profiler_convert_to_us(now - last);
        last = now;
        fps_x100 = (us > 0U) ? (100000000U / us) : 0U;

        capture = 1 - capture;
        first = false;
    }
}
