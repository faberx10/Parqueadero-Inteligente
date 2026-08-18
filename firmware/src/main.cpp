/*
  ============================================================
  Parqueadero Inteligente - Firmware ESP32
  ============================================================
  El estado se ve por Serial,
  por la pagina web, y por los 16 LEDs rojo/verde.

  Cada cubiculo tiene 3 estados posibles:
    LIBRE      -> LED verde, nadie lo reservo, sensor no detecta
    RESERVADO  -> LED rojo, alguien lo aparto, pero el carro
                  fisico aun no ha llegado (sensor no detecta)
    OCUPADO    -> LED rojo, el sensor SI detecta un carro
                  (haya sido reservado antes o no)

  Visualmente RESERVADO y OCUPADO se ven igual (LED rojo),
  la diferencia esta en el estado logico que reporta el
  sistema por HTTP/voz.

  Regla de transicion automatica:
    - Si esta RESERVADO y el sensor detecta el carro -> pasa a OCUPADO
    - Si esta OCUPADO y el sensor deja de detectar   -> pasa a LIBRE
      (se asume que el carro se fue, la reserva se consume)
    - Si esta LIBRE y el sensor detecta un carro      -> pasa a OCUPADO
      (alguien parqueo sin reservar antes)

  El ventilador SOLO reacciona a carros reales (estado OCUPADO).
  Los cubiculos RESERVADOS no cuentan para el ventilador: si no
  hay carro fisico todavia, no hay nada que refrigerar.

  Endpoints:
    GET  /estado           -> JSON con libres, ocupados, reservados,
                               ventilador, luz, y el detalle de
                               cada uno de los 8 cubiculos
    POST /luz/on            -> enciende la luz inteligente
    POST /luz/off           -> apaga la luz inteligente
    POST /reservar/<1-8>    -> reserva el cubiculo N (si esta libre)
    POST /liberar/<1-8>     -> cancela la reserva del cubiculo N
                                (si aun no ha llegado el carro)
    POST /reservar-todos    -> reserva todos los que esten LIBRE
    POST /liberar-todos     -> libera todos los que esten RESERVADO

  Las variables compartidas entre el Core 0 (que las escribe)
  y el Core 1 (que las lee para responder al navegador) estan
  protegidas con un mutex.

  Mapeo de LEDs (cada 595 = 4 cubiculos completos):
    595 #1 -> cubiculos 1,2,3,4  (Q0=R1 Q1=V1 Q2=R2 Q3=V2 Q4=R3 Q5=V3 Q6=R4 Q7=V4)
    595 #2 -> cubiculos 5,6,7,8  (mismo patron)
    RESERVADO y OCUPADO ambos encienden el LED rojo.

  FC-51: activo bajo (LOW = detecta objeto).

  Core 0 -> hardware (sensores, LEDs, ventilador, luz)
  Core 1 -> red (WiFi, servidor HTTP)
  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"

WebServer servidor(80);

// Mutex para proteger el acceso a variables compartidas entre nucleos
SemaphoreHandle_t mutexEstado;

TaskHandle_t taskHardwareHandle = NULL;
TaskHandle_t taskRedHandle = NULL;

// ------------------------------------------------------------
// Estados posibles de un cubiculo
// ------------------------------------------------------------
enum EstadoCubiculo {
  LIBRE = 0,
  RESERVADO = 1,
  OCUPADO = 2
};

const char* nombreEstado(EstadoCubiculo e) {
  switch (e) {
    case LIBRE: return "LIBRE";
    case RESERVADO: return "RESERVADO";
    case OCUPADO: return "OCUPADO";
  }
  return "LIBRE";
}

// ------------------------------------------------------------
// Pines de los 8 sensores, en orden de cubiculo 1 a 8
// ------------------------------------------------------------
const int pinesSensores[TOTAL_CUPOS] = {
  PIN_SENSOR_1, PIN_SENSOR_2, PIN_SENSOR_3, PIN_SENSOR_4,
  PIN_SENSOR_5, PIN_SENSOR_6, PIN_SENSOR_7, PIN_SENSOR_8
};

// Estado de cada cubiculo (LIBRE / RESERVADO / OCUPADO)
EstadoCubiculo estadoCubiculo[TOTAL_CUPOS] = {LIBRE, LIBRE, LIBRE, LIBRE, LIBRE, LIBRE, LIBRE, LIBRE};

// Contadores globales, se actualizan cada ciclo
int cuposOcupados = 0;      // cuenta OCUPADO + RESERVADO (para el LED rojo y la web)
int cuposOcupadosReal = 0;  // cuenta SOLO OCUPADO (carro fisico detectado) - para el ventilador
int cuposLibres = TOTAL_CUPOS;
int cuposReservados = 0;    // solo RESERVADO (informativo)

// Estado de los actuadores
bool ventiladorEncendido = false;
bool luzEncendida = false;

// ------------------------------------------------------------
// Conexion WiFi
// ------------------------------------------------------------
void conectarWiFi() {
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi conectado correctamente.");
    Serial.print("IP asignada: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("No se pudo conectar al WiFi. Revisa credenciales en config.h");
  }
}

// ------------------------------------------------------------
// Inicializa los pines de los sensores
// ------------------------------------------------------------
void inicializarSensores() {
  for (int i = 0; i < TOTAL_CUPOS; i++) {
    pinMode(pinesSensores[i], INPUT);
  }
}

// ------------------------------------------------------------
// Lee los 8 sensores y actualiza estadoCubiculo[] + contadores
// ------------------------------------------------------------
void leerSensores() {
  bool detectado[TOTAL_CUPOS];

  // Lectura fisica de los 8 sensores (fuera del mutex, no toca variables compartidas)
  for (int i = 0; i < TOTAL_CUPOS; i++) {
    int lectura = digitalRead(pinesSensores[i]);
    detectado[i] = (lectura == LOW); // LOW = el sensor ve un carro
  }

  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    int ocupadosTemp = 0;
    int ocupadosRealTemp = 0;
    int reservadosTemp = 0;

    for (int i = 0; i < TOTAL_CUPOS; i++) {
      EstadoCubiculo actual = estadoCubiculo[i];

      // Reglas de transicion:
      if (actual == RESERVADO && detectado[i]) {
        // Llego el carro que se habia reservado -> pasa a OCUPADO
        estadoCubiculo[i] = OCUPADO;
      } else if (actual == OCUPADO && !detectado[i]) {
        // El carro se fue -> vuelve a LIBRE (la reserva se consume)
        estadoCubiculo[i] = LIBRE;
      } else if (actual == LIBRE && detectado[i]) {
        // Alguien parqueo sin reservar -> OCUPADO directo
        estadoCubiculo[i] = OCUPADO;
      }
      // RESERVADO sin deteccion -> se queda en RESERVADO (nadie ha llegado aun)
      // OCUPADO con deteccion   -> se queda en OCUPADO (sigue el carro ahi)

      if (estadoCubiculo[i] == OCUPADO || estadoCubiculo[i] == RESERVADO) {
        ocupadosTemp++; // para el LED rojo y el conteo mostrado en la web
      }
      if (estadoCubiculo[i] == OCUPADO) {
        ocupadosRealTemp++; // SOLO carro fisico detectado - esto mueve el ventilador
      }
      if (estadoCubiculo[i] == RESERVADO) {
        reservadosTemp++;
      }
    }

    cuposOcupados = ocupadosTemp;
    cuposOcupadosReal = ocupadosRealTemp;
    cuposReservados = reservadosTemp;
    cuposLibres = TOTAL_CUPOS - ocupadosTemp;
    xSemaphoreGive(mutexEstado);
  }
}

// ------------------------------------------------------------
// Reserva un cubiculo (1 a 8). Solo funciona si esta LIBRE.
// Devuelve true si la reserva se hizo, false si no se pudo.
// ------------------------------------------------------------
bool reservarCubiculo(int numeroCubiculo) {
  int indice = numeroCubiculo - 1;
  if (indice < 0 || indice >= TOTAL_CUPOS) return false;

  bool exito = false;
  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    if (estadoCubiculo[indice] == LIBRE) {
      estadoCubiculo[indice] = RESERVADO;
      exito = true;
    }
    xSemaphoreGive(mutexEstado);
  }
  return exito;
}

// ------------------------------------------------------------
// Cancela la reserva de un cubiculo (1 a 8).
// Solo funciona si esta RESERVADO (si ya esta OCUPADO por un
// carro real, no se puede "liberar" desde aqui).
// ------------------------------------------------------------
bool liberarCubiculo(int numeroCubiculo) {
  int indice = numeroCubiculo - 1;
  if (indice < 0 || indice >= TOTAL_CUPOS) return false;

  bool exito = false;
  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    if (estadoCubiculo[indice] == RESERVADO) {
      estadoCubiculo[indice] = LIBRE;
      exito = true;
    }
    xSemaphoreGive(mutexEstado);
  }
  return exito;
}

// ------------------------------------------------------------
// Arma el byte de LEDs para un grupo de 4 cubiculos
// (bitOffset = indice del primer cubiculo del grupo: 0 o 4)
// ------------------------------------------------------------
byte armarByteLeds(int bitOffset) {
  byte valor = 0;

  for (int i = 0; i < 4; i++) {
    int cubiculo = bitOffset + i;
    // RESERVADO y OCUPADO se ven igual: LED rojo
    bool rojo = (estadoCubiculo[cubiculo] == OCUPADO || estadoCubiculo[cubiculo] == RESERVADO);

    // Cada cubiculo usa 2 bits consecutivos: [rojo][verde]
    int bitRojo  = i * 2;
    int bitVerde = i * 2 + 1;

    if (rojo) {
      valor |= (1 << bitRojo);   // enciende rojo
    } else {
      valor |= (1 << bitVerde);  // enciende verde
    }
  }

  return valor;
}

// ------------------------------------------------------------
// Envia los 16 bits a los 2 shift registers
// ------------------------------------------------------------
void actualizarLeds() {
  byte byteCubiculos1a4 = armarByteLeds(0); // cubiculos 1-4
  byte byteCubiculos5a8 = armarByteLeds(4); // cubiculos 5-8

  digitalWrite(PIN_595_LATCH, LOW);

  // Se envia primero el byte que debe terminar en el 2do chip
  // (el mas lejano del ESP32 en la cadena): cubiculos 5-8
  shiftOut(PIN_595_DATA, PIN_595_CLOCK, MSBFIRST, byteCubiculos5a8);

  // Luego el byte que queda en el 1er chip: cubiculos 1-4
  shiftOut(PIN_595_DATA, PIN_595_CLOCK, MSBFIRST, byteCubiculos1a4);

  digitalWrite(PIN_595_LATCH, HIGH);
}

// ------------------------------------------------------------
// Imprime el estado por Serial
// ------------------------------------------------------------
void imprimirEstado() {
  Serial.print("Cubiculos: ");
  for (int i = 0; i < TOTAL_CUPOS; i++) {
    Serial.print(i + 1);
    Serial.print("=");
    Serial.print(nombreEstado(estadoCubiculo[i]));
    if (i < TOTAL_CUPOS - 1) Serial.print(" | ");
  }
  Serial.println();

  Serial.print("Libres: ");
  Serial.print(cuposLibres);
  Serial.print("   Ocupados+Reservados: ");
  Serial.print(cuposOcupados);
  Serial.print("   (de ellos Reservados: ");
  Serial.print(cuposReservados);
  Serial.println(")");

  Serial.print("Carros reales (para ventilador): ");
  Serial.print(cuposOcupadosReal);
  Serial.print(" / umbral ");
  Serial.println(UMBRAL_VENTILADOR);

  Serial.print("Ventilador: ");
  Serial.println(ventiladorEncendido ? "ENCENDIDO" : "apagado");

  Serial.print("Luz inteligente: ");
  Serial.println(luzEncendida ? "ENCENDIDA" : "apagada");

  Serial.println("(Escribe 'luz on', 'luz off', 'reservar 3', 'liberar 3', 'reservar todos' o 'liberar todos')");
  Serial.println("--------------------------------------------------");
}

// ------------------------------------------------------------
// Controla el ventilador segun la cantidad de carros REALES
// (cubiculos en estado OCUPADO). Los RESERVADOS no cuentan.
// ------------------------------------------------------------
void controlarVentilador() {
  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    bool debeEncender = (cuposOcupadosReal >= UMBRAL_VENTILADOR);

    if (debeEncender != ventiladorEncendido) {
      ventiladorEncendido = debeEncender;
      digitalWrite(PIN_VENTILADOR, ventiladorEncendido ? HIGH : LOW);
    }
    xSemaphoreGive(mutexEstado);
  }
}

// ------------------------------------------------------------
// Enciende o apaga la luz inteligente
// ------------------------------------------------------------
void encenderLuz() {
  luzEncendida = true;
  digitalWrite(PIN_LUZ, HIGH);
  Serial.println(">> Luz inteligente ENCENDIDA");
}

void apagarLuz() {
  luzEncendida = false;
  digitalWrite(PIN_LUZ, LOW);
  Serial.println(">> Luz inteligente APAGADA");
}

// ------------------------------------------------------------
// Revisa si llego un comando por el monitor serial.
// ------------------------------------------------------------
void revisarComandosSerial() {
  if (Serial.available() > 0) {
    String comando = Serial.readStringUntil('\n');
    comando.trim();
    comando.toLowerCase();

    if (comando == "luz on") {
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
        encenderLuz();
        xSemaphoreGive(mutexEstado);
      }
    } else if (comando == "luz off") {
      if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
        apagarLuz();
        xSemaphoreGive(mutexEstado);
      }
    } else if (comando == "reservar todos") {
      int contador = 0;
      for (int i = 1; i <= TOTAL_CUPOS; i++) {
        if (reservarCubiculo(i)) contador++;
      }
      Serial.println(">> " + String(contador) + " cubiculo(s) reservado(s)");
    } else if (comando == "liberar todos") {
      int contador = 0;
      for (int i = 1; i <= TOTAL_CUPOS; i++) {
        if (liberarCubiculo(i)) contador++;
      }
      Serial.println(">> " + String(contador) + " cubiculo(s) liberado(s)");
    } else if (comando.startsWith("reservar ")) {
      int numero = comando.substring(9).toInt();
      bool ok = reservarCubiculo(numero);
      Serial.println(ok
        ? (">> Cubiculo " + String(numero) + " RESERVADO")
        : (">> No se pudo reservar el cubiculo " + String(numero) + " (no esta libre)"));
    } else if (comando.startsWith("liberar ")) {
      int numero = comando.substring(8).toInt();
      bool ok = liberarCubiculo(numero);
      Serial.println(ok
        ? (">> Reserva del cubiculo " + String(numero) + " cancelada")
        : (">> No se pudo liberar el cubiculo " + String(numero) + " (no estaba reservado)"));
    }
  }
}

// ------------------------------------------------------------
// Handlers del servidor HTTP
// ------------------------------------------------------------

// Headers CORS: permiten que la pagina web (en localhost) le
// hable al ESP32 (en otra IP) sin que el navegador lo bloquee.
void agregarHeadersCORS() {
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  servidor.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// GET /estado -> JSON con el estado actual del parqueadero,
// incluyendo el detalle de cada uno de los 8 cubiculos
void handleEstado() {
  agregarHeadersCORS();

  int libres, ocupados, reservados;
  bool venti, luz;
  EstadoCubiculo copiaEstados[TOTAL_CUPOS];

  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    libres = cuposLibres;
    ocupados = cuposOcupados;
    reservados = cuposReservados;
    venti = ventiladorEncendido;
    luz = luzEncendida;
    for (int i = 0; i < TOTAL_CUPOS; i++) {
      copiaEstados[i] = estadoCubiculo[i];
    }
    xSemaphoreGive(mutexEstado);
  }

  String json = "{";
  json += "\"libres\":" + String(libres) + ",";
  json += "\"ocupados\":" + String(ocupados) + ",";
  json += "\"reservados\":" + String(reservados) + ",";
  json += "\"total\":" + String(TOTAL_CUPOS) + ",";
  json += "\"ventilador\":" + String(venti ? "true" : "false") + ",";
  json += "\"luz\":" + String(luz ? "true" : "false") + ",";

  json += "\"cubiculos\":[";
  for (int i = 0; i < TOTAL_CUPOS; i++) {
    json += "\"" + String(nombreEstado(copiaEstados[i])) + "\"";
    if (i < TOTAL_CUPOS - 1) json += ",";
  }
  json += "]";

  json += "}";

  servidor.send(200, "application/json", json);
}

// POST /luz/on -> enciende la luz inteligente
void handleLuzOn() {
  agregarHeadersCORS();

  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    encenderLuz();
    xSemaphoreGive(mutexEstado);
  }

  servidor.send(200, "application/json", "{\"ok\":true,\"luz\":true}");
}

// POST /luz/off -> apaga la luz inteligente
void handleLuzOff() {
  agregarHeadersCORS();

  if (xSemaphoreTake(mutexEstado, portMAX_DELAY) == pdTRUE) {
    apagarLuz();
    xSemaphoreGive(mutexEstado);
  }

  servidor.send(200, "application/json", "{\"ok\":true,\"luz\":false}");
}

// POST /reservar/<1-8> -> reserva ese cubiculo si esta libre
void handleReservar() {
  agregarHeadersCORS();

  String uri = servidor.uri(); // ej: "/reservar/3"
  int numero = uri.substring(uri.lastIndexOf('/') + 1).toInt();

  bool exito = reservarCubiculo(numero);

  if (exito) {
    Serial.println(">> Cubiculo " + String(numero) + " RESERVADO (via web/voz)");
    servidor.send(200, "application/json", "{\"ok\":true}");
  } else {
    servidor.send(409, "application/json", "{\"ok\":false,\"error\":\"cubiculo no disponible\"}");
  }
}

// POST /liberar/<1-8> -> cancela la reserva de ese cubiculo
void handleLiberar() {
  agregarHeadersCORS();

  String uri = servidor.uri(); // ej: "/liberar/3"
  int numero = uri.substring(uri.lastIndexOf('/') + 1).toInt();

  bool exito = liberarCubiculo(numero);

  if (exito) {
    Serial.println(">> Reserva del cubiculo " + String(numero) + " cancelada (via web/voz)");
    servidor.send(200, "application/json", "{\"ok\":true}");
  } else {
    servidor.send(409, "application/json", "{\"ok\":false,\"error\":\"cubiculo no estaba reservado\"}");
  }
}

// POST /reservar-todos -> reserva todos los cubiculos que esten
// LIBRE en este momento (los OCUPADOS y ya RESERVADOS se ignoran,
// no dan error)
void handleReservarTodos() {
  agregarHeadersCORS();

  int reservadosAhora = 0;
  for (int i = 1; i <= TOTAL_CUPOS; i++) {
    if (reservarCubiculo(i)) {
      reservadosAhora++;
    }
  }

  Serial.println(">> Reservados " + String(reservadosAhora) + " cubiculo(s) (comando: todos)");

  String json = "{\"ok\":true,\"reservados\":" + String(reservadosAhora) + "}";
  servidor.send(200, "application/json", json);
}

// POST /liberar-todos -> cancela la reserva de todos los
// cubiculos que esten en RESERVADO (los OCUPADOS con carro
// real no se tocan, no se puede "liberar" un carro fisico)
void handleLiberarTodos() {
  agregarHeadersCORS();

  int liberadosAhora = 0;
  for (int i = 1; i <= TOTAL_CUPOS; i++) {
    if (liberarCubiculo(i)) {
      liberadosAhora++;
    }
  }

  Serial.println(">> Liberados " + String(liberadosAhora) + " cubiculo(s) (comando: todos)");

  String json = "{\"ok\":true,\"liberados\":" + String(liberadosAhora) + "}";
  servidor.send(200, "application/json", json);
}

// Responde a las peticiones OPTIONS que manda el navegador
// automaticamente antes de un POST (parte del protocolo CORS)
void handleOptions() {
  agregarHeadersCORS();
  servidor.send(204);
}

// Configura todas las rutas del servidor
void configurarServidor() {
  servidor.on("/estado", HTTP_GET, handleEstado);
  servidor.on("/luz/on", HTTP_POST, handleLuzOn);
  servidor.on("/luz/off", HTTP_POST, handleLuzOff);

  servidor.on("/estado", HTTP_OPTIONS, handleOptions);
  servidor.on("/luz/on", HTTP_OPTIONS, handleOptions);
  servidor.on("/luz/off", HTTP_OPTIONS, handleOptions);

  // Rutas con numero variable: /reservar/1 ... /reservar/8
  //                            /liberar/1  ... /liberar/8
  for (int i = 1; i <= TOTAL_CUPOS; i++) {
    servidor.on("/reservar/" + String(i), HTTP_POST, handleReservar);
    servidor.on("/liberar/" + String(i), HTTP_POST, handleLiberar);
    servidor.on("/reservar/" + String(i), HTTP_OPTIONS, handleOptions);
    servidor.on("/liberar/" + String(i), HTTP_OPTIONS, handleOptions);
  }

  // Reservar/liberar TODOS los cubiculos de una vez
  servidor.on("/reservar-todos", HTTP_POST, handleReservarTodos);
  servidor.on("/liberar-todos", HTTP_POST, handleLiberarTodos);
  servidor.on("/reservar-todos", HTTP_OPTIONS, handleOptions);
  servidor.on("/liberar-todos", HTTP_OPTIONS, handleOptions);

  servidor.begin();
  Serial.println("Servidor HTTP iniciado en el puerto 80");
}

// ------------------------------------------------------------
// Tarea de HARDWARE - Core 0
// ------------------------------------------------------------
void taskHardware(void *parametro) {
  // Configuracion de pines de los 595 (se hace una sola vez)
  pinMode(PIN_595_DATA, OUTPUT);
  pinMode(PIN_595_CLOCK, OUTPUT);
  pinMode(PIN_595_LATCH, OUTPUT);

  // Configuracion de actuadores
  pinMode(PIN_VENTILADOR, OUTPUT);
  pinMode(PIN_LUZ, OUTPUT);
  digitalWrite(PIN_VENTILADOR, LOW);
  digitalWrite(PIN_LUZ, LOW);

  inicializarSensores();

  for (;;) {
    leerSensores();
    actualizarLeds();
    controlarVentilador();
    revisarComandosSerial();

    imprimirEstado();

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ------------------------------------------------------------
// Tarea de RED - Core 1
// ------------------------------------------------------------
void taskRed(void *parametro) {
  configurarServidor();

  for (;;) {
    servidor.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS); // chequeo frecuente, el servidor debe responder rapido
  }
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println("  Parqueadero Inteligente - Firmware v1.2");
  Serial.println("  (VALET: reservas + HTTP)");
  Serial.println("============================================");

  mutexEstado = xSemaphoreCreateMutex();

  conectarWiFi();

  xTaskCreatePinnedToCore(taskHardware, "TaskHardware", 4096, NULL, 1, &taskHardwareHandle, 0);
  xTaskCreatePinnedToCore(taskRed, "TaskRed", 4096, NULL, 1, &taskRedHandle, 1);

  Serial.println("Tareas de hardware y red creadas correctamente.");
  Serial.println();
  Serial.println(">>> Usa esta IP en la pagina web (webapp/js/app.js): ");
  Serial.print(">>> http://");
  Serial.println(WiFi.localIP());
  Serial.println();
}

void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
