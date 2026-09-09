#include "http_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "gpio_dev.h"
#include "log.h"
#include "shared.h"
#include "version.h"

#define HTTP_MAX_HEADER 2048
#define HTTP_MAX_BODY   512

static pthread_t g_thread;
static int g_thread_started = 0;
static int g_listen_fd = -1;

/* ------------------------------------------------------------------ */
/* HTML page                                                           */
/* ------------------------------------------------------------------ */

static const char *INDEX_HTML =
"<!DOCTYPE html>"
"<html lang=\"zh-CN\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>Orange Pi Zero 3 Gateway</title>"
"<style>"
" body{font-family:sans-serif;margin:2rem;background:#0f172a;color:#e2e8f0}"
" h1{font-size:1.5rem;margin-bottom:.5rem}"
" .card{background:#1e293b;border-radius:.75rem;padding:1rem 1.25rem;margin:1rem 0;max-width:720px}"
" .row{display:flex;justify-content:space-between;padding:.35rem 0;border-bottom:1px solid #334155}"
" .row:last-child{border-bottom:none}"
" .label{color:#94a3b8}"
" .value{font-weight:bold;font-family:monospace}"
" .led{display:inline-block;width:12px;height:12px;border-radius:50%;background:#475569;margin-right:6px}"
" .led.on{background:#4ade80}"
" button{background:#2563eb;border:none;color:white;padding:.55rem 1rem;border-radius:.5rem;margin-right:.5rem;cursor:pointer}"
" button.off{background:#dc2626}"
" button.tog{background:#0891b2}"
"</style>"
"</head>"
"<body>"
"<h1>Orange Pi Zero 3 Linux Gateway</h1>"
"<div class=\"card\">"
"  <div class=\"row\"><span class=\"label\">Hostname</span><span class=\"value\" id=\"host\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">Uptime</span><span class=\"value\" id=\"uptime\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">CPU usage</span><span class=\"value\" id=\"cpu\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">CPU frequency</span><span class=\"value\" id=\"freq\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">SoC temperature</span><span class=\"value\" id=\"temp\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">Memory</span><span class=\"value\" id=\"mem\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">Sensor</span><span class=\"value\" id=\"sensor\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">Serial latest</span><span class=\"value\" id=\"serial\">-</span></div>"
"  <div class=\"row\"><span class=\"label\"><span class=\"led\" id=\"leddot\"></span>LED</span><span class=\"value\" id=\"led\">-</span></div>"
"  <div class=\"row\"><span class=\"label\">Button presses</span><span class=\"value\" id=\"btn\">0</span></div>"
"</div>"
"<div class=\"card\">"
"  <button onclick=\"ledCtl('on')\">LED ON</button>"
"  <button class=\"off\" onclick=\"ledCtl('off')\">LED OFF</button>"
"  <button class=\"tog\" onclick=\"ledCtl('toggle')\">TOGGLE</button>"
"</div>"
"<script>"
"function ledCtl(s){fetch('/api/gpio?state='+s,{method:'POST'}).then(r=>r.json()).then(j=>update(j));}"
"async function refresh(){try{let r=await fetch('/api/status');let j=await r.json();update(j);}catch(e){}}"
"function update(j){"
" document.getElementById('host').textContent=j.hostname||'-';"
" document.getElementById('uptime').textContent=j.uptime_s?j.uptime_s+' s':'-';"
" document.getElementById('cpu').textContent=(j.cpu_usage_pct!=null?j.cpu_usage_pct.toFixed(1)+'%':'-')+' / '+j.cpu_count+' cores';"
" document.getElementById('freq').textContent=j.cpu_freq_khz>0?(j.cpu_freq_khz/1000).toFixed(0)+' MHz':'-';"
" document.getElementById('temp').textContent=j.soc_temp_c>-900?j.soc_temp_c.toFixed(1)+' C':'-';"
" document.getElementById('mem').textContent=j.mem_total_kb>0?(j.mem_usage_pct.toFixed(1)+'% ('+(j.mem_avail_kb/1024).toFixed(0)+' MB free of '+(j.mem_total_kb/1024).toFixed(0)+' MB)'):'-';"
" let s=j.sensor&&j.sensor.valid?(j.sensor.temperature_c.toFixed(1)+' C, '+j.sensor.humidity_pct.toFixed(1)+' %RH'):'-';"
" document.getElementById('sensor').textContent=s;"
" document.getElementById('serial').textContent=j.serial_line||'-';"
" document.getElementById('led').textContent=j.led?'ON':'OFF';"
" document.getElementById('leddot').className='led'+(j.led?' on':'');"
" document.getElementById('btn').textContent=j.button_pressed_count||0;"
"}"
"setInterval(refresh,2000);refresh();"
"</script>"
"</body>"
"</html>";

/* ------------------------------------------------------------------ */
/* tiny helpers                                                        */
/* ------------------------------------------------------------------ */

static const char *http_code_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void http_send(int fd, int code, const char *content_type, const char *body)
{
    char header[512];
    size_t body_len = strlen(body ? body : "");

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Server: opiz3-gateway/%s\r\n"
             "Content-Type: %s; charset=utf-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-cache\r\n"
             "\r\n",
             code, http_code_text(code), APP_VERSION, content_type, body_len);

    ssize_t w;
    w = write(fd, header, strlen(header));
    (void)w;
    if (body && body_len > 0) {
        w = write(fd, body, body_len);
        (void)w;
    }
}

static int http_get_query_param(const char *path, const char *name, char *out, size_t out_sz)
{
    const char *q = strchr(path, '?');
    const char *p;

    if (!q) return 0;
    q++;
    out[0] = '\0';

    while ((p = strstr(q, name)) != NULL) {
        if ((p == q || p[-1] == '&' || p[-1] == '?') &&
            p[strlen(name)] == '=') {
            const char *v = p + strlen(name) + 1;
            size_t i = 0;
            while (v[i] && v[i] != '&' && i < out_sz - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return 1;
        }
        q = p + strlen(name);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON builders                                                       */
/* ------------------------------------------------------------------ */

static int json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;

    while (*src && i + 2 < dst_sz) {
        if (*src == '\\' || *src == '"') {
            if (i + 3 >= dst_sz) break;
            dst[i++] = '\\';
        }
        dst[i++] = *src++;
    }
    dst[i] = '\0';
    return (int)i;
}

static void build_gpio_json(char *out, size_t out_sz)
{
    int led = gpio_dev_get_led();

    snprintf(out, out_sz,
             "{\"led\":%d,\"button_current_pressed\":%d,\"button_pressed_count\":%d}",
             led, shared_button_current(), shared_button_pressed_count());
}

static void build_status_json(char *out, size_t out_sz)
{
    system_info_t si;
    sensor_sample_t sensor;
    char serial[512];
    char sensor_json[256];
    int  led = gpio_dev_get_led();

    shared_get_sysinfo(&si);
    shared_get_sensor(&sensor);
    shared_get_serial_line(serial, sizeof(serial));

    if (sensor.valid) {
        snprintf(sensor_json, sizeof(sensor_json),
                 "{\"valid\":1,\"temperature_c\":%.2f,\"humidity_pct\":%.2f}",
                 sensor.temperature_c, sensor.humidity_pct);
    } else {
        snprintf(sensor_json, sizeof(sensor_json), "{\"valid\":0}");
    }

    snprintf(out, out_sz,
             "{"
             "\"app\":\"%s\","
             "\"version\":\"%s\","
             "\"hostname\":\"%s\","
             "\"uptime_s\":%.0f,"
             "\"cpu_count\":%d,"
             "\"cpu_usage_pct\":%.2f,"
             "\"loadavg\":[%.2f,%.2f,%.2f],"
             "\"mem_total_kb\":%ld,"
             "\"mem_avail_kb\":%ld,"
             "\"mem_usage_pct\":%.2f,"
             "\"soc_temp_c\":%.2f,"
             "\"cpu_freq_khz\":%ld,"
             "\"sensor\":%s,"
             "\"led\":%d,"
             "\"button_pressed_count\":%d,"
             "\"serial_line\":\"",
             APP_NAME, APP_VERSION, si.hostname, si.uptime_s,
             si.cpu_count, si.cpu_usage_pct,
             si.loadavg_1m, si.loadavg_5m, si.loadavg_15m,
             si.mem_total_kb, si.mem_avail_kb, si.mem_usage_pct,
             si.soc_temp_c, si.cpu_freq_khz, sensor_json,
             led < 0 ? -1 : led, shared_button_pressed_count());

    /* append escaped serial line and close JSON */
    if (strlen(out) + strlen(serial) + 3 < out_sz) {
        char esc[600];
        json_escape(serial, esc, sizeof(esc));
        strncat(out, esc, out_sz - strlen(out) - 3);
    }
    strncat(out, "\"}", out_sz - strlen(out) - 1);
}

/* ------------------------------------------------------------------ */
/* request handling                                                    */
/* ------------------------------------------------------------------ */

static void handle_client(int fd)
{
    char buf[HTTP_MAX_HEADER];
    ssize_t n;
    char method[8] = "";
    char path[512] = "";
    char *line_end;

    /* Read request head (up to blank line). */
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    if (sscanf(buf, "%7s %511s", method, path) != 2) {
        http_send(fd, 404, "text/plain", "{\"error\":\"bad request\"}");
        return;
    }

    LOG_INFO("HTTP %s %s", method, path);

    line_end = strstr(buf, "\r\n\r\n");
    (void)line_end; /* we do not need headers for this tiny API */

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send(fd, 200, "text/html", INDEX_HTML);
        return;
    }

    if (strcmp(path, "/api/status") == 0) {
        char json[2048];
        build_status_json(json, sizeof(json));
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (strncmp(path, "/api/sensor", 11) == 0) {
        sensor_sample_t s;
        char json[256];
        shared_get_sensor(&s);
        if (s.valid) {
            snprintf(json, sizeof(json),
                     "{\"valid\":1,\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"timestamp\":%ld}",
                     s.temperature_c, s.humidity_pct, (long)s.timestamp);
        } else {
            snprintf(json, sizeof(json), "{\"valid\":0}");
        }
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (strncmp(path, "/api/serial", 11) == 0) {
        char line[512];
        char json[2000];
        char esc[1600];
        shared_get_serial_line(line, sizeof(line));
        json_escape(line, esc, sizeof(esc));
        snprintf(json, sizeof(json), "{\"line\":\"%s\"}", esc);
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (strncmp(path, "/api/gpio", 9) == 0) {
        char state[32] = "";
        int  led = -1;

        http_get_query_param(path, "state", state, sizeof(state));

        if (state[0]) {
            int current = gpio_dev_get_led();

            if (strcmp(state, "toggle") == 0) {
                led = (current > 0) ? 0 : 1;
            } else if (strcmp(state, "on") == 0 || strcmp(state, "1") == 0) {
                led = 1;
            } else if (strcmp(state, "off") == 0 || strcmp(state, "0") == 0) {
                led = 0;
            } else {
                http_send(fd, 404, "application/json", "{\"error\":\"invalid state\"}");
                return;
            }

            if (gpio_dev_set_led(led) != 0) {
                http_send(fd, 500, "application/json", "{\"error\":\"gpio write failed\"}");
                return;
            }
            shared_set_led(led);
        }

        char json[256];
        build_gpio_json(json, sizeof(json));
        http_send(fd, 200, "application/json", json);
        return;
    }

    http_send(fd, 404, "application/json", "{\"error\":\"not found\"}");
}

/* ------------------------------------------------------------------ */
/* server thread                                                       */
/* ------------------------------------------------------------------ */

static void *http_server_thread(void *arg)
{
    int port = *(int *)arg;
    int fd;
    int reuse = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return NULL;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind(:%d) failed: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }

    if (listen(fd, 8) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    g_listen_fd = fd;
    LOG_INFO("HTTP server listening on 0.0.0.0:%d", port);

    while (shared_is_running()) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (shared_is_running()) {
                LOG_ERROR("accept() failed: %s", strerror(errno));
            }
            break;
        }
        handle_client(client);
        close(client);
    }

    close(fd);
    g_listen_fd = -1;
    LOG_INFO("HTTP server thread exited");
    return NULL;
}

int http_server_start(int port, const char *bind_ip)
{
    static int port_arg;
    (void)bind_ip; /* bind 0.0.0.0; config kept for future use */
    port_arg = port;

    if (g_thread_started) return 0;

    if (pthread_create(&g_thread, NULL, http_server_thread, &port_arg) != 0) {
        LOG_ERROR("cannot start http server thread");
        return -1;
    }
    g_thread_started = 1;
    return 0;
}

void http_server_stop(void)
{
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
}
