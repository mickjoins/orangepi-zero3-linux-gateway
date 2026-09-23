#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Starts the HTTP server in a background thread.
 * Returns 0 on success, -1 on failure. */
int http_server_start(int port, const char *bind_ip, const char *access_token);

/* Should be called once before process exit but after http_server thread
 * finished (tracked by shared_is_running). */
void http_server_stop(void);

#endif /* HTTP_SERVER_H */
