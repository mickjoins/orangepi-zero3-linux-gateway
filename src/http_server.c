#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "gpio_dev.h"
#include "log.h"
#include "shared.h"
#include "version.h"

#define HTTP_MAX_HEADER 2048
#define HTTP_MAX_TOKEN  256
#define HTTP_JSON_SIZE  4096
#define HTTP_TIMEOUT_MS 3000
#define HTTP_POLL_MS    200

static pthread_t g_thread;
static int g_thread_started = 0;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static pthread_mutex_t g_socket_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_access_token[HTTP_MAX_TOKEN];
static int g_http_port;

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
"let apiToken=sessionStorage.getItem('opiz3-token')||'';"
"let authDismissed=false;"
"async function apiFetch(path,opts={}){"
" let headers={...(opts.headers||{}),'X-Requested-With':'XMLHttpRequest'};"
" if(apiToken)headers.Authorization='Bearer '+apiToken;"
" let r=await fetch(path,{...opts,headers});"
" if(r.status===401&&!authDismissed){"
"  let entered=prompt('请输入 API 访问令牌');"
"  if(entered===null){authDismissed=true;return r;}"
"  apiToken=entered.trim();sessionStorage.setItem('opiz3-token',apiToken);"
"  headers.Authorization='Bearer '+apiToken;"
"  r=await fetch(path,{...opts,headers});"
"  if(r.status===401){sessionStorage.removeItem('opiz3-token');apiToken='';authDismissed=true;alert('访问令牌无效，请重试。');}"
" }"
" return r;"
"}"
"async function ledCtl(s){try{authDismissed=false;let r=await apiFetch('/api/gpio?state='+s,{method:'POST'});if(r.ok)update(await r.json());}catch(e){}}"
"async function refresh(){try{let r=await apiFetch('/api/status');if(r.ok)update(await r.json());}catch(e){}}"
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
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static int http_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void http_send_extra(int fd, int code, const char *content_type,
                            const char *body, const char *extra_headers)
{
    char header[512];
    size_t body_len = strlen(body ? body : "");
    int n;

    n = snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Server: opiz3-gateway/%s\r\n"
             "Content-Type: %s; charset=utf-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-cache\r\n"
             "%s"
             "\r\n",
             code, http_code_text(code), APP_VERSION, content_type, body_len,
             extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof(header)) return;

    if (http_write_all(fd, header, strlen(header)) != 0) return;
    if (body && body_len > 0) {
        (void)http_write_all(fd, body, body_len);
    }
}

static void http_send(int fd, int code, const char *content_type, const char *body)
{
    http_send_extra(fd, code, content_type, body, NULL);
}

static void http_send_json_error(int fd, int code, const char *message,
                                 const char *extra_headers)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    if (n < 0 || (size_t)n >= sizeof(body)) return;
    http_send_extra(fd, code, "application/json", body, extra_headers);
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

static int http_path_matches(const char *path, const char *route)
{
    size_t n = strlen(route);

    return strncmp(path, route, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

/* ------------------------------------------------------------------ */
/* JSON builders                                                       */
/* ------------------------------------------------------------------ */

static int json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    static const char hex[] = "0123456789abcdef";

    if (!src || !dst || dst_sz == 0) return -1;
    while (*src) {
        unsigned char ch = (unsigned char)*src++;
        size_t needed = ch < 0x20 ? 6 : (ch == '\\' || ch == '"' ? 2 : 1);

        if (needed >= dst_sz - i) {
            dst[i] = '\0';
            return -1;
        }
        if (ch < 0x20) {
            dst[i++] = '\\';
            dst[i++] = 'u';
            dst[i++] = '0';
            dst[i++] = '0';
            dst[i++] = hex[ch >> 4];
            dst[i++] = hex[ch & 0x0f];
        } else {
            if (ch == '\\' || ch == '"') dst[i++] = '\\';
            dst[i++] = (char)ch;
        }
    }
    dst[i] = '\0';
    return 0;
}

static int build_gpio_json(char *out, size_t out_sz)
{
    int led = gpio_dev_get_led();
    int n;

    n = snprintf(out, out_sz,
             "{\"led\":%d,\"button_current_pressed\":%d,\"button_pressed_count\":%d}",
             led, shared_button_current(), shared_button_pressed_count());
    return n >= 0 && (size_t)n < out_sz ? 0 : -1;
}

static int build_status_json(char *out, size_t out_sz)
{
    system_info_t si;
    sensor_sample_t sensor;
    char serial[512];
    char sensor_json[256];
    char serial_escaped[6 * sizeof(serial) + 1];
    char hostname_escaped[6 * sizeof(si.hostname) + 1];
    int  led = gpio_dev_get_led();
    int n;

    shared_get_sysinfo(&si);
    shared_get_sensor(&sensor);
    shared_get_serial_line(serial, sizeof(serial));

    if (json_escape(serial, serial_escaped, sizeof(serial_escaped)) != 0 ||
        json_escape(si.hostname, hostname_escaped, sizeof(hostname_escaped)) != 0) {
        return -1;
    }

    if (sensor.valid) {
        n = snprintf(sensor_json, sizeof(sensor_json),
                 "{\"valid\":1,\"temperature_c\":%.2f,\"humidity_pct\":%.2f}",
                 sensor.temperature_c, sensor.humidity_pct);
    } else {
        n = snprintf(sensor_json, sizeof(sensor_json), "{\"valid\":0}");
    }
    if (n < 0 || (size_t)n >= sizeof(sensor_json)) return -1;

    n = snprintf(out, out_sz,
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
             "\"serial_line\":\"%s\"}",
             APP_NAME, APP_VERSION, hostname_escaped, si.uptime_s,
             si.cpu_count, si.cpu_usage_pct,
             si.loadavg_1m, si.loadavg_5m, si.loadavg_15m,
             si.mem_total_kb, si.mem_avail_kb, si.mem_usage_pct,
             si.soc_temp_c, si.cpu_freq_khz, sensor_json,
             led < 0 ? -1 : led, shared_button_pressed_count(), serial_escaped);
    return n >= 0 && (size_t)n < out_sz ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* request handling                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char method[16];
    char path[512];
    const char *authorization;
    const char *host;
    int host_count;
    const char *requested_with;
} http_request_t;

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read through the header terminator, with a total deadline and size limit. */
static int http_read_head(int fd, char *buf, size_t buf_sz)
{
    size_t used = 0;
    long long now = monotonic_ms();
    long long deadline;

    if (now < 0 || buf_sz < 5) return 400;
    deadline = now + HTTP_TIMEOUT_MS;

    for (;;) {
        struct pollfd pfd;
        int wait_ms;
        int ready;
        ssize_t n;
        size_t i;

        if (!shared_is_running()) return -1;
        now = monotonic_ms();
        if (now < 0) return 400;
        if (now >= deadline) return 408;
        wait_ms = (int)(deadline - now);
        if (wait_ms > HTTP_POLL_MS) wait_ms = HTTP_POLL_MS;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return 400;
        }
        if (ready == 0) continue;
        if (pfd.revents & POLLNVAL) return 400;

        n = recv(fd, buf + used, buf_sz - used - 1, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return 400;
        }
        if (n == 0) return 400;
        for (i = 0; i < (size_t)n; i++) {
            if (buf[used + i] == '\0') {
                /* A body may contain NUL; only the request head is parsed. */
                buf[used + i] = '\0';
                if (strstr(buf, "\r\n\r\n")) return 0;
                return 400;
            }
        }
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) return 0;
        if (used == buf_sz - 1) return 431;
    }
}

static int http_parse_request(char *buf, http_request_t *req)
{
    char *line_end = strstr(buf, "\r\n");
    char *head_end = strstr(buf, "\r\n\r\n");
    char *space1;
    char *space2;
    char *line;
    int authorization_seen = 0;

    if (!line_end || !head_end || line_end > head_end) return -1;
    *line_end = '\0';
    space1 = strchr(buf, ' ');
    if (!space1) return -1;
    *space1++ = '\0';
    space2 = strchr(space1, ' ');
    if (!space2) return -1;
    *space2++ = '\0';
    if (!buf[0] || !space1[0] || strchr(space2, ' ') ||
        (strcmp(space2, "HTTP/1.1") != 0 && strcmp(space2, "HTTP/1.0") != 0) ||
        strlen(buf) >= sizeof(req->method) || strlen(space1) >= sizeof(req->path) ||
        space1[0] != '/') {
        return -1;
    }
    snprintf(req->method, sizeof(req->method), "%s", buf);
    snprintf(req->path, sizeof(req->path), "%s", space1);
    req->authorization = NULL;
    req->host = NULL;
    req->host_count = 0;
    req->requested_with = NULL;

    line = line_end + 2;
    while (line < head_end) {
        char *next = strstr(line, "\r\n");
        char *colon;
        char *value;
        char *end;

        if (!next || next > head_end) return -1;
        *next = '\0';
        colon = strchr(line, ':');
        if (!colon || colon == line) return -1;
        *colon = '\0';
        value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (strcasecmp(line, "Authorization") == 0) {
            if (authorization_seen++) return -1;
            req->authorization = value;
        } else if (strcasecmp(line, "Host") == 0) {
            req->host = value;
            req->host_count++;
        } else if (strcasecmp(line, "X-Requested-With") == 0) {
            req->requested_with = value;
        }
        line = next + 2;
    }
    return 0;
}

static int http_host_allowed(const char *host)
{
    const char *port_separator;
    size_t name_len;
    char expected_port[6];

    if (!host || !host[0]) return 0;
    port_separator = strchr(host, ':');
    name_len = port_separator ? (size_t)(port_separator - host) : strlen(host);
    if (name_len != 9 ||
        (strncasecmp(host, "localhost", name_len) != 0 &&
         strncmp(host, "127.0.0.1", name_len) != 0)) {
        return 0;
    }
    if (!port_separator) return 1;
    snprintf(expected_port, sizeof(expected_port), "%d", g_http_port);
    return strcmp(port_separator + 1, expected_port) == 0;
}

/* Fixed work for every supported token length; request length is public. */
static int http_token_matches(const char *authorization)
{
    const char *provided = "";
    size_t provided_len;
    size_t expected_len = strlen(g_access_token);
    volatile unsigned int diff;
    size_t i;

    if (authorization && strncasecmp(authorization, "Bearer ", 7) == 0) {
        provided = authorization + 7;
    }
    provided_len = strlen(provided);
    diff = (unsigned int)(provided_len ^ expected_len);
    for (i = 0; i < HTTP_MAX_TOKEN; i++) {
        unsigned char actual = i < provided_len ? (unsigned char)provided[i] : 0;
        diff |= (unsigned char)g_access_token[i] ^ actual;
    }
    return diff == 0;
}

static int http_require_method(int fd, const char *actual, const char *wanted)
{
    if (strcmp(actual, wanted) == 0) return 1;
    if (strcmp(wanted, "POST") == 0) {
        http_send_json_error(fd, 405, "method not allowed", "Allow: POST\r\n");
    } else {
        http_send_json_error(fd, 405, "method not allowed", "Allow: GET\r\n");
    }
    return 0;
}

static void http_drain_pending_input(int fd)
{
    char scratch[1024];
    size_t drained = 0;

    while (drained < 8192) {
        ssize_t n = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);
        if (n <= 0) return;
        drained += (size_t)n;
    }
}

static void handle_client(int fd)
{
    char buf[HTTP_MAX_HEADER + 1];
    http_request_t req;
    int read_result = http_read_head(fd, buf, sizeof(buf));

    if (read_result < 0) return;
    if (read_result != 0) {
        http_send_json_error(fd, read_result,
                             read_result == 408 ? "request timeout" :
                             read_result == 431 ? "request headers too large" : "bad request",
                             NULL);
        if (read_result == 431) http_drain_pending_input(fd);
        return;
    }
    if (http_parse_request(buf, &req) != 0) {
        http_send_json_error(fd, 400, "bad request", NULL);
        return;
    }

    LOG_INFO("HTTP %s %s", req.method, req.path);

    if (!g_access_token[0]) {
        if (req.host_count != 1) {
            http_send_json_error(fd, 400, "missing or duplicate host", NULL);
            return;
        }
        if (!http_host_allowed(req.host)) {
            http_send_json_error(fd, 403, "invalid host", NULL);
            return;
        }
    }

    if (g_access_token[0] &&
        (strncmp(req.path, "/api/", 5) == 0 || strcmp(req.path, "/api") == 0) &&
        !http_token_matches(req.authorization)) {
        http_send_json_error(fd, 401, "unauthorized",
                             "WWW-Authenticate: Bearer realm=\"opiz3-gateway\"\r\n");
        return;
    }

    if (strcmp(req.path, "/") == 0) {
        if (!http_require_method(fd, req.method, "GET")) return;
        http_send(fd, 200, "text/html", INDEX_HTML);
        return;
    }

    if (http_path_matches(req.path, "/api/status")) {
        char json[HTTP_JSON_SIZE];
        if (!http_require_method(fd, req.method, "GET")) return;
        if (build_status_json(json, sizeof(json)) != 0) {
            http_send_json_error(fd, 500, "json too large", NULL);
            return;
        }
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (http_path_matches(req.path, "/api/sensor")) {
        sensor_sample_t s;
        char json[256];
        int n;
        if (!http_require_method(fd, req.method, "GET")) return;
        shared_get_sensor(&s);
        if (s.valid) {
            n = snprintf(json, sizeof(json),
                     "{\"valid\":1,\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"timestamp\":%ld}",
                     s.temperature_c, s.humidity_pct, (long)s.timestamp);
        } else {
            n = snprintf(json, sizeof(json), "{\"valid\":0}");
        }
        if (n < 0 || (size_t)n >= sizeof(json)) {
            http_send_json_error(fd, 500, "json too large", NULL);
            return;
        }
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (http_path_matches(req.path, "/api/serial")) {
        char line[512];
        char json[HTTP_JSON_SIZE];
        char esc[6 * sizeof(line) + 1];
        int n;
        if (!http_require_method(fd, req.method, "GET")) return;
        shared_get_serial_line(line, sizeof(line));
        if (json_escape(line, esc, sizeof(esc)) != 0) {
            http_send_json_error(fd, 500, "json too large", NULL);
            return;
        }
        n = snprintf(json, sizeof(json), "{\"line\":\"%s\"}", esc);
        if (n < 0 || (size_t)n >= sizeof(json)) {
            http_send_json_error(fd, 500, "json too large", NULL);
            return;
        }
        http_send(fd, 200, "application/json", json);
        return;
    }

    if (http_path_matches(req.path, "/api/gpio")) {
        char state[32] = "";
        int  led = -1;
        int has_state = http_get_query_param(req.path, "state", state, sizeof(state));

        if (!http_require_method(fd, req.method, has_state ? "POST" : "GET")) return;

        if (has_state) {
            int current;

            if (!req.requested_with ||
                strcmp(req.requested_with, "XMLHttpRequest") != 0) {
                http_send_json_error(fd, 403, "X-Requested-With required", NULL);
                return;
            }
            current = gpio_dev_get_led();

            if (strcmp(state, "toggle") == 0) {
                led = (current > 0) ? 0 : 1;
            } else if (strcmp(state, "on") == 0 || strcmp(state, "1") == 0) {
                led = 1;
            } else if (strcmp(state, "off") == 0 || strcmp(state, "0") == 0) {
                led = 0;
            } else {
                http_send_json_error(fd, 400, "invalid state", NULL);
                return;
            }

            if (gpio_dev_set_led(led) != 0) {
                http_send_json_error(fd, 500, "gpio write failed", NULL);
                return;
            }
            shared_set_led(led);
        }

        char json[256];
        if (build_gpio_json(json, sizeof(json)) != 0) {
            http_send_json_error(fd, 500, "json too large", NULL);
            return;
        }
        http_send(fd, 200, "application/json", json);
        return;
    }

    http_send_json_error(fd, 404, "not found", NULL);
}

/* ------------------------------------------------------------------ */
/* server thread                                                       */
/* ------------------------------------------------------------------ */

static void *http_server_thread(void *arg)
{
    int fd = g_listen_fd;

    (void)arg;

    if (fd < 0) {
        LOG_ERROR("HTTP server thread started without a listen socket");
        return NULL;
    }

    while (shared_is_running()) {
        int client = accept(fd, NULL, NULL);
        struct timeval timeout = {
            .tv_sec = HTTP_TIMEOUT_MS / 1000,
            .tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000
        };
        if (client < 0) {
            if (errno == EINTR) continue;
            if (shared_is_running()) {
                LOG_ERROR("accept() failed: %s", strerror(errno));
            }
            break;
        }
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            LOG_ERROR("cannot set HTTP client timeout: %s", strerror(errno));
            close(client);
            continue;
        }
        pthread_mutex_lock(&g_socket_mutex);
        if (!shared_is_running()) {
            pthread_mutex_unlock(&g_socket_mutex);
            close(client);
            break;
        }
        g_client_fd = client;
        pthread_mutex_unlock(&g_socket_mutex);
        handle_client(client);
        pthread_mutex_lock(&g_socket_mutex);
        g_client_fd = -1;
        close(client);
        pthread_mutex_unlock(&g_socket_mutex);
    }

    pthread_mutex_lock(&g_socket_mutex);
    close(fd);
    if (g_listen_fd == fd) g_listen_fd = -1;
    pthread_mutex_unlock(&g_socket_mutex);
    LOG_INFO("HTTP server thread exited");
    return NULL;
}

int http_server_start(int port, const char *bind_ip, const char *access_token)
{
    int fd;
    int reuse = 1;
    struct sockaddr_in addr;
    size_t token_len = access_token ? strlen(access_token) : 0;

    if (g_thread_started) return 0;

    if (port <= 0 || port > 65535) {
        LOG_ERROR("invalid HTTP port: %d", port);
        return -1;
    }

    if (token_len >= sizeof(g_access_token)) {
        LOG_ERROR("HTTP access token is too long");
        return -1;
    }
    if ((!bind_ip || strcmp(bind_ip, "127.0.0.1") != 0) && token_len == 0) {
        LOG_ERROR("HTTP access token is required outside 127.0.0.1");
        return -1;
    }
    memset(g_access_token, 0, sizeof(g_access_token));
    if (token_len > 0) memcpy(g_access_token, access_token, token_len);
    g_http_port = port;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (bind_ip && bind_ip[0]) {
        if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
            LOG_ERROR("invalid HTTP bind IP: %s", bind_ip);
            close(fd);
            return -1;
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind(%s:%d) failed: %s",
                  bind_ip && bind_ip[0] ? bind_ip : "0.0.0.0",
                  port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_listen_fd = fd;

    if (pthread_create(&g_thread, NULL, http_server_thread, NULL) != 0) {
        LOG_ERROR("cannot start http server thread");
        close(fd);
        g_listen_fd = -1;
        return -1;
    }
    g_thread_started = 1;
    LOG_INFO("HTTP server listening on %s:%d",
             bind_ip && bind_ip[0] ? bind_ip : "0.0.0.0", port);
    return 0;
}

void http_server_stop(void)
{
    pthread_mutex_lock(&g_socket_mutex);
    if (g_listen_fd >= 0) (void)shutdown(g_listen_fd, SHUT_RDWR);
    if (g_client_fd >= 0) (void)shutdown(g_client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&g_socket_mutex);

    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
    memset(g_access_token, 0, sizeof(g_access_token));
}
