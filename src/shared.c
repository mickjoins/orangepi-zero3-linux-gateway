#include <stdio.h>
#include "shared.h"

#include <string.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_running = 1;

static system_info_t  g_sysinfo;
static sensor_sample_t g_sensor;
static int g_led = 0;
static int g_button_current = 0;
static int g_button_count = 0;
static char g_serial_line[512] = "";

void shared_init(void)
{
    memset(&g_sysinfo, 0, sizeof(g_sysinfo));
    memset(&g_sensor, 0, sizeof(g_sensor));
    g_sensor.valid = 0;
    g_running = 1;
    g_led = 0;
    g_button_current = 0;
    g_button_count = 0;
    g_serial_line[0] = '\0';
}

void shared_destroy(void)
{
    pthread_mutex_destroy(&g_mutex);
}

void shared_set_running(int running)
{
    g_running = running;
}

int shared_is_running(void)
{
    return g_running;
}

void shared_set_sysinfo(const system_info_t *info)
{
    if (!info) return;
    pthread_mutex_lock(&g_mutex);
    g_sysinfo = *info;
    pthread_mutex_unlock(&g_mutex);
}

int shared_get_sysinfo(system_info_t *info)
{
    if (!info) return -1;
    pthread_mutex_lock(&g_mutex);
    *info = g_sysinfo;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void shared_set_sensor(const sensor_sample_t *sample)
{
    if (!sample) return;
    pthread_mutex_lock(&g_mutex);
    g_sensor = *sample;
    pthread_mutex_unlock(&g_mutex);
}

int shared_get_sensor(sensor_sample_t *sample)
{
    if (!sample) return -1;
    pthread_mutex_lock(&g_mutex);
    *sample = g_sensor;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void shared_set_led(int value)
{
    pthread_mutex_lock(&g_mutex);
    g_led = value ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
}

int shared_get_led(void)
{
    int v;
    pthread_mutex_lock(&g_mutex);
    v = g_led;
    pthread_mutex_unlock(&g_mutex);
    return v;
}

void shared_button_set(int pressed)
{
    pthread_mutex_lock(&g_mutex);
    if (pressed && !g_button_current) {
        g_button_count++;
    }
    g_button_current = pressed ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
}

int shared_button_pressed_count(void)
{
    int v;
    pthread_mutex_lock(&g_mutex);
    v = g_button_count;
    pthread_mutex_unlock(&g_mutex);
    return v;
}

int shared_button_current(void)
{
    int v;
    pthread_mutex_lock(&g_mutex);
    v = g_button_current;
    pthread_mutex_unlock(&g_mutex);
    return v;
}

void shared_set_serial_line(const char *line)
{
    if (!line) return;
    pthread_mutex_lock(&g_mutex);
    snprintf(g_serial_line, sizeof(g_serial_line), "%s", line);
    pthread_mutex_unlock(&g_mutex);
}

void shared_get_serial_line(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    pthread_mutex_lock(&g_mutex);
    snprintf(out, out_size, "%s", g_serial_line);
    pthread_mutex_unlock(&g_mutex);
}
