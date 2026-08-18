// Copia este archivo como "config.h" en la misma carpeta,
// y coloca ahí tus credenciales reales de WiFi.
// config.h NO se sube a git (está en .gitignore) para que
// cada quien use su propia red sin pisar la del otro.

#ifndef CONFIG_H
#define CONFIG_H

#define WIFI_SSID     "Tenda_6D47D0"
#define WIFI_PASSWORD "Felipe18"

#define PIN_SENSOR_1  32
#define PIN_SENSOR_2  33
#define PIN_SENSOR_3  25
#define PIN_SENSOR_4  26
#define PIN_SENSOR_5  27
#define PIN_SENSOR_6  14
#define PIN_SENSOR_7  13
#define PIN_SENSOR_8  35

#define PIN_595_DATA  23
#define PIN_595_CLOCK 18
#define PIN_595_LATCH 5

#define PIN_VENTILADOR 19
#define PIN_LUZ        4
#define PIN_BUZZER     15

#define LCD_SDA 21
#define LCD_SCL 22
#define LCD_ADDR 0x27

#define TOTAL_CUPOS 8
#define UMBRAL_VENTILADOR 4

#endif
