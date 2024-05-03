#ifndef __OASIS_TIME_H__
#define __OASIS_TIME_H__

#include <time.h>
#include "esp_err.h"

esp_err_t oasis_timer_init(void);
void oasis_timer_exit(void);

bool oasis_timer_initialization_required(void);
time_t oasis_get_systemtime_sec(void);
time_t oasis_get_systemtime_usec(void);
#endif
