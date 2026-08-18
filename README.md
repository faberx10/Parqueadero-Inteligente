# Parqueadero Inteligente — ESP32 + Chatbot de Voz

Proyecto parcial de Microcontroladores.

## Estructura del proyecto

```
parqueadero-inteligente/
├── firmware/              -> Código C++ que corre en el ESP32
│   ├── platformio.ini
│   ├── include/
│   │   ├── config.h            (credenciales y pines, NO se sube a git)
│   │   └── config.example.h    (plantilla, sí se sube a git)
│   └── src/
│       └── main.cpp
│
└── webapp/                -> Chatbot de voz + interfaz gráfica (corre en el navegador)
    ├── css/
    └── js/
```

## Requisitos (una sola vez por PC)

1. Instalar **VS Code**.
2. Instalar la extensión **PlatformIO IDE** desde el marketplace de VS Code.
3. Instalar el driver USB del ESP32 si Windows no lo reconoce (CP2102 o CH340, según la placa).

## Cómo levantar el firmware

1. Abrir la carpeta `firmware/` en VS Code (Archivo > Abrir carpeta).
2. Esperar a que PlatformIO indexe el proyecto (ícono de "alien" en la barra lateral izquierda).
3. Copiar `include/config.example.h` como `include/config.h` y poner ahí el WiFi real.
4. Conectar el ESP32 por USB.
5. Compilar y subir: ícono de flecha (→) en la barra inferior de PlatformIO, o `Ctrl+Alt+U`.
6. Abrir el monitor serial: ícono de enchufe, o `Ctrl+Alt+S`. Debe verse la conexión WiFi y las dos tareas (Core 0 y Core 1) imprimiendo cada 2 segundos.

## Cómo levantar el chatbot (webapp)

Por ahora la carpeta está vacía, se arma en el paso 6 del plan. Cuando tengamos el HTML/JS, se sirve con Live Server desde VS Code o `python -m http.server` desde la carpeta `webapp/`.

## Estado del proyecto

- [x] Paso 1: Estructura del proyecto + firmware base con conexión WiFi
- [ ] Paso 2: Un sensor FC-51 en protoboard
- [ ] Paso 3: Los 8 sensores + 74HC595
- [ ] Paso 4: LCD I2C
- [ ] Paso 5: Ventilador y luz inteligente
- [ ] Paso 6: Chatbot de voz + interfaz web
- [ ] Paso 7: Maqueta física
- [ ] Paso 8: Integración final
