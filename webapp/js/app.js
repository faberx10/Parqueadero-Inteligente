/* ==========================================================
   VALET — Parqueadero Inteligente — app.js
   - Consulta el estado del ESP32 cada 2s (GET /estado)
   - Reconocimiento de voz con Web Speech API
   - Comandos: encender/apagar luces, reservar/liberar uno,
     varios, o todos los cubiculos
   ========================================================== */

// ============================================================
// CONFIGURACION — cambia esta IP por la que muestra el ESP32
// en el monitor serial al conectarse al WiFi.
// ============================================================
const ESP32_IP = "10.191.26.220"; // <-- AJUSTAR AQUI
const ESP32_URL = `http://${ESP32_IP}`;
const INTERVALO_ACTUALIZACION_MS = 2000;

// ============================================================
// Referencias al DOM
// ============================================================
const elConexion = document.getElementById("estadoConexion");
const elConexionTexto = document.getElementById("conexionTexto");
const elFooterIP = document.getElementById("footerIP");

const elValorLibres = document.getElementById("valorLibres");
const elValorOcupados = document.getElementById("valorOcupados");
const elEstadoVentilador = document.getElementById("estadoVentilador");
const elCardVentilador = document.getElementById("cardVentilador");
const elEstadoLuz = document.getElementById("estadoLuz");
const elCardLuz = document.getElementById("cardLuz");

const elMapaCubiculos = document.getElementById("mapaCubiculos");

const elBtnMicrofono = document.getElementById("btnMicrofono");
const elMicLabel = document.getElementById("micLabel");
const elTranscripcionTexto = document.getElementById("transcripcionTexto");
const elVozLog = document.getElementById("vozLog");

// ============================================================
// Estado local
// ============================================================
let ultimoEstado = null;

// ============================================================
// Construir el mapa de 8 cubiculos (una sola vez)
// ============================================================
function construirMapaCubiculos() {
  elMapaCubiculos.innerHTML = "";
  for (let i = 1; i <= 8; i++) {
    const div = document.createElement("div");
    div.className = "cubiculo libre";
    div.id = `cubiculo-${i}`;
    div.tabIndex = 0;
    div.innerHTML = `
      <span class="cubiculo-numero">CUBÍCULO ${i}</span>
      <span class="cubiculo-luz"></span>
      <span class="cubiculo-estado">LIBRE</span>
    `;
    div.addEventListener("click", () => manejarClickCubiculo(i));
    div.addEventListener("keydown", (evento) => {
      if (evento.key === "Enter" || evento.key === " ") {
        evento.preventDefault();
        manejarClickCubiculo(i);
      }
    });
    elMapaCubiculos.appendChild(div);
  }
}

// ============================================================
// Click (o Enter) en un cubiculo: reserva si esta libre,
// cancela la reserva si esta reservado. Si esta OCUPADO por
// un carro real, no hace nada (no se puede liberar un carro
// fisico desde la web).
// ============================================================
function manejarClickCubiculo(numero) {
  const estadoActual = ultimoEstado?.cubiculos?.[numero - 1];

  if (estadoActual === "LIBRE") {
    reservarCubiculo(numero);
  } else if (estadoActual === "RESERVADO") {
    liberarCubiculo(numero);
  }
  // Si esta OCUPADO, no se hace nada: hay un carro real ahi.
}

// ============================================================
// Actualiza la interfaz con el estado recibido del ESP32.
// Usa el detalle EXACTO por cubiculo que manda el firmware
// (arreglo "cubiculos": ["LIBRE","RESERVADO","OCUPADO",...]).
// ============================================================
function actualizarUI(estado) {
  elValorLibres.textContent = estado.libres;
  elValorOcupados.textContent = estado.ocupados;

  if (estado.reservados > 0) {
    elValorOcupados.parentElement.querySelector("#ocupadosNota").textContent =
      `de 8 (${estado.reservados} reservado${estado.reservados > 1 ? "s" : ""})`;
  } else {
    elValorOcupados.parentElement.querySelector("#ocupadosNota").textContent = "de 8";
  }

  // Ventilador
  elEstadoVentilador.textContent = estado.ventilador ? "ENCENDIDO" : "APAGADO";
  elCardVentilador.classList.toggle("activo", estado.ventilador);

  // Luz inteligente
  elEstadoLuz.textContent = estado.luz ? "ENCENDIDA" : "APAGADA";
  elCardLuz.classList.toggle("activo", estado.luz);

  // Mapa de cubiculos: usa el detalle REAL que manda el ESP32
  // (LIBRE / RESERVADO / OCUPADO por cada uno de los 8)
  estado.cubiculos.forEach((estadoTexto, indice) => {
    const numero = indice + 1;
    const cub = document.getElementById(`cubiculo-${numero}`);
    if (!cub) return;

    cub.classList.remove("libre", "reservado", "ocupado");
    cub.classList.add(estadoTexto.toLowerCase());
    cub.querySelector(".cubiculo-estado").textContent = estadoTexto;
  });

  ultimoEstado = estado;
}

// ============================================================
// Marca visualmente el estado de la conexion con el ESP32
// ============================================================
function marcarConexion(online) {
  elConexion.classList.toggle("online", online);
  elConexion.classList.toggle("offline", !online);
  elConexionTexto.textContent = online ? "ESP32 conectado" : "Sin conexión";
  elFooterIP.textContent = online
    ? `Conectado a ${ESP32_URL}`
    : `Intentando conectar a ${ESP32_URL}…`;
}

// ============================================================
// Consulta GET /estado en el ESP32
// ============================================================
async function consultarEstado() {
  try {
    const respuesta = await fetch(`${ESP32_URL}/estado`, { method: "GET" });
    if (!respuesta.ok) throw new Error("Respuesta no OK");

    const datos = await respuesta.json();
    marcarConexion(true);
    actualizarUI(datos);
  } catch (error) {
    marcarConexion(false);
  }
}

// ============================================================
// Envia comando de luz al ESP32
// ============================================================
async function enviarComandoLuz(encender) {
  const ruta = encender ? "/luz/on" : "/luz/off";
  try {
    const respuesta = await fetch(`${ESP32_URL}${ruta}`, { method: "POST" });
    if (!respuesta.ok) throw new Error("Respuesta no OK");

    agregarLogVoz(
      encender ? "Luces encendidas" : "Luces apagadas",
      true
    );
    // Refresca el estado inmediatamente para que la UI reaccione rapido
    consultarEstado();
  } catch (error) {
    agregarLogVoz("No se pudo comunicar con el ESP32", false);
  }
}

// ============================================================
// Reserva un cubiculo especifico en el ESP32
// ============================================================
async function reservarCubiculo(numero) {
  try {
    const respuesta = await fetch(`${ESP32_URL}/reservar/${numero}`, { method: "POST" });
    const datos = await respuesta.json();

    if (respuesta.ok && datos.ok) {
      agregarLogVoz(`Cubículo ${numero} reservado`, true);
    } else {
      agregarLogVoz(`Cubículo ${numero} no está disponible`, false);
    }
    consultarEstado();
  } catch (error) {
    agregarLogVoz("No se pudo comunicar con el ESP32", false);
  }
}

// ============================================================
// Cancela la reserva de un cubiculo especifico en el ESP32
// ============================================================
async function liberarCubiculo(numero) {
  try {
    const respuesta = await fetch(`${ESP32_URL}/liberar/${numero}`, { method: "POST" });
    const datos = await respuesta.json();

    if (respuesta.ok && datos.ok) {
      agregarLogVoz(`Reserva del cubículo ${numero} cancelada`, true);
    } else {
      agregarLogVoz(`El cubículo ${numero} no estaba reservado`, false);
    }
    consultarEstado();
  } catch (error) {
    agregarLogVoz("No se pudo comunicar con el ESP32", false);
  }
}

// ============================================================
// Agrega una entrada al historial de comandos de voz
// ============================================================
function agregarLogVoz(mensaje, exito) {
  const vacio = elVozLog.querySelector(".log-vacio");
  if (vacio) vacio.remove();

  const hora = new Date().toLocaleTimeString("es-CO", {
    hour: "2-digit", minute: "2-digit", second: "2-digit"
  });

  const item = document.createElement("div");
  item.className = "log-item";
  item.innerHTML = `
    <span class="log-hora">${hora}</span>
    <span class="${exito ? "log-ok" : "log-error"}">${mensaje}</span>
  `;
  elVozLog.prepend(item);
}

// ============================================================
// RECONOCIMIENTO DE VOZ (Web Speech API)
// ============================================================
const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;
let reconocimiento = null;
let escuchando = false;

function inicializarReconocimientoVoz() {
  if (!SpeechRecognition) {
    elMicLabel.textContent = "Voz no soportada en este navegador";
    elBtnMicrofono.disabled = true;
    agregarLogVoz(
      "Este navegador no soporta reconocimiento de voz. Usa Chrome.",
      false
    );
    return;
  }

  reconocimiento = new SpeechRecognition();
  reconocimiento.lang = "es-CO";
  reconocimiento.continuous = false;
  reconocimiento.interimResults = false;
  reconocimiento.maxAlternatives = 1;

  reconocimiento.onstart = () => {
    escuchando = true;
    elBtnMicrofono.classList.add("escuchando");
    elMicLabel.textContent = "Escuchando…";
  };

  reconocimiento.onend = () => {
    escuchando = false;
    elBtnMicrofono.classList.remove("escuchando");
    elMicLabel.textContent = "Presiona para hablar";
  };

  reconocimiento.onerror = (evento) => {
    escuchando = false;
    elBtnMicrofono.classList.remove("escuchando");
    elMicLabel.textContent = "Presiona para hablar";

    if (evento.error === "not-allowed") {
      agregarLogVoz("Permiso de micrófono denegado", false);
    } else if (evento.error === "no-speech") {
      agregarLogVoz("No se detectó voz, intenta de nuevo", false);
    } else {
      agregarLogVoz(`Error de reconocimiento: ${evento.error}`, false);
    }
  };

  reconocimiento.onresult = (evento) => {
    const texto = evento.results[0][0].transcript.trim();
    elTranscripcionTexto.textContent = texto;
    procesarComandoVoz(texto);
  };
}

// ============================================================
// Convierte TODOS los numeros mencionados en el texto (en
// digitos o en palabras) a una lista de enteros unicos, entre
// 1 y 8. Ej: "cubiculos 2, 4 y 6" -> [2, 4, 6]
// ============================================================
function extraerNumerosCubiculo(texto) {
  const palabras = {
    "uno": 1, "una": 1, "dos": 2, "tres": 3, "cuatro": 4,
    "cinco": 5, "seis": 6, "siete": 7, "ocho": 8
  };

  const encontrados = new Set();

  // Digitos sueltos del 1 al 8 (ej: "2, 4 y 6")
  const coincidenciasDigito = texto.match(/\b[1-8]\b/g);
  if (coincidenciasDigito) {
    coincidenciasDigito.forEach(d => encontrados.add(parseInt(d, 10)));
  }

  // Numeros dichos en palabra (ej: "dos, cuatro y seis")
  for (const palabra in palabras) {
    // \b evita que "una" haga match dentro de otra palabra
    const regex = new RegExp(`\\b${palabra}\\b`, "g");
    if (regex.test(texto)) {
      encontrados.add(palabras[palabra]);
    }
  }

  return Array.from(encontrados).sort((a, b) => a - b);
}

// ============================================================
// Reserva o libera una LISTA de cubiculos, uno por uno, y deja
// un solo resumen en el log en vez de una linea por cada uno.
// ============================================================
async function reservarVarios(numeros) {
  let exitosos = 0;
  for (const numero of numeros) {
    try {
      const respuesta = await fetch(`${ESP32_URL}/reservar/${numero}`, { method: "POST" });
      const datos = await respuesta.json();
      if (respuesta.ok && datos.ok) exitosos++;
    } catch (error) { /* se cuenta como fallo, sigue con el resto */ }
  }
  agregarLogVoz(`Reservados ${exitosos} de ${numeros.length} cubículo(s)`, exitosos > 0);
  consultarEstado();
}

async function liberarVarios(numeros) {
  let exitosos = 0;
  for (const numero of numeros) {
    try {
      const respuesta = await fetch(`${ESP32_URL}/liberar/${numero}`, { method: "POST" });
      const datos = await respuesta.json();
      if (respuesta.ok && datos.ok) exitosos++;
    } catch (error) { /* se cuenta como fallo, sigue con el resto */ }
  }
  agregarLogVoz(`Liberados ${exitosos} de ${numeros.length} cubículo(s)`, exitosos > 0);
  consultarEstado();
}

// ============================================================
// Reserva o libera TODOS los cubiculos de una vez
// ============================================================
async function reservarTodos() {
  try {
    const respuesta = await fetch(`${ESP32_URL}/reservar-todos`, { method: "POST" });
    const datos = await respuesta.json();
    agregarLogVoz(`${datos.reservados} cubículo(s) reservados (todos)`, true);
    consultarEstado();
  } catch (error) {
    agregarLogVoz("No se pudo comunicar con el ESP32", false);
  }
}

async function liberarTodos() {
  try {
    const respuesta = await fetch(`${ESP32_URL}/liberar-todos`, { method: "POST" });
    const datos = await respuesta.json();
    agregarLogVoz(`${datos.liberados} cubículo(s) liberados (todos)`, true);
    consultarEstado();
  } catch (error) {
    agregarLogVoz("No se pudo comunicar con el ESP32", false);
  }
}

// ============================================================
// Interpreta el texto reconocido y ejecuta la accion.
// Palabra de activacion: "VALET" (se acepta con o sin ella,
// para que un reconocimiento imperfecto no bloquee el comando).
// ============================================================
function procesarComandoVoz(textoOriginal) {
  const texto = textoOriginal.toLowerCase();

  const mencionaLuces = texto.includes("luz") || texto.includes("luces");
  const mencionaEnciende = texto.includes("enciende") || texto.includes("prende") || texto.includes("encender");
  const mencionaApaga = texto.includes("apaga") || texto.includes("apagar");

  const mencionaCubiculo = texto.includes("cubiculo") || texto.includes("cubículo") || texto.includes("espacio");
  const mencionaReserva = texto.includes("reserva") || texto.includes("aparta") || texto.includes("separa");
  const mencionaLibera = texto.includes("libera") || texto.includes("cancela") || texto.includes("desreserva");
  const mencionaTodos = texto.includes("todos") || texto.includes("todo");

  if (mencionaLuces && mencionaEnciende) {
    enviarComandoLuz(true);
    return;
  }

  if (mencionaLuces && mencionaApaga) {
    enviarComandoLuz(false);
    return;
  }

  // "Valet, reserva todos" / "Valet, libera todos"
  // (no hace falta que mencione la palabra "cubiculo" aqui)
  if (mencionaTodos && mencionaReserva) {
    reservarTodos();
    return;
  }

  if (mencionaTodos && mencionaLibera) {
    liberarTodos();
    return;
  }

  if (mencionaCubiculo && mencionaReserva) {
    const numeros = extraerNumerosCubiculo(texto);
    if (numeros.length === 1) {
      reservarCubiculo(numeros[0]);
    } else if (numeros.length > 1) {
      reservarVarios(numeros);
    } else {
      agregarLogVoz("No entendí qué cubículo reservar", false);
    }
    return;
  }

  if (mencionaCubiculo && mencionaLibera) {
    const numeros = extraerNumerosCubiculo(texto);
    if (numeros.length === 1) {
      liberarCubiculo(numeros[0]);
    } else if (numeros.length > 1) {
      liberarVarios(numeros);
    } else {
      agregarLogVoz("No entendí qué cubículo liberar", false);
    }
    return;
  }

  agregarLogVoz(`Comando no reconocido: "${textoOriginal}"`, false);
}

// ============================================================
// Boton de microfono
// ============================================================
elBtnMicrofono.addEventListener("click", () => {
  if (!reconocimiento) return;
  if (escuchando) {
    reconocimiento.stop();
  } else {
    reconocimiento.start();
  }
});

// ============================================================
// Inicio
// ============================================================
construirMapaCubiculos();
inicializarReconocimientoVoz();
consultarEstado();
setInterval(consultarEstado, INTERVALO_ACTUALIZACION_MS);
