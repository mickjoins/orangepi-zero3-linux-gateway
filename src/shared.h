#ifndef SHARED_H
#define SHARED_H

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#include "sysinfo.h"

typedef struct {
    double temperature_c;
    double humidity_pct;
    int    valid;              /* 1 when a real sensor produced the numbers */
    time_t timestamp;
} sensor_sample_t;

/* Application wide shared state, protected by mutex. */
void shared_init(void);
void shared_destroy(void);

/* running / stop flag for background threads */
void shared_set_running(int running);
int  shared_is_running(void);

/* system information (collector) */
void shared_set_sysinfo(const system_info_t *info);
int  shared_get_sysinfo(system_info_t *info);

/* environment sensor */
void shared_set_sensor(const sensor_sample_t *sample);
int  shared_get_sensor(sensor_sample_t *sample);

/* LED */
void shared_set_led(int value);       /* 0 or 1 */
int  shared_get_led(void);

/* button */
void shared_button_set_initial(int pressed); /* state only; no press count */
void shared_button_set(int pressed);  /* called by button monitor */
int  shared_button_pressed_count(void);
int  shared_button_current(void);

/* latest serial line, zero terminated */
void shared_set_serial_line(const char *line);
void shared_get_serial_line(char *out, size_t out_size);

#endif /* SHARED_H */
