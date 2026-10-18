#pragma once
typedef void *temperature_sensor_handle_t;
typedef struct { int a, b; } temperature_sensor_config_t;
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(a, b) temperature_sensor_config_t{a, b}
inline int temperature_sensor_install(temperature_sensor_config_t *, temperature_sensor_handle_t *) { return -1; }
inline int temperature_sensor_enable(temperature_sensor_handle_t) { return 0; }
inline int temperature_sensor_get_celsius(temperature_sensor_handle_t, float *) { return -1; }
