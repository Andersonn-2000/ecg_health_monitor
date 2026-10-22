
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

// pinout
#define PIN_SDA       21
#define PIN_SCL       22
#define PIN_LO_PLUS   32
#define PIN_LO_MINUS  33
#define PIN_ONEWIRE    4 // DS18B20

#define ADS_ENDERECO  0x48

// MAX30102
#define PPG_PERIODO_MS 10
#define PPG_BUFFER_SIZE 100
#define PPG_PASSO 25
#define LIMIAR_DEDO_IR 50000UL

// ADS8232
#define ECG_PERIODO_MS 4 // periodo de aquisicao do ECG
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
    int32_t spo2;
    bool spo2Valid;
    int32_t bpmPPG;
    bool bpmPPGValid;
    bool fingerPresent;
    float tempC;
    bool tempValid;
};

// objetos e recursos compartilhados

Adafruit_ADS1015 ads;
MAX30105 sensorPPG;

QueueHandle_t queueECG = nullptr;

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
        baseline = baseline * 0.99f + sample.mV * 0.01f;
        float sinal = sample.mV - baseline;

        // envelope baseado na magnitude do sinal.
        float magnitude = fabsf(sinal);
        if (magnitude > envelope) {
            envelope = magnitude;
        } else {
            envelope *= 0.995f;
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

                if (full < PPG_BUFFER_SIZE) {
                    bufferIR[full] = ir;
                    bufferRED[full] = red;
                    full++;
                } else {
                    // janela deslizante, remove a sample mais antiga
                    memmove(bufferIR, bufferIR + 1, (PPG_BUFFER_SIZE - 1) * sizeof(uint32_t));
                    memmove(bufferRED, bufferRED + 1, (PPG_BUFFER_SIZE - 1) * sizeof(uint32_t));

                    bufferIR[PPG_BUFFER_SIZE - 1] = ir;
                    bufferRED[PPG_BUFFER_SIZE - 1] = red;
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

    // cria os recursos antes de inicializar as tarefas.
    mutexI2C = xSemaphoreCreateMutex();
    mutexVital = xSemaphoreCreateMutex();

    queueECG = xQueueCreate(256, sizeof(SampleECG));

    if (mutexI2C == nullptr || mutexVital == nullptr || queueECG == nullptr) {

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
    sensorPPG.setup(60, 4, 2, 100, 411, 4096);

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
    Serial.println("Tarefas FreeRTOS iniciadas.");
}

// loop principal

void loop() {
    vTaskDelay(portMAX_DELAY);
}
