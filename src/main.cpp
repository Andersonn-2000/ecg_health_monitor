#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include <Adafruit_ADS1X15.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

#include <LittleFS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

// pinout
#define PIN_SDA       21
#define PIN_SCL       22
#define PIN_LO_PLUS   32
#define PIN_LO_MINUS  33
#define PIN_ONEWIRE    4 // DS18B20

#define ADS_ENDERECO  0x48

// MAX30102 - taxa de amostragem (criterio de Nyquist)
#define PPG_SAMPLE_RATE_HW 400
#define PPG_SAMPLE_AVG 4
#define PPG_FS_HZ (PPG_SAMPLE_RATE_HW / PPG_SAMPLE_AVG)   // 100 Hz
#define PPG_PERIODO_MS (1000 / PPG_FS_HZ)

#define PPG_ALG_FS_HZ 25
#define PPG_DECIM (PPG_FS_HZ / PPG_ALG_FS_HZ)
#define PPG_BUFFER_SIZE 100
#define PPG_PASSO 25
#define PPG_DC_ALPHA 0.03f
#define LIMIAR_DEDO_IR 50000UL

// ADS8232
// Adotado 250 Hz
#define ECG_FS_HZ 250
static_assert(1000 % ECG_FS_HZ == 0, "ECG_FS_HZ deve dividir 1000 (periodo em ms inteiro)");
#define ECG_PERIODO_MS (1000 / ECG_FS_HZ)           // 4 ms
// Constantes do detector derivadas de fs 
#define ECG_BASELINE_ALPHA (2.5f / ECG_FS_HZ)
#define ECG_ENV_DECAY (1.0f - 1.25f / ECG_FS_HZ)
#define ECG_LIMIAR_MIN_MV 10.0f
#define ECG_ENVELOPE_MIN_MV 15.0f
#define ECG_REFRACTARIO_MS 250
#define ECG_RR_MIN_MS 250
#define ECG_RR_MAX_MS 2000

// DS18B20
#define TEMP_RESOLUTION_BITS 12
#define TEMP_PERIOD_MS 1000
#define TEMP_ERRORS_MS 3
#define TEMP_ALPHA 0.4f
#define TEMP_OFFSET_C 0.0f

// estruturas

struct SampleECG {
    uint32_t time_ms;
    int16_t raw;
    float mV;
    bool leadoff;
    bool validLecture;
};

struct VitalSigns {
    float bpmECG;
    bool leadoff;
    float ecg_mV;
    int32_t spo2;
    bool spo2Valid;
    int32_t bpmPPG;
    bool bpmPPGValid;
    bool fingerPresent;
    float tempC;
    bool tempValid;
};

// Wifi e WebSocket

const char* hostname = "monitorvital";
const int port = 80;
const char* ssid = "MonitorVital-ESP32";
const char* password = "viuusw24";

// criação do objeto do server e do websocket (url "/wb")

AsyncWebServer server(port);
AsyncWebSocket ws("/ws");
bool isServerRunning = false;

// objetos e recursos compartilhados

Adafruit_ADS1015 ads;
MAX30105 sensorPPG;

QueueHandle_t queueECG = nullptr;

#define STREAM_PERIOD_MS 100
struct PpgWave { int32_t ir; int32_t red; };
QueueHandle_t queueWaveECG = nullptr; 
QueueHandle_t queueWavePPG = nullptr;   

SemaphoreHandle_t mutexI2C = nullptr;
SemaphoreHandle_t mutexVital = nullptr;

VitalSigns vital = {};

// funções auxiliares

void updateEletrodeStates(bool solto) {
    if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
        vital.leadoff = solto;

        if (solto) {
            vital.bpmECG = 0.0f;
        }
        xSemaphoreGive(mutexVital);
    }
}

void invalidatePPG(bool fingerPresent) {
    if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
        vital.fingerPresent = fingerPresent;
        vital.spo2Valid = false;
        vital.bpmPPGValid = false;

        if (!fingerPresent) {
            vital.spo2 = 0;
            vital.bpmPPG = 0;
        }
        xSemaphoreGive(mutexVital);
    }
}

void updateTemperature(float tempC, bool valid) {
    if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
        vital.tempC = valid ? tempC: 0.0f;
        vital.tempValid = valid;
        xSemaphoreGive(mutexVital);
    }
}
// função para formatação e envio dos dados vitais em JSON para os clientes WebSocket

void broadcastVitalSigns(VitalSigns *v) {
    if (ws.count() == 0) return;

    char jsonBuffer[380];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"leadoff\":%d,\"bpmECG\":%.1f,\"ecg_mV\":%.2f,\"fingerPresent\":%d,\"bpmPPGValid\":%d,\"bpmPPG\":%ld,"
             "\"spo2Valid\":%d,\"spo2\":%ld,\"tempValid\":%d,\"tempC\":%.2f}",
             v->leadoff, v->bpmECG, v->ecg_mV, v->fingerPresent, v->bpmPPGValid,
             (long)v->bpmPPG, v->spo2Valid, (long)v->spo2,
             v->tempValid, v->tempC);

    ws.textAll(jsonBuffer);
}

// envia em lote as amostras acumuladas (ECG @250 Hz, PPG @100 Hz)
void broadcastWaves() {
    static int16_t ecgA[100];
    static PpgWave ppgA[50];
    static char buf[2048];
    size_t ne = 0, np = 0;

    // sempre esvazia as filas, mesmo sem clientes, para nao enviar dados velhos depois
    while (ne < 100 && xQueueReceive(queueWaveECG, &ecgA[ne], 0) == pdTRUE) ne++;
    while (np < 50 && xQueueReceive(queueWavePPG, &ppgA[np], 0) == pdTRUE) np++;

    if (ws.count() == 0 || !ws.availableForWriteAll() || (ne == 0 && np == 0)) return;

    size_t n = snprintf(buf, sizeof(buf), "{\"w\":1,\"ecg\":[");
    for (size_t i = 0; i < ne && n < sizeof(buf) - 16; i++)
        n += snprintf(buf + n, sizeof(buf) - n, i ? ",%d" : "%d", ecgA[i]);
    n += snprintf(buf + n, sizeof(buf) - n, "],\"ir\":[");
    for (size_t i = 0; i < np && n < sizeof(buf) - 16; i++)
        n += snprintf(buf + n, sizeof(buf) - n, i ? ",%ld" : "%ld", (long)ppgA[i].ir);
    n += snprintf(buf + n, sizeof(buf) - n, "],\"red\":[");
    for (size_t i = 0; i < np && n < sizeof(buf) - 16; i++)
        n += snprintf(buf + n, sizeof(buf) - n, i ? ",%ld" : "%ld", (long)ppgA[i].red);
    snprintf(buf + n, sizeof(buf) - n, "]}");

    ws.textAll(buf);
}

// evento do WebSocket (trata conexões/desconexões)

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("Cliente WebSocket #%u conectado de %s\n", client->id(), client->remoteIP().toString().c_str());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("Cliente WebSocket #%u desconectado\n", client->id());
    }
}

// Tasks

// Task 1: Aquisição do ECG

void taskECG(void *pv) {
    TickType_t previousState = xTaskGetTickCount();

    bool prevStateLeadOff = true;

    for (;;) {
        SampleECG sample = {};
        sample.time_ms = millis();
        sample.leadoff = digitalRead(PIN_LO_PLUS) || digitalRead(PIN_LO_MINUS);

        // Se os eletrodos acabaram de desconectar, envia um
        // evento para a tarefa de processamento reiniciar o detector.
        if (sample.leadoff) {
            if (!prevStateLeadOff) {
                sample.validLecture = false;

                if (xQueueSend(queueECG, &sample, 0) == pdTRUE) {
                    prevStateLeadOff = true;
                }
                // Se a fila estiver cheia, tenta novamente
                // na proxima iteracao.
            }
            vTaskDelayUntil(&previousState, pdMS_TO_TICKS(ECG_PERIODO_MS));
            continue;
        }

        prevStateLeadOff = false;

        // acesso ao barramento I2C protegido por mutex.
        bool lectureOK = false;

        if (xSemaphoreTake(mutexI2C, pdMS_TO_TICKS(10)) == pdTRUE) {
            sample.raw = ads.readADC_SingleEnded(0);
            sample.mV = ads.computeVolts(sample.raw) * 1000.0f;

            xSemaphoreGive(mutexI2C);
            lectureOK = true;
        }
        if (lectureOK) {
            sample.validLecture = true;

            if (xQueueSend(queueECG, &sample, 0) != pdTRUE) {
                // amostra descartada: a tarefa continua sem bloquear.
            }
        }

        // se a leitura falhou, nao envia uma sample artificial com valor zero.
        vTaskDelayUntil(&previousState, pdMS_TO_TICKS(ECG_PERIODO_MS));
    }
}

// Task 2: processamento do ECG

void taskProcessECG(void *parametro) {
    SampleECG sample;

    float baseline = 0.0f;
    float envelope = 0.0f;

    uint32_t lastPeakMs = 0;

    uint32_t intervalRR[5] = {};
    uint8_t indexRR = 0;
    uint8_t quantRR = 0;

    bool eletrodoAnteriorSolto = true;

    auto rebootDetector = [&]() {
        baseline = 0.0f;
        envelope = 0.0f;
        lastPeakMs = 0;

        memset(intervalRR, 0, sizeof(intervalRR));

        indexRR = 0;
        quantRR = 0;
    };

    for (;;) {
        if (xQueueReceive(queueECG, &sample, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // evento de desconexao dos eletrodos
        if (sample.leadoff) {
            rebootDetector();
            eletrodoAnteriorSolto = true;

            updateEletrodeStates(true);
            continue;
        }

        // ignora qualquer sample invalida
        if (!sample.validLecture) {
            continue;
        }

        // ao reconectar os eletrodos, reinicia o detector para
        // nao calcular bpm com intervalos anteriores.
        if (eletrodoAnteriorSolto) {
            rebootDetector();
            eletrodoAnteriorSolto = false;

            updateEletrodeStates(false);
        }

        // remove aproximadamente o nivel DC do sinal.
        if (baseline == 0.0f) baseline = sample.mV;   // evita transiente inicial
        baseline += ECG_BASELINE_ALPHA * (sample.mV - baseline);
        float sinal = sample.mV - baseline;

        if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(2)) == pdTRUE){
            vital.ecg_mV = sinal;
            xSemaphoreGive(mutexVital);
        }
        // forma de onda completa (250 Hz) para o grafico
        int16_t wave = (int16_t)lroundf(fminf(fmaxf(sinal, -32000.0f), 32000.0f));
        xQueueSend(queueWaveECG, &wave, 0);
        // envelope baseado na magnitude do sinal.
        float magnitude = fabsf(sinal);
        if (magnitude > envelope) {
            envelope = magnitude;
        } else {
            envelope *= ECG_ENV_DECAY;
        }
        float threshold = fmaxf(ECG_LIMIAR_MIN_MV, envelope * 0.55f);

        bool candidatePeak = sinal > threshold && envelope > ECG_ENVELOPE_MIN_MV;

        if (candidatePeak) {
            uint32_t now = sample.time_ms;

            // evita detectar varios picos durante o mesmo complexo QRS
            bool foraDoRefratario = lastPeakMs == 0 || (uint32_t)(now - lastPeakMs) >= ECG_REFRACTARIO_MS;

            if (foraDoRefratario) {
                if (lastPeakMs != 0) {
                    uint32_t rr = now - lastPeakMs;
                    if (rr >= ECG_RR_MIN_MS && rr <= ECG_RR_MAX_MS) {
                        intervalRR[indexRR] = rr;

                        indexRR = (indexRR + 1) % 5;
                        if (quantRR < 5) {
                            quantRR++;
                        }
                        // espera pelo menos tres intervalos RR antes de calcular uma estimativa.
                        if (quantRR >= 3) {
                            uint64_t sumRR = 0;

                            for (uint8_t i = 0; i < quantRR; i++) {
                                sumRR += intervalRR[i];
                            }

                            float mediaRR = (float)sumRR / quantRR;
                            float bpm = 60000.0f / mediaRR;

                            if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
                                vital.bpmECG = bpm;
                                vital.leadoff = false;
                                xSemaphoreGive(mutexVital);
                            }
                        }
                    }
                }

                lastPeakMs = now;
            }
        }

        // se nenhum pico aparece por tempo prolongado, evita manter indefinidamente um BPM antigo
        if (lastPeakMs != 0 &&
            (uint32_t)(sample.time_ms - lastPeakMs) > 3000) {

            rebootDetector();

            if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
                vital.bpmECG = 0.0f;
                xSemaphoreGive(mutexVital);
            }
        }
    }
}

// Task 3: aquisição e processamento do PPG

void taskPPG(void *parametro) {
    static uint32_t bufferIR[PPG_BUFFER_SIZE] = {};
    static uint32_t bufferRED[PPG_BUFFER_SIZE] = {};

    uint16_t full = 0;
    uint16_t news = 0;

    bool bufferAnalised = false;
    bool fingerPresent = false;

    uint32_t lastIR = 0;
    uint32_t lastSamplesMs = 0;

    // decimacao 4:1 (100 Hz -> 25 Hz) para o algoritmo de SpO2/BPM
    uint32_t accIR = 0, accRED = 0;
    uint8_t accN = 0;

    // remocao de DC para o grafico
    float dcIR = 0.0f, dcRED = 0.0f;
    bool dcInit = false;

    TickType_t previousState = xTaskGetTickCount();

    for (;;) {
        bool receivedSamples = false;

        // leitura do FIFO do max30102
        if (xSemaphoreTake(mutexI2C, pdMS_TO_TICKS(10)) == pdTRUE) {
            sensorPPG.check();
            while (sensorPPG.available()) {
                uint32_t red = sensorPPG.getFIFORed();
                uint32_t ir = sensorPPG.getFIFOIR();

                sensorPPG.nextSample();

                lastIR = ir;
                lastSamplesMs = millis();
                receivedSamples = true;

                // forma de onda (100 Hz) para o grafico, so com dedo presente
                if (ir > LIMIAR_DEDO_IR) {
                    if (!dcInit) { dcIR = ir; dcRED = red; dcInit = true; }
                    dcIR  += PPG_DC_ALPHA * ((float)ir  - dcIR);
                    dcRED += PPG_DC_ALPHA * ((float)red - dcRED);
                    PpgWave w = { (int32_t)lroundf(dcIR - (float)ir), (int32_t)lroundf(dcRED - (float)red) };
                    xQueueSend(queueWavePPG, &w, 0);
                } else {
                    dcInit = false;
                }

                // decimacao por media (anti-aliasing) -> 25 Hz para o algoritmo
                accIR += ir;
                accRED += red;
                if (++accN < PPG_DECIM) continue;

                uint32_t irD = accIR / accN;
                uint32_t redD = accRED / accN;
                accIR = accRED = 0;
                accN = 0;

                if (full < PPG_BUFFER_SIZE) {
                    bufferIR[full] = irD;
                    bufferRED[full] = redD;
                    full++;
                } else {
                    // janela deslizante, remove a sample mais antiga
                    memmove(bufferIR, bufferIR + 1, (PPG_BUFFER_SIZE - 1) * sizeof(uint32_t));
                    memmove(bufferRED, bufferRED + 1, (PPG_BUFFER_SIZE - 1) * sizeof(uint32_t));

                    bufferIR[PPG_BUFFER_SIZE - 1] = irD;
                    bufferRED[PPG_BUFFER_SIZE - 1] = redD;
                }

                news++;
            }
            xSemaphoreGive(mutexI2C);
        }

        // atualiza a presenca do dedo quando ha novas amostras
        if (receivedSamples) {
            fingerPresent = lastIR > LIMIAR_DEDO_IR;
        }

        // se o sensor parar de produzir amostras, invalida os dados depois de um periodo sem atualizacoes
        if (lastSamplesMs == 0 || (uint32_t)(millis() - lastSamplesMs) > 1000) {
            fingerPresent = false;
        }

        if (!fingerPresent) {
            full = 0;
            news = 0;
            accIR = accRED = 0;
            accN = 0;
            bufferAnalised = false;
            invalidatePPG(false);

        } else {
            if (receivedSamples) {
                if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
                    vital.fingerPresent = true;
                    xSemaphoreGive(mutexVital);
                }
            }

            // analisa o primeiro buffer completo e, depois,
            // somente quando houver pelo menos PPG_PASSO novas amostras
            bool shouldAnalyze = full >= PPG_BUFFER_SIZE && (!bufferAnalised || news >= PPG_PASSO);

            if (shouldAnalyze) {
                int32_t spo2 = 0;
                int32_t bpm = 0;

                int8_t spo2Valid = 0;
                int8_t bpmValido = 0;

                maxim_heart_rate_and_oxygen_saturation(
                    bufferIR,
                    PPG_BUFFER_SIZE,
                    bufferRED,
                    &spo2,
                    &spo2Valid,
                    &bpm,
                    &bpmValido
                );

                bufferAnalised = true;
                news = 0;

                if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
                    vital.fingerPresent = true;
                    vital.spo2 = spo2;
                    vital.spo2Valid = (spo2Valid == 1);
                    vital.bpmPPG = bpm;
                    vital.bpmPPGValid = (bpmValido == 1);
                    xSemaphoreGive(mutexVital);
                }
            }
        }
        vTaskDelayUntil(&previousState, pdMS_TO_TICKS(PPG_PERIODO_MS));
    }
}

// Task 4: temperatura corporal
void taskTemperature(void *pv){
    OneWire oneWire(PIN_ONEWIRE);
    DallasTemperature sensors(&oneWire);
    DeviceAddress addr;

    bool found = false;
    bool hasFiltered = false;
    float filtered = 0.0f;
    uint8_t failures = 0;

    TickType_t previousState = xTaskGetTickCount();

    for (;;) {
        if(!found) {
            sensors.begin();
            found = sensors.getAddress(addr, 0);

            if(!found) {
                updateTemperature(0.0f, false);
                hasFiltered = false;
                vTaskDelay(pdMS_TO_TICKS(2000));
                previousState = xTaskGetTickCount();
                continue;
            }

            sensors.setResolution(addr, TEMP_RESOLUTION_BITS);
            sensors.setWaitForConversion(false);
            failures = 0;
        }

        sensors.requestTemperaturesByAddress(addr);
        vTaskDelay(pdMS_TO_TICKS(sensors.millisToWaitForConversion(TEMP_RESOLUTION_BITS) + 50));

        float t = sensors.getTempC(addr);
        bool ok = (t != DEVICE_DISCONNECTED_C) && (t != 85.0f) && (t > -55.0f) && (t < 125.0f);

        if (ok) {
            failures = 0;
            t += TEMP_OFFSET_C;

            if (!hasFiltered) {
                filtered = t;
                hasFiltered = true;
            } else {
                filtered = filtered + TEMP_ALPHA * (t-filtered);
            }

            updateTemperature(filtered, true);
            
        } else if (++failures >= TEMP_ERRORS_MS){
            updateTemperature(0.0f, false);
            hasFiltered = false;
            found = false;
            failures = 0;
        }

        vTaskDelayUntil(&previousState, pdMS_TO_TICKS(TEMP_PERIOD_MS));

    }
}

// Task: envio das formas de onda (ECG/PPG) ao navegador
void taskStream(void *parametro) {
    TickType_t previousState = xTaskGetTickCount();
    for (;;) {
        if (isServerRunning) broadcastWaves();
        vTaskDelayUntil(&previousState, pdMS_TO_TICKS(STREAM_PERIOD_MS));
    }
}

// Task 5: monitor serial

void taskMonitor(void *parametro) {
    TickType_t previousState = xTaskGetTickCount();

    for (;;) {
        VitalSigns v = {};
        if (xSemaphoreTake(mutexVital, pdMS_TO_TICKS(10)) == pdTRUE) {
          v = vital;
          xSemaphoreGive(mutexVital);

        } else {
            // nao utiliza dados potencialmente nao inicializados.
            vTaskDelayUntil(&previousState, pdMS_TO_TICKS(1000));
            continue;
        }

        Serial.println();
        Serial.println("-------- Sinais Vitais --------");
        Serial.printf("Acesse: http://%s  (rede: %s)\n", WiFi.softAPIP().toString().c_str(), ssid);

        if (v.leadoff) {
            Serial.println("ECG: eletrodos desconectados");
        } else if (v.bpmECG > 0.0f) {
            Serial.printf("BPM ECG: %.1f\n", v.bpmECG);
        } else {
            Serial.println("ECG: aguardando deteccao de batimentos");
        }

        if (!v.fingerPresent) {
            Serial.println("PPG: dedo ausente ou sem novas amostras");
        } else {
            if (v.bpmPPGValid) {
                Serial.printf("BPM PPG: %ld\n", (long)v.bpmPPG);
            } else {
                Serial.println("BPM PPG: resultado invalido");
            }

            if (v.spo2Valid) {
                Serial.printf("SpO2: %ld%%\n", (long)v.spo2);
            } else {
                Serial.println("SpO2: resultado invalido");
            }
        }

        if (v.tempValid) {
            Serial.printf("Temperatuta: %.2f C\n", v.tempC);
        } else {
            Serial.println("Temperatura: sensor sem leitura!");
        }

        Serial.println("*********************************");

        if (isServerRunning) broadcastVitalSigns(&v);

        vTaskDelayUntil(&previousState, pdMS_TO_TICKS(1000));
    }
}

// Setup

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(PIN_LO_PLUS, INPUT);
    pinMode(PIN_LO_MINUS, INPUT);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);

    // cria ponto de acesso AP, configura e inicia o WebServer
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    delay(100);
    Serial.printf("Rede WiFi \"%s\" criada com sucesso\n", ssid);
    Serial.printf("Endereço IP do AP: %s\n", WiFi.softAPIP().toString().c_str()); // geralmente 192.168.4.1

    // inicializa o mDNS
    if(MDNS.begin(hostname)) {
        Serial.printf("mDNS iniciado! Acesse: http://%s.local\n", hostname);
        MDNS.addService("http", "tcp", port);
    } else {
        Serial.println("Aviso: Erro ao configurar mDNS.");
    }

    // WebSocket primeiro, depois os arquivos estaticos
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // inicializa a memória Flash e confere se o index.html foi gravado (uploadfs)
    bool fsOk = LittleFS.begin(true);
    bool hasIndex = fsOk && LittleFS.exists("/index.html");
    Serial.printf("LittleFS: %s | /index.html: %s\n", fsOk ? "montado" : "FALHOU", hasIndex ? "encontrado" : "NAO ENCONTRADO");

    if (hasIndex) {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    } else {
        Serial.println(">> Grave o sistema de arquivos: coloque index.html em data/ e rode 'Upload Filesystem Image'.");
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send(200, "text/html; charset=utf-8",
                "<h2>ESP32 online, mas o index.html n&atilde;o est&aacute; no LittleFS.</h2>"
                "<p>Coloque o arquivo em <code>data/index.html</code> e execute "
                "<b>Upload Filesystem Image</b> (PlatformIO: <code>pio run -t uploadfs</code>).</p>");
        });
    }
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Nao encontrado");
    });

    server.begin();
    isServerRunning = true;

    // cria os recursos antes de inicializar as tarefas.
    mutexI2C = xSemaphoreCreateMutex();
    mutexVital = xSemaphoreCreateMutex();

    queueECG = xQueueCreate(256, sizeof(SampleECG));
    queueWaveECG = xQueueCreate(512, sizeof(int16_t));
    queueWavePPG = xQueueCreate(256, sizeof(PpgWave));

    if (mutexI2C == nullptr || mutexVital == nullptr || queueECG == nullptr ||
        queueWaveECG == nullptr || queueWavePPG == nullptr) {

        Serial.println("Falha ao criar recursos FreeRTOS");
        while (true) {
            delay(1000);
        }
    }

    // inicializa o ADS1015.
    if (!ads.begin(ADS_ENDERECO, &Wire)) {
        Serial.println("Coversor a/d ADS1015 nao encontrado");
        while (true) {
            delay(1000);
        }
    }

    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1015_1600SPS);

    // inicializa o MAX30102.
    if (!sensorPPG.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("O modulo MAX30102 nao encontrado!");
        while (true) {
            delay(1000);
        }
    }

    // LED, media, modo LED, taxa de amostragem,
    // largura do pulso e faixa do ADC
    sensorPPG.setup(60, PPG_SAMPLE_AVG, 2, PPG_SAMPLE_RATE_HW, 411, 4096);   // saida = 400/4 = 100 SPS

    Serial.println("Sensores inicializados.");

    // Cria as tarefas e verifica cada retorno
    BaseType_t result;

    result = xTaskCreatePinnedToCore(taskECG, "TaskECG", 4096, nullptr, 3, nullptr, 1);
    if (result != pdPASS) {
        Serial.println("ERRO: nao foi possivel criar taskECG.");
        while (true) {
            delay(1000);
        }
    }

    result = xTaskCreatePinnedToCore(taskProcessECG, "ProcessECG", 4096, nullptr, 2, nullptr, 1);
    if (result != pdPASS) {
        Serial.println("Nao foi possivel criar taskProcessECG!");
        while (true) {
            delay(1000);
        }
    }

    result = xTaskCreatePinnedToCore(taskPPG, "TaskPPG", 6144, nullptr, 2, nullptr, 0);
    if (result != pdPASS) {
        Serial.println("Nao foi possivel criar taskPPG!");
        while (true) {
            delay(1000);
        }
    }

    result = xTaskCreatePinnedToCore(taskMonitor, "Monitor", 4096, nullptr, 1, nullptr, 0);
    if (result != pdPASS) {
        Serial.println("Nao foi possivel criar taskMonitor!");
        while (true) {
            delay(1000);
        }
    }

    result = xTaskCreatePinnedToCore(taskTemperature, "Temperature", 4096, nullptr, 1, nullptr, 0);
    if (result != pdPASS){
        Serial.printf("Não foi possivel criar taskTemperature!");
        while (true) {
            delay(1000);
        }
    }
    result = xTaskCreatePinnedToCore(taskStream, "Stream", 4096, nullptr, 1, nullptr, 0);
    if (result != pdPASS) {
        Serial.println("Nao foi possivel criar taskStream!");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("Tarefas FreeRTOS iniciadas.");
}

// loop principal

void loop() {
    // limpa clientes que se desconectam abruptamente
    if (isServerRunning) ws.cleanupClients();

    vTaskDelay(pdMS_TO_TICKS(2000));
}